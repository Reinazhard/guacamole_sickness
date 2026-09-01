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

struct victim_info {
	struct task_struct *tsk;
	struct mm_struct *mm;
	unsigned long size;
	unsigned long score;
	/*
	 * Resident anonymous pages credited against the memory deficit for
	 * this victim. Zero until the kill is actually dispatched, so that
	 * candidates that were selected but never killed are not counted as
	 * memory already on its way to being freed.
	 */
	unsigned long pending;
};

static struct victim_info victims[MAX_VICTIMS] __cacheline_aligned_in_smp;
static struct task_struct *task_bucket[ADJ_MAX + 1] __cacheline_aligned;
static DECLARE_WAIT_QUEUE_HEAD(oom_waitq);
static DECLARE_WAIT_QUEUE_HEAD(reaper_waitq);
static DECLARE_COMPLETION(psi_init_done);

static int nr_victims;
static bool reclaim_active;

#define LMK_TIERS 3
static const short tier_min_adj[LMK_TIERS] = { 800, 200, 1 };

static atomic_t needs_reclaim = ATOMIC_INIT(0);
static atomic_t needs_reap = ATOMIC_INIT(0);
static atomic_t target_min_adj = ATOMIC_INIT(tier_min_adj[0]);

/*
 * Anonymous pages belonging to victims that have been killed but whose memory
 * has not yet been reaped or released by exit. nr_free_pages() still counts
 * these against us, even though they are already committed to being freed.
 *
 * Derived by summing the victims array rather than maintained as a counter:
 * a victim's memory can stop pending by any of three paths -- a successful
 * __oom_reap_task_mm(), exit_mmap() via simple_lmk_mm_freed(), or the
 * force-give-up path in next_reap_victim() -- and any of them can race.
 * Summing current state cannot double-subtract or leak the way an
 * event-driven counter can.
 *
 * This is the same invariant Sultan's synchronous design obtained by waiting
 * for each victim's memory to be freed before proceeding to kill more,
 * expressed as accounting so the kill path stays asynchronous and needs no
 * artificial cooldown.
 */
static unsigned long pages_pending_free(void)
{
	unsigned long total = 0;
	int i;

	for (i = 0; i < READ_ONCE(nr_victims); i++) {
		struct mm_struct *mm = READ_ONCE(victims[i].mm);

		/* No victim, or its memory has already been accounted as free */
		if (!mm || test_bit(MMF_OOM_SKIP, &mm->flags))
			continue;

		total += victims[i].pending;
	}

	return total;
}

static unsigned long get_target_free_pages(void)
{
	unsigned long deficit, pending;

	if (nr_free_pages() >= totalreserve_pages)
		return 0;

	deficit = totalreserve_pages - nr_free_pages();
	deficit += (deficit >> 3); /* 12.5% margin */

	/*
	 * Do not charge this cycle for memory that earlier kills have already
	 * committed to freeing. Without this, the deficit is charged for the
	 * same pages on every cycle until the victim's memory actually lands:
	 * a cycle then sees a deficit that is already being resolved, finds no
	 * victims left at the current tier, and escalates -- which is how the
	 * driver reaches the most aggressive tier and kills far more than the
	 * deficit ever required.
	 */
	pending = pages_pending_free();
	if (pending >= deficit)
		return 0;

	return deficit - pending;
}

static int victim_cmp(const void *lhs_ptr, const void *rhs_ptr)
{
	const struct victim_info *lhs = (typeof(lhs))lhs_ptr;
	const struct victim_info *rhs = (typeof(rhs))rhs_ptr;

	if (rhs->score > lhs->score)
		return 1;
	if (rhs->score < lhs->score)
		return -1;
	return 0;
}

static int victim_cmp_size(const void *lhs_ptr, const void *rhs_ptr)
{
	const struct victim_info *lhs = (typeof(lhs))lhs_ptr;
	const struct victim_info *rhs = (typeof(rhs))rhs_ptr;

	if (rhs->size > lhs->size)
		return 1;
	if (rhs->size < lhs->size)
		return -1;
	return 0;
}

static void victim_swap(void *lhs_ptr, void *rhs_ptr, int size)
{
	struct victim_info *lhs = (typeof(lhs))lhs_ptr;
	struct victim_info *rhs = (typeof(rhs))rhs_ptr;

	swap(*lhs, *rhs);
}

/*
 * Pages a task is holding that only killing it would release: resident
 * anonymous memory, plus the swap slots its entries occupy. File pages are
 * left out because the kernel can drop those without killing anything.
 *
 * Freeing the swap slots matters here specifically: this device runs zram
 * near capacity, so reclaim cannot push further anon pages out until some
 * slots are released.
 *
 * No attempt is made to weight these pages by how cold they are. Android
 * already orders the cached tiers by recency, so oom_score_adj carries that
 * information and find_victims() selects on it. Re-deriving coldness here
 * from how long a task has been backgrounded double-counts the same signal,
 * needs tunables to describe, and is not checkable -- MGLRU generations and
 * workingset are authoritative, and a killer should not compete with them.
 *
 * This is also what makes the deficit arithmetic sound. get_target_free_pages()
 * credits kills with the pages actually pending free, so victim sizes have to
 * mean real pages; a discounted estimate would understate the credit and
 * drive the driver to kill again.
 */
static unsigned long get_reclaimable_pages(struct mm_struct *mm)
{
	return get_mm_counter(mm, MM_ANONPAGES) + get_mm_counter(mm, MM_SWAPENTS);
}

static unsigned long find_victims(int *vindex)
{
	short i, min_adj = ADJ_MAX, max_adj = 0;
	short limit_adj = atomic_read(&target_min_adj);
	unsigned long pages_found = 0;
	unsigned long target_pages = get_target_free_pages();
	struct task_struct *tsk;

	/*
	 * Phase 1: Walk the process list under RCU to collect pinned
	 * candidates. get_task_struct() prevents the task from being freed
	 * after we drop RCU, so the bucket chains remain valid.
	 */
	rcu_read_lock();
	for_each_process(tsk) {
		struct signal_struct *sig;
		short adj;

		/*
		 * Search for suitable tasks with a positive adj (importance).
		 * Since only tasks with a positive adj can be targeted, that
		 * naturally excludes tasks which shouldn't be killed, like init
		 * and kthreads. Although oom_score_adj can still be changed
		 * while this code runs, it doesn't really matter; we just need
		 * a snapshot of the task's adj.
		 */
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

	/*
	 * Phase 2: Evaluate pinned candidates. Each candidate gets a brief
	 * RCU critical section only around find_lock_task_mm() (which needs
	 * RCU for for_each_thread()). This avoids holding rcu_read_lock()
	 * across the entire process walk and evaluation pass.
	 */
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
			unsigned long pages = 0;

			next = tsk->simple_lmk_next;

			/*
			 * Grace period: protect recently backgrounded apps from
			 * Tier 0 kills. When an app enters the cached tier
			 * (adj >= 800), it gets a 5-second grace period.
			 * Only applies during mild Tier 0 pressure.
			 */
			if (limit_adj == tier_min_adj[0] && i >= tier_min_adj[0] &&
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

			get_task_struct(vtsk);
			mmgrab(vtsk->mm);
			task_unlock(vtsk);
			rcu_read_unlock();

		victims[*vindex].tsk = vtsk;
		victims[*vindex].mm = vtsk->mm;
		victims[*vindex].size = pages;
		/* Not killed yet, so nothing is pending on its account */
		victims[*vindex].pending = 0;

			pages_found += pages;

			if (++*vindex == MAX_VICTIMS) {
				put_task_struct(tsk);
				/*
				 * Drain the rest of this bucket's chain
				 * since task_bucket[i] is already NULL
				 * and drain_remaining won't find them.
				 */
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
	/* Release refs for any candidates still in buckets we didn't visit */
	for (i = min_adj; i <= max_adj; i++) {
		tsk = task_bucket[i];
		task_bucket[i] = NULL;
		while (tsk) {
			struct task_struct *next = tsk->simple_lmk_next;
			put_task_struct(tsk);
			tsk = next;
		}
	}

	return pages_found;
}

static int process_victims(int vlen)
{
	unsigned long pages_found = 0;
	unsigned long target_pages = get_target_free_pages();
	int i, nr_to_kill = 0;

	/*
	 * Calculate the number of tasks that need to be killed and quickly
	 * release the references to those that'll live.
	 */
	for (i = 0; i < vlen; i++) {
		struct victim_info *victim = &victims[i];
		struct task_struct *vtsk = victim->tsk;

		/* The victim's mm and task refs were taken in find_victims */
		if (pages_found >= target_pages) {
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
	unsigned long pages_found;
	int num_drop;

	/*
	 * If the reaper is still processing the previous victim set, do not
	 * overwrite the shared victims array. Skip this cycle; PSI will
	 * re-fire if memory pressure persists.
	 */
	if (READ_ONCE(reclaim_active))
		return;

	/* Populate the victims array with tasks sorted by adj and then size */
	pages_found = find_victims(&nr_found);
	if (unlikely(!nr_found)) {
		/*
		 * No victims at the current tier. If there's still a memory
		 * deficit, escalate immediately to the next tier instead of
		 * waiting for the next PSI event. Without this, the system
		 * gets stuck at Tier 0 forever when all cached apps are
		 * already dead but memory pressure continues.
		 */
		if (get_target_free_pages() > 0) {
			int current_adj = atomic_read(&target_min_adj);
			if (current_adj == tier_min_adj[0]) {
				atomic_set(&target_min_adj, tier_min_adj[1]);
				atomic_set(&needs_reclaim, 1);
			} else if (current_adj == tier_min_adj[1]) {
				atomic_set(&target_min_adj, tier_min_adj[2]);
				atomic_set(&needs_reclaim, 1);
			}
			pr_info_ratelimited("Escalating to adj %d, no victims at current tier\n",
					    atomic_read(&target_min_adj));
		}
		return;
	}

	/*
	 * Sort all victims by size (descending) to kill largest first,
	 * then select the minimum number needed to meet the target.
	 */
	sort(victims, nr_found, sizeof(*victims), victim_cmp_size, victim_swap);
	nr_to_kill = process_victims(nr_found);

	/*
	 * Store the final number of victims for simple_lmk_mm_freed() and the
	 * reaper thread, and indicate that reclaim is active.
	 */
	num_drop = 0;
	WRITE_ONCE(nr_victims, nr_to_kill);
	WRITE_ONCE(reclaim_active, true);
	for (i = 0; i < nr_to_kill; i++) {
		struct mm_struct *mm = victims[i].mm;

		if (mm && test_bit(MMF_OOM_SKIP, &mm->flags)) {
			victims[i].mm = NULL;
			drop_mms[num_drop++] = mm;
		}
	}

	for (i = 0; i < num_drop; i++)
		mmdrop(drop_mms[i]);

	/* Kill the victims */
	for (i = 0; i < nr_to_kill; i++) {
		struct victim_info *victim = &victims[i];
		struct task_struct *t, *vtsk = victim->tsk;
		struct mm_struct *mm = victim->mm;

		/*
		 * Released above rather than killed: its memory was already
		 * gone before we selected it, so there is nothing here to
		 * reclaim. Killing it anyway would only emit a
		 * "Killing ... to free N KiB" line for memory that will never
		 * be freed, and inflate the kill count for anyone reading the
		 * log to judge whether the driver is over-killing.
		 */
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

		/*
		 * Thaw the victim first so it can receive and process the
		 * kill signal immediately. Signals can't wake frozen tasks;
		 * only a thaw operation can.
		 */
		if (frozen(vtsk))
			__thaw_task(vtsk);

		/* Accelerate the victim's death by forcing the kill signal */
		do_send_sig_info(SIGKILL, SEND_SIG_PRIV, vtsk, PIDTYPE_TGID);

		set_bit(MMF_SIMPLE_LMK_VICTIM, &mm->flags);

		/*
		 * Drop the victim's oom_score_adj to OOM_SCORE_ADJ_MIN.
		 * This cleanly ensures Android and the kernel's scheduler
		 * prioritize the dying task's teardown.
		 */
		WRITE_ONCE(vtsk->signal->oom_score_adj, OOM_SCORE_ADJ_MIN);

		/*
		 * Mark the thread group dead so that the page allocator knows
		 * to give these tasks emergency memory priority (ALLOC_NO_WATERMARKS).
		 * Without this, victims stall during exit under extreme pressure.
		 */
		rcu_read_lock();
		for_each_thread(vtsk, t)
			set_tsk_thread_flag(t, TIF_MEMDIE);
		rcu_read_unlock();

		/* Allow the victim to run on any CPU. This won't schedule. */
		set_cpus_allowed_ptr(vtsk, cpu_all_mask);

		/* Store the number of anon pages to sort victims for reaping */
		victim->score = get_mm_counter(mm, MM_ANONPAGES);

		/*
		 * The kill is dispatched below, so this victim's resident
		 * anonymous pages are now guaranteed to be freed even though
		 * nr_free_pages() will not reflect that for some time.
		 */
		victim->pending = victim->score;

		/* We don't need the task_struct anymore */
		put_task_struct(vtsk);
		victim->tsk = NULL;
	}

	/*
	 * Sort the victims by descending order of anonymous pages so the reaper
	 * thread can prioritize reaping the victims with the most anonymous
	 * pages first. Then wake the reaper thread if it's asleep.
	 *
	 * reclaim_active stays true until the reaper confirms all victims are
	 * done (see next_reap_victim). The smp_wmb() ensures the reaper sees
	 * the fully-populated victims array and nr_victims before it observes
	 * needs_reap == 1.
	 */
	sort(victims, nr_to_kill, sizeof(*victims), victim_cmp, victim_swap);
	smp_wmb();
	atomic_set(&needs_reap, 1);
	if (waitqueue_active(&reaper_waitq))
		wake_up(&reaper_waitq);
}

static int simple_lmk_reclaim_thread(void *data)
{
	/* Use maximum RT priority */
	set_task_rt_prio(current, MAX_RT_PRIO - 1);
	set_freezable();

	while (1) {
		wait_event_freezable(oom_waitq, atomic_read(&needs_reclaim));
		/*
		 * Clear needs_reclaim before scanning so that any escalation
		 * signal set by scan_and_kill() (or a new PSI event arriving
		 * during the scan) is not lost.
		 */
		atomic_set(&needs_reclaim, 0);
		scan_and_kill();
	}

	return 0;
}

static struct mm_struct *next_reap_victim(bool force)
{
	struct mm_struct *mm = NULL;
	bool should_retry = false;
	int i;

	/*
	 * cmpxchg in simple_lmk_mm_freed() protects victims[i].mm. We take an
	 * mmget reference so the mm struct can't be freed while we reap it.
	 */
	for (i = 0; i < READ_ONCE(nr_victims); i++, mm = NULL) {
		/* Check if this victim is alive and hasn't been reaped yet */
		mm = READ_ONCE(victims[i].mm);
		if (!mm || test_bit(MMF_OOM_SKIP, &mm->flags))
			continue;

		if (!mmget_not_zero(mm))
			continue;

		/*
		 * Do a trylock so the reaper thread doesn't sleep. If the
		 * trylock fails and we've exhausted the retry deadline (force
		 * == true), give up on this victim: mark it OOM_SKIP, clear
		 * it from the array, and drop our mm_count reference. The
		 * victim already has SIGKILL + TIF_MEMDIE, so it will exit
		 * and its pages will be freed by exit_mmap() without us.
		 */
		if (!mmap_read_trylock(mm)) {
			if (force) {
				/*
				 * Operate on the mm's flags and drop our mmgrab()
				 * reference *before* mmput(). mmput() can drop the
				 * last mm_users reference, which synchronously runs
				 * __mmput() -> exit_mmap() -> simple_lmk_mm_freed(),
				 * and the latter can mmdrop() the mmgrab() reference
				 * we hold here, freeing the mm. Dereferencing mm
				 * after mmput() would therefore be a use-after-free.
				 */
				set_bit(MMF_OOM_SKIP, &mm->flags);
				if (cmpxchg(&victims[i].mm, mm, NULL) == mm)
					mmdrop(mm);
			} else {
				should_retry = true;
			}
			mmput(mm);
			continue;
		}

		/*
		 * Check MMF_OOM_SKIP again under the lock in case this mm was
		 * reaped by exit_mmap() and then had its page tables destroyed.
		 */
		if (!test_bit(MMF_OOM_SKIP, &mm->flags))
			break;

		mmap_read_unlock(mm);
		mmput(mm);
	}

	if (!mm) {
		if (should_retry) {
			/* Return ERR_PTR(-EAGAIN) to try reaping again later */
			mm = ERR_PTR(-EAGAIN);
		} else {
			/*
			 * Nothing left to reap. Clear reclaim_active so
			 * simple_lmk_mm_freed() stops searching the victims
			 * array, and so scan_and_kill() can start a new cycle.
			 * The smp_mb() pairs with the smp_wmb() in
			 * scan_and_kill() to ensure all prior victim mm
			 * pointers are visible as NULL before we declare
			 * reclaim inactive.
			 *
			 * This must stay inside the else: clearing the flag
			 * while we are handing back -EAGAIN lets a concurrent
			 * scan_and_kill() overwrite the victims array that
			 * next_reap_victim() and simple_lmk_mm_freed() are
			 * still walking.
			 */
			smp_mb();
			WRITE_ONCE(reclaim_active, false);
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
			/*
			 * A victim's mmap_read_trylock failed. Retry with a
			 * bounded deadline derived from
			 * CONFIG_ANDROID_SIMPLE_LMK_TIMEOUT_MSEC. If the
			 * deadline expires, force-give-up on stuck victims
			 * instead of spinning forever.
			 */
			if (!retry_deadline) {
				retry_deadline = jiffies + REAP_RETRY_JIFFIES;
			} else if (time_after(jiffies, retry_deadline)) {
				force = true;
				retry_deadline = 0;
			}
			/* Wait one jiffy before trying to reap again */
			schedule_timeout_uninterruptible(1);
			continue;
		}

		/* Successfully acquired mmap_lock; reset retry state */
		retry_deadline = 0;
		force = false;

		/*
		 * Try to reap the victim. Mark it as reaped with MMF_OOM_SKIP
		 * if successful.
		 */
		if (__oom_reap_task_mm(mm))
			set_bit(MMF_OOM_SKIP, &mm->flags);
		mmap_read_unlock(mm);
		mmput(mm);

		/* Yield to let RCU grace periods and other work proceed */
		cond_resched();
	}
}

static int simple_lmk_reaper_thread(void *data)
{
	/* Use a lower priority than the reclaim thread */
	set_task_rt_prio(current, MAX_RT_PRIO - 2);
	set_freezable();

	while (1) {
		wait_event_freezable(reaper_waitq, atomic_read(&needs_reap));
		atomic_set(&needs_reap, 0);
		reap_victims();
	}

	return 0;
}

void simple_lmk_mm_freed(struct mm_struct *mm)
{
	int i;
	bool matched = false;

	/*
	 * Victims are guaranteed to have MMF_OOM_SKIP set after exit_mmap()
	 * finishes. Use this to ignore unrelated dying processes.
	 */
	if (!test_bit(MMF_OOM_SKIP, &mm->flags) || !test_bit(MMF_SIMPLE_LMK_VICTIM, &mm->flags))
		return;

	/*
	 * No fast path on reclaim_active here, and that is deliberate.
	 *
	 * reclaim_active is cleared by next_reap_victim() once *reaping* is
	 * done, but a victim that was reaped successfully has not *exited*
	 * yet -- and this function runs from __mmput() after exit_mmap(). For
	 * such a victim this is the only place its mmgrab() reference is
	 * released, so skipping the search strands it: __mmput() then drops
	 * its own reference at the end without ours ever being dropped,
	 * mm_count never reaches zero, and the mm_struct is leaked outright.
	 *
	 * Scanning is bounded by MAX_VICTIMS against a cacheline-aligned
	 * array and runs once per process exit, so the search is not worth a
	 * correctness hazard. Every path that drops a victim's reference
	 * clears its slot first, so stale entries cannot be matched.
	 */
	for (i = 0; i < READ_ONCE(nr_victims); i++) {
		if (READ_ONCE(victims[i].mm) == mm) {
			if (cmpxchg(&victims[i].mm, mm, NULL) == mm) {
				matched = true;
				break;
			}
		}
	}

	if (matched)
		mmdrop(mm);
}

static struct psi_trigger *psi_triggers[LMK_TIERS];
static DECLARE_WAIT_QUEUE_HEAD(psi_waitq);

static int simple_lmk_psi_thread(void *data)
{
	set_task_rt_prio(current, MAX_RT_PRIO - 3);
	set_freezable();

	/* Wait for PSI triggers to be created before accessing them */
	wait_for_completion(&psi_init_done);

	while (!kthread_should_stop()) {
		short min_adj = ADJ_MAX;

		/*
		 * Sleep until a PSI trigger fires. wait_event_freezable
		 * checks try_to_freeze() before sleeping, allowing the
		 * freezer to suspend us.
		 */
		wait_event_freezable(psi_waitq,
				     READ_ONCE(psi_triggers[0]->event) ||
				     READ_ONCE(psi_triggers[1]->event) ||
				     READ_ONCE(psi_triggers[2]->event));

		/* Check triggers from highest to lowest severity */
		if (cmpxchg(&psi_triggers[2]->event, 1, 0)) {
			min_adj = tier_min_adj[2];
		} else if (cmpxchg(&psi_triggers[1]->event, 1, 0)) {
			min_adj = tier_min_adj[1];
		} else if (cmpxchg(&psi_triggers[0]->event, 1, 0)) {
			min_adj = tier_min_adj[0];
		}

		/*
		 * Map PSI stall events to target adj levels.
		 * reclaim_active gates new cycles while scan_and_kill
		 * is still running.
		 */
		if (min_adj != ADJ_MAX && !READ_ONCE(reclaim_active)) {
			atomic_set(&target_min_adj, min_adj);
			if (!atomic_xchg(&needs_reclaim, 1) && waitqueue_active(&oom_waitq))
				wake_up(&oom_waitq);
		}
	}

	return 0;
}

/*
 * Stamp simple_lmk_cache_time only when the task *enters* the cached tier.
 *
 * This hook runs on every oom_score_adj write, and ActivityManager rewrites
 * adj for cached apps on many state changes. Stamping unconditionally made
 * cache_time mean "last written" rather than "entered the tier", while its
 * only reader -- the grace period in find_victims() -- means the latter: it
 * drops any candidate stamped within GRACE_PERIOD_MS, so an app rewritten
 * more often than that is never killable at Tier 0 at all.
 *
 * That is the under-killing counterpart to the over-killing fixed by
 * accounting for pages already pending free.
 *
 * Called before the new value is assigned, so task->signal->oom_score_adj
 * still holds the previous one and the transition is visible.
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

	/*
	 * This is an uncaught OOM event (e.g. a huge sudden allocation) that
	 * PSI missed. Escalate to the maximum tier and wake the reclaim thread
	 * to handle it asynchronously. We never call scan_and_kill() directly
	 * here because it may sleep (set_cpus_allowed_ptr, etc.) and the OOM
	 * notifier can run from contexts where sleeping is undesirable.
	 *
	 * Tell the core OOM killer we are handling it (*freed = 1) to suppress
	 * a dual-kill collision. If the reclaim thread fails to find victims,
	 * the next PSI/OOM event will re-trigger.
	 */
	atomic_set(&target_min_adj, tier_min_adj[2]);
	if (!atomic_xchg(&needs_reclaim, 1) && waitqueue_active(&oom_waitq))
		wake_up(&oom_waitq);

	*freed = 1;
	return NOTIFY_OK;
}

static struct notifier_block simple_lmk_oom_nb = {
	.notifier_call = simple_lmk_oom_notify,
};

/* Initialize Simple LMK when lmkd in Android writes to the minfree parameter */
static int simple_lmk_init_set(const char *val, const struct kernel_param *kp)
{
	static atomic_t init_done = ATOMIC_INIT(0);
	struct task_struct *thread;
	int thresholds[LMK_TIERS] = {
		CONFIG_ANDROID_SIMPLE_LMK_PSI_THRESHOLD_LOW_US,
		CONFIG_ANDROID_SIMPLE_LMK_PSI_THRESHOLD_MED_US,
		CONFIG_ANDROID_SIMPLE_LMK_PSI_THRESHOLD_HIGH_US
	};
	int i, ret = 0;

	if (!atomic_cmpxchg(&init_done, 0, 1)) {
		thread = kthread_run(simple_lmk_reaper_thread, NULL,
				     "simple_lmkd_reaper");
		if (IS_ERR(thread)) {
			ret = PTR_ERR(thread);
			goto fail;
		}

		thread = kthread_run(simple_lmk_reclaim_thread, NULL,
				     "simple_lmkd");
		if (IS_ERR(thread)) {
			ret = PTR_ERR(thread);
			goto fail;
		}

		/*
		 * Create PSI triggers before the PSI monitor thread so
		 * the triggers are ready when the thread wakes up.
		 */
		for (i = 0; i < LMK_TIERS; i++) {
			char buf[64];
			snprintf(buf, sizeof(buf), "full %d %d", thresholds[i],
				 CONFIG_ANDROID_SIMPLE_LMK_PSI_WINDOW_MS * 1000);
			psi_triggers[i] = psi_trigger_create(&psi_system, buf, PSI_MEM);
			if (IS_ERR(psi_triggers[i])) {
				ret = PTR_ERR(psi_triggers[i]);
				psi_triggers[i] = NULL;
				goto fail;
			}
			psi_trigger_set_waitq(psi_triggers[i], &psi_waitq);
		}

		thread = kthread_run(simple_lmk_psi_thread, NULL,
				     "simple_lmkd_psi");
		if (IS_ERR(thread)) {
			ret = PTR_ERR(thread);
			goto fail;
		}

		WARN_ON(register_oom_notifier(&simple_lmk_oom_nb));

		complete(&psi_init_done);
	}

	return 0;

fail:
	/*
	 * Roll back any partially created state and allow lmkd to retry
	 * initialization on a subsequent write to the minfree parameter.
	 */
	for (i = 0; i < LMK_TIERS; i++) {
		if (psi_triggers[i]) {
			psi_trigger_destroy(psi_triggers[i]);
			psi_triggers[i] = NULL;
		}
	}
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
