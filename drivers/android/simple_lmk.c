// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019-2023 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "simple_lmk: " fmt

#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/oom.h>
#include <linux/sched/mm.h>
#include <linux/sort.h>
#include <linux/swap.h>
#include <linux/psi.h>

#include <uapi/linux/sched/types.h>

/* Grace period in milliseconds for newly backgrounded apps */
#define GRACE_PERIOD_MS 5000

/* Kill up to this many victims per reclaim */
#define MAX_VICTIMS 32

/* Deadline in jiffies for reaping a stuck victim before giving up */
#define REAP_RETRY_JIFFIES msecs_to_jiffies(CONFIG_ANDROID_SIMPLE_LMK_TIMEOUT_MSEC)

/* Android oom_score_adj range is 0 to 1000 */
#define ADJ_MAX 1000

/* Idle cycles required before the kill target steps back toward Tier 0 */
#define LMK_RELAX_CYCLES 3

/*
 * PSI stall thresholds, one per kill tier. Full stall means every non-idle task
 * is blocked on memory, so the bound is a latency budget: 100 ms is about
 * twelve dropped frames on a 120 Hz panel, 200 ms is near-OOM. Perception and
 * the display fix these, not RAM size.
 */
#define LMK_PSI_WINDOW_MS 1000
#define LMK_PSI_THRESHOLD_LOW_US 100000
#define LMK_PSI_THRESHOLD_MED_US 150000
#define LMK_PSI_THRESHOLD_HIGH_US 200000

#define LMK_TIERS 3

static const int psi_thresholds[LMK_TIERS] = {
	LMK_PSI_THRESHOLD_LOW_US,
	LMK_PSI_THRESHOLD_MED_US,
	LMK_PSI_THRESHOLD_HIGH_US
};

/*
 * Tier adj ceilings, from the Android priority brackets. A tier is entered
 * when its PSI threshold is crossed; these are the shallowest adj each tier may
 * reach, i.e. the shortest list of processes it will ever consider.
 */
#define LMK_TIER0_MIN_ADJ 800
#define LMK_TIER1_MIN_ADJ 500
#define LMK_TIER2_MIN_ADJ 200

/*
 * Nothing below this adj is ever killed regardless of pressure: it is the
 * foreground/visible band. If RAM runs out under adj < 200 the core OOM killer
 * takes over.
 */
#define LMK_TIER_FLOOR_ADJ 200

static const short tier_min_adj[LMK_TIERS] = {
	LMK_TIER0_MIN_ADJ,
	LMK_TIER1_MIN_ADJ,
	LMK_TIER2_MIN_ADJ
};

struct victim_info {
	struct task_struct *tsk;
	struct mm_struct *mm;
	unsigned long size;
	unsigned long score;
	/* Pages credited against the deficit once the kill is dispatched */
	unsigned long pending;
};

static struct victim_info victims[MAX_VICTIMS] __cacheline_aligned_in_smp;
static struct task_struct *task_bucket[ADJ_MAX + 1] __cacheline_aligned;
static DEFINE_SPINLOCK(victims_lock);
static DECLARE_WAIT_QUEUE_HEAD(oom_waitq);
static DECLARE_WAIT_QUEUE_HEAD(reaper_waitq);
static DECLARE_COMPLETION(psi_init_done);

/* Ceiling the current cycle may reach, set by PSI or the OOM notifier */
static atomic_t target_min_adj = ATOMIC_INIT(tier_min_adj[0]);
static atomic_t needs_reclaim = ATOMIC_INIT(0);
static atomic_t needs_reap = ATOMIC_INIT(0);
static atomic_t oom_attempts = ATOMIC_INIT(0);

static int nr_victims;
static bool reclaim_active;

/*
 * Pages belonging to killed victims whose memory has not landed yet. Summed
 * from the victims array rather than counted: a victim stops pending through
 * any of three racing paths (__oom_reap_task_mm, exit_mmap, the force-give-up
 * in next_reap_victim), and summing current state cannot double-count or leak
 * the way an event-driven counter can.
 */
static unsigned long pages_pending_free(void)
{
	unsigned long total = 0;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&victims_lock, flags);
	for (i = 0; i < READ_ONCE(nr_victims); i++) {
		struct mm_struct *mm = victims[i].mm;

		if (!mm || test_bit(MMF_OOM_SKIP, &mm->flags))
			continue;

		total += victims[i].pending;
	}
	spin_unlock_irqrestore(&victims_lock, flags);

	return total;
}

/*
 * How far below the free-page reserve we are, less the pages already committed
 * to earlier kills. Without that subtraction the same deficit is charged every
 * cycle until the memory lands, and the driver keeps escalating past what the
 * deficit needed.
 */
static unsigned long get_target_free_pages(void)
{
	unsigned long deficit, pending;

	if (nr_free_pages() >= totalreserve_pages)
		return 0;

	deficit = totalreserve_pages - nr_free_pages();
	deficit += (deficit >> 3); /* 12.5% margin */

	pending = pages_pending_free();
	if (pending >= deficit)
		return 0;

	return deficit - pending;
}

/*
 * Fraction of the last interval, in percent, spent fully stalled on memory.
 * Sample the PSI poll counter and the clock each cycle; the growth over the
 * wall time is the same stall PSI compares against a trigger threshold, so it
 * measures how hard reclaim is actually losing rather than just that a
 * threshold fired.
 */
static u64 stall_ns_last;
static unsigned long stall_jiffies_last;

static unsigned long measure_stall_pct(unsigned long interval_jiffies)
{
	u64 now_ns = psi_system.total[PSI_POLL][PSI_MEM_FULL];
	u64 delta_ns;
	u64 window_ns;

	if (!interval_jiffies)
		return 0;

	delta_ns = now_ns - stall_ns_last;
	stall_ns_last = now_ns;

	window_ns = (u64)interval_jiffies * NSEC_PER_SEC / HZ;
	if (delta_ns >= window_ns)
		return 100;

	return div_u64(delta_ns * 100, window_ns);
}

static short tier_of_ceil(short ceil_adj)
{
	int i;

	for (i = 0; i < LMK_TIERS; i++) {
		if (tier_min_adj[i] == ceil_adj)
			return i;
	}

	return LMK_TIERS - 1;
}

/*
 * Lowest adj a tier may reach given the measured stall. The tier's own
 * threshold is the reference: at it the tier is at exactly the pressure it was
 * defined for and stays near its ceiling, and each further multiple of it lets
 * the tier descend toward LMK_TIER_FLOOR_ADJ. Both the reference and the span
 * come from values already fixed above, so there is nothing to tune.
 */
static short tier_reach_adj(short tier_index, unsigned long stall_pct)
{
	unsigned long target_duty, span;

	if (tier_index < 0 || tier_index >= LMK_TIERS)
		tier_index = LMK_TIERS - 1;

	if (tier_min_adj[tier_index] <= LMK_TIER_FLOOR_ADJ)
		return LMK_TIER_FLOOR_ADJ;

	/* The tier threshold as a percentage of the sampling window */
	target_duty = (unsigned long)psi_thresholds[tier_index] * 100 /
		      (LMK_PSI_WINDOW_MS * 1000);
	if (!target_duty)
		target_duty = 1;

	if (stall_pct >= target_duty)
		return LMK_TIER_FLOOR_ADJ;

	span = tier_min_adj[tier_index] - LMK_TIER_FLOOR_ADJ;
	return tier_min_adj[tier_index] -
	       (short)(span * stall_pct / target_duty);
}

/*
 * Raise the target one tier after LMK_RELAX_CYCLES idle cycles. target_min_adj
 * is only ever lowered elsewhere, so without this one PSI spike or uncaught OOM
 * would keep the driver killing to adj 200 for the rest of uptime -- and keep
 * the Tier 0 grace period disabled, since it is gated on the target. A cycle
 * with any deficit left resets the count.
 */
static void relax_min_adj(void)
{
	static unsigned int idle_cycles;
	short cur = atomic_read(&target_min_adj);
	int i;

	if (get_target_free_pages() != 0) {
		idle_cycles = 0;
		return;
	}

	if (++idle_cycles < LMK_RELAX_CYCLES)
		return;

	idle_cycles = 0;

	for (i = 0; i < LMK_TIERS; i++) {
		if (tier_min_adj[i] > cur) {
			atomic_set(&target_min_adj, tier_min_adj[i]);
			break;
		}
	}
}

static int victim_cmp(const void *lhs_ptr, const void *rhs_ptr)
{
	const struct victim_info *lhs = (typeof(lhs))lhs_ptr;
	const struct victim_info *rhs = (typeof(rhs))rhs_ptr;

	return rhs->score - lhs->score;
}

static int victim_cmp_size(const void *lhs_ptr, const void *rhs_ptr)
{
	const struct victim_info *lhs = (typeof(lhs))lhs_ptr;
	const struct victim_info *rhs = (typeof(rhs))rhs_ptr;

	return rhs->size - lhs->size;
}

static void victim_swap(void *lhs_ptr, void *rhs_ptr, int size)
{
	struct victim_info *lhs = (typeof(lhs))lhs_ptr;
	struct victim_info *rhs = (typeof(rhs))rhs_ptr;

	swap(*lhs, *rhs);
}

/*
 * Pages only a kill would release: anonymous memory, tmpfs, and the swap slots
 * their entries occupy. tmpfs counts because exit_mmap() frees it, and it is
 * charged to MM_SHMEMPAGES rather than MM_ANONPAGES. File pages are left out
 * because reclaim frees those without killing anything.
 */
static unsigned long get_reclaimable_pages(struct mm_struct *mm)
{
	return get_mm_counter(mm, MM_ANONPAGES) +
	       get_mm_counter(mm, MM_SHMEMPAGES) +
	       get_mm_counter(mm, MM_SWAPENTS);
}

/*
 * Fill the victims array with the least important killable tasks, hand back the
 * deficit they were selected against, and return their count. The return value
 * is not used by the caller, so this reports through *vindex.
 */
static void find_victims(int *vindex, unsigned long *target_out,
			 unsigned long stall_pct)
{
	short i, min_adj = ADJ_MAX, max_adj = 0;
	short tier_ceil = atomic_read(&target_min_adj);
	short limit_adj = tier_reach_adj(tier_of_ceil(tier_ceil), stall_pct);
	unsigned long pages_found = 0;
	unsigned long target_pages = get_target_free_pages();
	struct task_struct *tsk;

	*target_out = target_pages;

	/*
	 * Walk the process list under RCU and pin every candidate, so the
	 * bucket chains stay valid after RCU is dropped. Zero and negative adjs
	 * are excluded, which naturally skips init and kthreads.
	 */
	rcu_read_lock();
	for_each_process(tsk) {
		struct signal_struct *sig;
		short adj;

		sig = tsk->signal;
		adj = READ_ONCE(sig->oom_score_adj);
		if (adj < limit_adj || adj > ADJ_MAX ||
		    sig->flags & SIGNAL_GROUP_EXIT ||
		    (thread_group_empty(tsk) && tsk->flags & PF_EXITING))
			continue;

		get_task_struct(tsk);
		tsk->simple_lmk_next = task_bucket[adj];
		task_bucket[adj] = tsk;

		if (adj > max_adj)
			max_adj = adj;
		if (adj < min_adj)
			min_adj = adj;
	}
	rcu_read_unlock();

	/* Evaluate candidates from most to least important adj */
	for (i = max_adj; i >= min_adj; i--) {
		int old_vindex;
		struct task_struct *next;

		tsk = task_bucket[i];
		if (!tsk)
			continue;

		task_bucket[i] = NULL;

		old_vindex = *vindex;
		do {
			struct task_struct *vtsk;
			struct mm_struct *vmm;
			unsigned long pages = 0;
			int vi;

			next = tsk->simple_lmk_next;

			/*
			 * 5 s grace for apps that just entered the cached tier,
			 * and only at Tier 0. Gate on the selected tier so the
			 * protection does not depend on the measured stall.
			 */
			if (tier_ceil == tier_min_adj[0] &&
			    time_before(jiffies, tsk->simple_lmk_cache_time + msecs_to_jiffies(GRACE_PERIOD_MS)))
				goto drop_ref;

			rcu_read_lock();
			vtsk = find_lock_task_mm(tsk);
			if (!vtsk || !vtsk->mm) {
				if (vtsk)
					task_unlock(vtsk);
				rcu_read_unlock();
				goto drop_ref;
			}

			pages = get_reclaimable_pages(vtsk->mm);
			if (!pages) {
				task_unlock(vtsk);
				rcu_read_unlock();
				goto drop_ref;
			}

			/*
			 * Take the mm and its reference under task_lock, so the
			 * slot records the same object the reference was taken
			 * on. Reading vtsk->mm again after the unlock could grab
			 * a cleared mm while the slot stores the old one.
			 */
			vmm = vtsk->mm;
			get_task_struct(vtsk);
			mmgrab(vmm);
			task_unlock(vtsk);
			rcu_read_unlock();

			/*
			 * An mm shared by several thread groups is reachable
			 * from several leaders. simple_lmk_mm_freed() releases
			 * it through a single slot, so keep only the first.
			 */
			for (vi = 0; vi < *vindex; vi++) {
				if (victims[vi].mm == vmm)
					break;
			}
			if (vi < *vindex) {
				mmdrop(vmm);
				put_task_struct(vtsk);
				goto drop_ref;
			}

			victims[*vindex].tsk = vtsk;
			victims[*vindex].mm = vmm;
			victims[*vindex].size = pages;
			victims[*vindex].pending = 0;

			pages_found += pages;

			if (++*vindex == MAX_VICTIMS) {
				put_task_struct(tsk);
				/* Drain the rest of this bucket's chain */
				while (next) {
					tsk = next;
					next = tsk->simple_lmk_next;
					put_task_struct(tsk);
				}
				goto drain_remaining;
			}
drop_ref:
			put_task_struct(tsk);
		} while ((tsk = next));

		if (*vindex == old_vindex)
			continue;

		if (*vindex == MAX_VICTIMS || pages_found >= target_pages)
			break;
	}

drain_remaining:
	/* Release candidates in buckets we stopped before reaching */
	for (i = min_adj; i <= max_adj; i++) {
		tsk = task_bucket[i];
		task_bucket[i] = NULL;
		while (tsk) {
			struct task_struct *next = tsk->simple_lmk_next;
			put_task_struct(tsk);
			tsk = next;
		}
	}
}

/*
 * Keep only as many victims as the deficit needs, releasing the rest. Making
 * the kill count agree with find_victims() means both use one target snapshot.
 */
static int process_victims(int vlen, unsigned long target_pages)
{
	unsigned long pages_found = 0;
	int i, nr_to_kill = 0;

	for (i = 0; i < vlen; i++) {
		struct victim_info *victim = &victims[i];
		struct task_struct *vtsk = victim->tsk;

		if (pages_found >= target_pages) {
			if (victim->mm)
				mmdrop(victim->mm);
			put_task_struct(vtsk);
			victim->mm = NULL;
			victim->tsk = NULL;
		} else {
			pages_found += victim->size;
			nr_to_kill++;
		}
	}

	return nr_to_kill;
}

static void set_task_rt_prio(struct task_struct *tsk, int priority)
{
	const struct sched_param rt_prio = {
		.sched_priority = priority
	};

	sched_setscheduler_nocheck(tsk, SCHED_RR, &rt_prio);
}

static void scan_and_kill(void)
{
	static struct mm_struct *drop_mms[MAX_VICTIMS];
	int i, nr_to_kill, nr_found = 0;
	unsigned long target_pages = 0;
	unsigned long stall_pct;
	unsigned long flags;
	int num_drop;

	/* The reaper still owns the array; PSI will re-fire if pressure holds */
	if (READ_ONCE(reclaim_active))
		return;

	relax_min_adj();

	stall_pct = measure_stall_pct(jiffies - stall_jiffies_last);
	stall_jiffies_last = jiffies;

	/*
	 * A successfully reaped victim keeps its slot until its task exits,
	 * because simple_lmk_mm_freed() is the only thing that releases its
	 * reference. find_victims() writes slots unconditionally, so sweep the
	 * old ones here or the reference is lost and the mm leaks. Under
	 * victims_lock so simple_lmk_mm_freed() cannot race.
	 */
	num_drop = 0;
	spin_lock_irqsave(&victims_lock, flags);
	WRITE_ONCE(nr_victims, 0);
	for (i = 0; i < MAX_VICTIMS; i++) {
		struct mm_struct *mm = victims[i].mm;

		victims[i].mm = NULL;
		if (mm)
			drop_mms[num_drop++] = mm;
	}
	spin_unlock_irqrestore(&victims_lock, flags);

	for (i = 0; i < num_drop; i++)
		mmdrop(drop_mms[i]);

	find_victims(&nr_found, &target_pages, stall_pct);
	if (unlikely(!nr_found))
		return;

	pr_info("stall %lu%%, tier %d, reached adj %d, deficit %lu pages, %d candidate(s)\n",
		stall_pct, tier_of_ceil(atomic_read(&target_min_adj)),
		tier_reach_adj(tier_of_ceil(atomic_read(&target_min_adj)),
			       stall_pct),
		target_pages, nr_found);

	/* Kill the largest first, then stop once the target is met */
	sort(victims, nr_found, sizeof(*victims), victim_cmp_size, victim_swap);
	nr_to_kill = process_victims(nr_found, target_pages);

	num_drop = 0;
	spin_lock_irqsave(&victims_lock, flags);
	WRITE_ONCE(nr_victims, nr_to_kill);
	WRITE_ONCE(reclaim_active, true);
	for (i = 0; i < nr_to_kill; i++) {
		struct mm_struct *mm = victims[i].mm;

		if (mm && test_bit(MMF_OOM_SKIP, &mm->flags)) {
			victims[i].mm = NULL;
			drop_mms[num_drop++] = mm;
		}
	}
	spin_unlock_irqrestore(&victims_lock, flags);

	for (i = 0; i < num_drop; i++)
		mmdrop(drop_mms[i]);

	for (i = 0; i < nr_to_kill; i++) {
		struct victim_info *victim = &victims[i];
		struct task_struct *t, *vtsk = victim->tsk;
		struct mm_struct *mm = victim->mm;

		/* Released above: its memory was already gone, so nothing to kill */
		if (!mm) {
			victim->score = 0;
			victim->pending = 0;
			put_task_struct(vtsk);
			victim->tsk = NULL;
			continue;
		}

		pr_info("Killing %s with adj %d to free %lu KiB\n", vtsk->comm,
			vtsk->signal->oom_score_adj,
			victim->size << (PAGE_SHIFT - 10));

		/* Thaw first: a frozen task cannot process the kill signal */
		rcu_read_lock();
		for_each_thread(vtsk, t) {
			if (frozen(t))
				__thaw_task(t);
		}
		rcu_read_unlock();

		do_send_sig_info(SIGKILL, SEND_SIG_PRIV, vtsk, PIDTYPE_TGID);

		set_bit(MMF_SIMPLE_LMK_VICTIM, &mm->flags);

		WRITE_ONCE(vtsk->signal->oom_score_adj, OOM_SCORE_ADJ_MIN);

		/*
		 * Mark the group dead so the allocator gives it emergency
		 * memory priority; otherwise victims stall during exit.
		 */
		rcu_read_lock();
		for_each_thread(vtsk, t)
			set_tsk_thread_flag(t, TIF_MEMDIE);
		rcu_read_unlock();

		set_cpus_allowed_ptr(vtsk, cpu_possible_mask);

		/*
		 * Credit what the kill should release, using the same
		 * measurement as get_reclaimable_pages(), refreshed because the
		 * task has been running since selection.
		 */
		victim->score = get_reclaimable_pages(mm);
		victim->size = victim->score;
		victim->pending = victim->score;

		put_task_struct(vtsk);
		victim->tsk = NULL;
	}

	/* Reap the biggest victims first; the reaper takes it from here */
	spin_lock_irqsave(&victims_lock, flags);
	sort(victims, nr_to_kill, sizeof(*victims), victim_cmp, victim_swap);
	spin_unlock_irqrestore(&victims_lock, flags);
	smp_wmb();
	atomic_set(&needs_reap, 1);
	atomic_set(&oom_attempts, 0);
	wake_up(&reaper_waitq);
}

static int simple_lmk_reclaim_thread(void *data)
{
	set_task_rt_prio(current, MAX_RT_PRIO - 1);
	set_freezable();

	while (!kthread_should_stop()) {
		wait_event_freezable(oom_waitq,
				     (atomic_read(&needs_reclaim) &&
				      !READ_ONCE(reclaim_active)) ||
				     kthread_should_stop());
		if (kthread_should_stop())
			break;
		/* Cleared first so an escalation during the scan is not lost */
		atomic_set(&needs_reclaim, 0);
		scan_and_kill();
	}

	return 0;
}

/*
 * Hand the reaper the next victim mm, or ERR_PTR(-EAGAIN) if one is busy, or
 * NULL when nothing is left. Inspected under victims_lock so exit_mmap() cannot
 * free an mm mid-scan.
 */
static struct mm_struct *next_reap_victim(bool force)
{
	struct mm_struct *mm = NULL;
	unsigned long flags;
	bool should_retry = false;
	int i;

	for (i = 0; i < READ_ONCE(nr_victims); i++, mm = NULL) {
		spin_lock_irqsave(&victims_lock, flags);
		mm = victims[i].mm;
		if (!mm || test_bit(MMF_OOM_SKIP, &mm->flags)) {
			spin_unlock_irqrestore(&victims_lock, flags);
			continue;
		}

		/*
		 * trylock, so the reaper never sleeps. On a failed trylock past
		 * the deadline, give up on this victim: it already has SIGKILL
		 * and TIF_MEMDIE, so exit_mmap() will free it without us.
		 */
		if (!mmap_read_trylock(mm)) {
			if (force) {
				struct mm_struct *drop_mm = NULL;

				set_bit(MMF_OOM_SKIP, &mm->flags);
				if (victims[i].mm == mm) {
					victims[i].mm = NULL;
					drop_mm = mm;
				}
				spin_unlock_irqrestore(&victims_lock, flags);
				if (drop_mm)
					mmdrop(drop_mm);
			} else {
				spin_unlock_irqrestore(&victims_lock, flags);
				should_retry = true;
			}
			continue;
		}

		/*
		 * Re-check under mmap_read_lock: exit_mmap() is serialized on
		 * mmap_write_lock, so the address space is stable here.
		 */
		if (!test_bit(MMF_OOM_SKIP, &mm->flags)) {
			spin_unlock_irqrestore(&victims_lock, flags);
			break;
		}

		mmap_read_unlock(mm);
		spin_unlock_irqrestore(&victims_lock, flags);
	}

	if (!mm) {
		if (should_retry) {
			mm = ERR_PTR(-EAGAIN);
		} else {
			/*
			 * Done. Clearing reclaim_active lets
			 * simple_lmk_mm_freed() and scan_and_kill() move on; the
			 * barrier orders the cleared slots before it. Keeping
			 * this in the else is load-bearing -- clearing it while
			 * returning -EAGAIN would let a new scan overwrite the
			 * array being walked.
			 */
			smp_mb();
			WRITE_ONCE(reclaim_active, false);
			/* Order the clear before sampling needs_reclaim */
			smp_mb();
			if (atomic_read(&needs_reclaim))
				wake_up(&oom_waitq);
		}
	}

	return mm;
}

static void reap_victims(void)
{
	struct mm_struct *mm;
	unsigned long retry_deadline = 0;
	bool force = false;

	while ((mm = next_reap_victim(force))) {
		if (IS_ERR(mm)) {
			/* A busy mm: retry until the deadline, then force past it */
			if (!retry_deadline) {
				retry_deadline = jiffies + REAP_RETRY_JIFFIES;
			} else if (time_after(jiffies, retry_deadline)) {
				force = true;
				retry_deadline = 0;
			}
			schedule_timeout_uninterruptible(1);
			continue;
		}

		/*
		 * Reap it. A failure (e.g. a driver's non-blocking MMU notifier
		 * returned -EAGAIN) is retried until the deadline, then given up
		 * so the reaper does not spin at RT priority.
		 */
		if (__oom_reap_task_mm(mm)) {
			set_bit(MMF_OOM_SKIP, &mm->flags);
			retry_deadline = 0;
			force = false;
		} else {
			if (!retry_deadline) {
				retry_deadline = jiffies + REAP_RETRY_JIFFIES;
			} else if (time_after(jiffies, retry_deadline)) {
				set_bit(MMF_OOM_SKIP, &mm->flags);
				retry_deadline = 0;
				force = false;
			}
			schedule_timeout_uninterruptible(1);
		}
		mmap_read_unlock(mm);

		cond_resched();
	}
}

static int simple_lmk_reaper_thread(void *data)
{
	set_task_rt_prio(current, MAX_RT_PRIO - 2);
	set_freezable();

	while (!kthread_should_stop()) {
		wait_event_freezable(reaper_waitq,
				     atomic_read(&needs_reap) ||
				     kthread_should_stop());
		if (kthread_should_stop())
			break;
		atomic_set(&needs_reap, 0);
		reap_victims();
	}

	return 0;
}

void simple_lmk_mm_freed(struct mm_struct *mm)
{
	unsigned long flags;
	int i;
	bool matched = false;

	/* Only reaped victims carry MMF_OOM_SKIP here; ignore other dying mms */
	if (!test_bit(MMF_OOM_SKIP, &mm->flags) ||
	    !test_bit(MMF_SIMPLE_LMK_VICTIM, &mm->flags))
		return;

	/*
	 * This runs from __mmput() after exit_mmap(), and for a victim that was
	 * reaped but has not exited yet it is the only place our mmgrab()
	 * reference is dropped. Searched even when reclaim is inactive, or that
	 * reference would never be released and the mm would leak.
	 */
	spin_lock_irqsave(&victims_lock, flags);
	for (i = 0; i < READ_ONCE(nr_victims); i++) {
		if (victims[i].mm == mm) {
			victims[i].mm = NULL;
			matched = true;
		}
	}
	spin_unlock_irqrestore(&victims_lock, flags);

	if (matched)
		mmdrop(mm);
}

static struct psi_trigger *psi_triggers[LMK_TIERS];
static DECLARE_WAIT_QUEUE_HEAD(psi_waitq);

static int simple_lmk_psi_thread(void *data)
{
	set_task_rt_prio(current, MAX_RT_PRIO - 3);
	set_freezable();

	wait_for_completion(&psi_init_done);

	while (!kthread_should_stop()) {
		short min_adj = ADJ_MAX;
		bool high, med, low;

		wait_event_freezable(psi_waitq,
				     READ_ONCE(psi_triggers[0]->event) ||
				     READ_ONCE(psi_triggers[1]->event) ||
				     READ_ONCE(psi_triggers[2]->event));

		/*
		 * Sample and clear all three. A severe event also leaves the
		 * milder triggers set, so clearing only the highest would wake
		 * this thread again immediately for spurious lower-tier cycles.
		 */
		high = cmpxchg(&psi_triggers[2]->event, 1, 0);
		med  = cmpxchg(&psi_triggers[1]->event, 1, 0);
		low  = cmpxchg(&psi_triggers[0]->event, 1, 0);

		if (high)
			min_adj = tier_min_adj[2];
		else if (med)
			min_adj = tier_min_adj[1];
		else if (low)
			min_adj = tier_min_adj[0];

		if (min_adj == ADJ_MAX)
			continue;

		/* Record a more severe tier; otherwise only when idle */
		if (READ_ONCE(reclaim_active) &&
		    min_adj >= atomic_read(&target_min_adj))
			continue;

		atomic_set(&target_min_adj, min_adj);
		atomic_set(&oom_attempts, 0);
		atomic_set(&needs_reclaim, 1);
		/*
		 * Order the store above before reading reclaim_active, so the
		 * reaper cannot clear it and miss the flag in the same window.
		 */
		smp_mb();
		if (!READ_ONCE(reclaim_active))
			wake_up(&oom_waitq);
	}

	return 0;
}

/*
 * Stamp cache_time only on entry to the cached tier. This runs on every adj
 * write, and ActivityManager rewrites cached adjs often, so stamping
 * unconditionally would make the Tier 0 grace period re-arm forever and keep
 * the app unkillable. Called before the new adj is assigned.
 */
void simple_lmk_update_adj(struct task_struct *task, int new_adj)
{
	if (new_adj >= tier_min_adj[0] &&
	    task->signal->oom_score_adj < tier_min_adj[0])
		task->simple_lmk_cache_time = jiffies;
}
EXPORT_SYMBOL_GPL(simple_lmk_update_adj);

static int simple_lmk_oom_notify(struct notifier_block *self,
				 unsigned long val, void *data)
{
	unsigned long *freed = data;

	/* Earlier kills are already freeing memory: let the allocator retry */
	if (pages_pending_free() > 0) {
		*freed = 1;
		atomic_set(&oom_attempts, 0);
		return NOTIFY_OK;
	}

	/*
	 * An OOM PSI did not catch. Try once at the most aggressive tier; if it
	 * comes back without pending pages there was nothing for us to kill, so
	 * hand the runaway consumer to the core OOM killer instead of
	 * suppressing it forever.
	 */
	if (atomic_inc_return(&oom_attempts) == 1) {
		atomic_set(&target_min_adj, tier_min_adj[2]);
		atomic_set(&needs_reclaim, 1);
		/* Order the store above before reading reclaim_active */
		smp_mb();
		if (!READ_ONCE(reclaim_active))
			wake_up(&oom_waitq);
		*freed = 1;
	} else {
		atomic_set(&oom_attempts, 0);
	}

	return NOTIFY_OK;
}

static struct notifier_block simple_lmk_oom_nb = {
	.notifier_call = simple_lmk_oom_notify,
};

/* Initialize Simple LMK when lmkd in Android writes to the minfree parameter */
static int simple_lmk_init_set(const char *val, const struct kernel_param *kp)
{
	static atomic_t init_done = ATOMIC_INIT(0);
	struct task_struct *reaper_thread = NULL;
	struct task_struct *reclaim_thread = NULL;
	struct task_struct *psi_thread = NULL;
	int i, ret = 0;

	if (!atomic_cmpxchg(&init_done, 0, 1)) {
		reaper_thread = kthread_run(simple_lmk_reaper_thread, NULL,
					    "simple_lmkd_reaper");
		if (IS_ERR(reaper_thread)) {
			ret = PTR_ERR(reaper_thread);
			reaper_thread = NULL;
			goto fail;
		}

		reclaim_thread = kthread_run(simple_lmk_reclaim_thread, NULL,
					     "simple_lmkd");
		if (IS_ERR(reclaim_thread)) {
			ret = PTR_ERR(reclaim_thread);
			reclaim_thread = NULL;
			goto fail;
		}

		for (i = 0; i < LMK_TIERS; i++) {
			char buf[64];

			snprintf(buf, sizeof(buf), "full %d %d", psi_thresholds[i],
				 LMK_PSI_WINDOW_MS * 1000);
			psi_triggers[i] = psi_trigger_create(&psi_system, buf,
							     PSI_MEM, NULL, NULL);
			if (IS_ERR(psi_triggers[i])) {
				ret = PTR_ERR(psi_triggers[i]);
				psi_triggers[i] = NULL;
				goto fail;
			}
			psi_trigger_set_waitq(psi_triggers[i], &psi_waitq);
		}

		psi_thread = kthread_run(simple_lmk_psi_thread, NULL,
					 "simple_lmkd_psi");
		if (IS_ERR(psi_thread)) {
			ret = PTR_ERR(psi_thread);
			psi_thread = NULL;
			goto fail;
		}

		ret = register_oom_notifier(&simple_lmk_oom_nb);
		if (ret)
			goto fail;

		complete(&psi_init_done);
	}

	return 0;

fail:
	/*
	 * Roll back so lmkd can retry on a later write. The psi thread blocks
	 * on psi_init_done, which the success path posts, so post it here too or
	 * a failure past its creation leaves kthread_stop() waiting forever.
	 */
	complete(&psi_init_done);
	if (psi_thread)
		kthread_stop(psi_thread);
	for (i = 0; i < LMK_TIERS; i++) {
		if (psi_triggers[i]) {
			psi_trigger_destroy(psi_triggers[i]);
			psi_triggers[i] = NULL;
		}
	}
	if (reclaim_thread)
		kthread_stop(reclaim_thread);
	if (reaper_thread)
		kthread_stop(reaper_thread);
	atomic_set(&init_done, 0);
	return ret;
}

static const struct kernel_param_ops simple_lmk_init_ops = {
	.set = simple_lmk_init_set
};

/* Needed to prevent Android from thinking there's no LMK and thus rebooting */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "lowmemorykiller."
module_param_cb(minfree, &simple_lmk_init_ops, NULL, 0200);
