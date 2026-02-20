// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019-2023 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "simple_lmk: " fmt

#include <linux/freezer.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/oom.h>
#include <linux/sched/mm.h>
#include <linux/sort.h>
#include <linux/vmpressure.h>
#include <uapi/linux/sched/types.h>

/* The minimum number of pages to free per reclaim */
#define MIN_FREE_PAGES (CONFIG_ANDROID_SIMPLE_LMK_MINFREE * SZ_1M / PAGE_SIZE)

/* Kill up to this many victims per reclaim */
#define MAX_VICTIMS 1024

/*
 * OOM score adj bucket range. Android uses -1000 to 1000 for oom_score_adj,
 * but only tasks with adj >= 0 are eligible for killing. The full range is
 * kept for bucket indexing so adj_to_bucket() yields a non-negative index.
 */
#define OOM_ADJ_MIN (-1024)
#define OOM_ADJ_MAX 1024
#define OOM_ADJ_BUCKET_COUNT (OOM_ADJ_MAX - OOM_ADJ_MIN + 1)

/* Convert OOM score adj to bucket index (always non-negative) */
#define adj_to_bucket(adj) ((adj) - OOM_ADJ_MIN)

/* Timeout in jiffies for each reclaim */
#define RECLAIM_EXPIRES msecs_to_jiffies(CONFIG_ANDROID_SIMPLE_LMK_TIMEOUT_MSEC)

/* Minimum interval in jiffies between reclaim trigger events */
#define RECLAIM_COOLDOWN \
	msecs_to_jiffies(CONFIG_ANDROID_SIMPLE_LMK_COOLDOWN_MSEC)

struct victim_info {
	struct task_struct *tsk;
	struct mm_struct *mm;
	unsigned long size;
};

static struct victim_info victims[MAX_VICTIMS] __cacheline_aligned_in_smp;
static struct task_struct *task_bucket[OOM_ADJ_BUCKET_COUNT] __cacheline_aligned;
static DECLARE_WAIT_QUEUE_HEAD(oom_waitq);
static DECLARE_WAIT_QUEUE_HEAD(reaper_waitq);
static DECLARE_COMPLETION(reclaim_done);
static __cacheline_aligned_in_smp DEFINE_RWLOCK(mm_free_lock);
static int nr_victims;
static bool reclaim_active;
static atomic_t needs_reclaim = ATOMIC_INIT(0);
static atomic_t needs_reap = ATOMIC_INIT(0);
static atomic_t nr_killed = ATOMIC_INIT(0);

/* Timestamp of last reclaim trigger for cooldown enforcement */
static unsigned long last_reclaim_jiffies;

static int victim_cmp(const void *lhs_ptr, const void *rhs_ptr)
{
	const struct victim_info *lhs = (typeof(lhs))lhs_ptr;
	const struct victim_info *rhs = (typeof(rhs))rhs_ptr;

	/* Sort in descending order of size */
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

static unsigned long get_total_mm_pages(struct mm_struct *mm)
{
	unsigned long pages = 0;
	int i;

	for (i = 0; i < NR_MM_COUNTERS; i++)
		pages += get_mm_counter(mm, i);

	return pages;
}

/*
 * Release mm references (mmgrab pins) for victims left over from a previous
 * reclaim cycle. Called at the start of each new scan_and_kill() invocation.
 *
 * After write_lock sets nr_victims to 0, no concurrent code (reaper thread
 * or simple_lmk_mm_freed callback) will access the victims array, so the
 * mmdrop calls outside the lock are safe without additional synchronization.
 */
static void release_stale_victims(void)
{
	int i, old_nr;

	write_lock(&mm_free_lock);
	old_nr = nr_victims;
	nr_victims = 0;
	write_unlock(&mm_free_lock);

	for (i = 0; i < old_nr; i++) {
		if (victims[i].mm) {
			mmdrop(victims[i].mm);
			victims[i].mm = NULL;
		}
	}
}

static unsigned long find_victims(int *vindex)
{
	short i, min_adj = SHRT_MAX, max_adj = 0;
	unsigned long pages_found = 0;
	struct task_struct *tsk;

	rcu_read_lock();
	for_each_process(tsk) {
		struct signal_struct *sig;
		short adj;

		/*
		 * Search for suitable tasks with a non-negative adj.
		 * Since only tasks with adj >= 0 can be targeted, that
		 * naturally excludes tasks which shouldn't be killed, like
		 * init and kthreads. Although oom_score_adj can still be
		 * changed while this code runs, it doesn't really matter;
		 * we just need a snapshot of the task's adj.
		 */
		sig = tsk->signal;
		adj = READ_ONCE(sig->oom_score_adj);
		if (adj < 0 || sig->flags & SIGNAL_GROUP_EXIT ||
		    (thread_group_empty(tsk) && tsk->flags & PF_EXITING))
			continue;

		/* Clamp adj to our supported range */
		if (adj > OOM_ADJ_MAX)
			adj = OOM_ADJ_MAX;

		/* Store the task in a linked-list bucket based on its adj */
		tsk->simple_lmk_next = task_bucket[adj_to_bucket(adj)];
		task_bucket[adj_to_bucket(adj)] = tsk;

		/* Track the min and max adjs to speed up the loop below */
		if (adj > max_adj)
			max_adj = adj;
		if (adj < min_adj)
			min_adj = adj;
	}

	/* Start searching for victims from the highest adj (least important) */
	for (i = max_adj; i >= min_adj; i--) {
		int old_vindex;
		int bucket_idx;

		bucket_idx = adj_to_bucket(i);
		tsk = task_bucket[bucket_idx];
		if (!tsk)
			continue;

		/* Clear this bucket; the unconditional memset below is belt-and-suspenders */
		task_bucket[bucket_idx] = NULL;

		/* Iterate through every task with this adj */
		old_vindex = *vindex;
		do {
			struct task_struct *vtsk;

			vtsk = find_lock_task_mm(tsk);
			if (!vtsk)
				continue;

			/*
			 * Pin the mm_struct so it cannot be freed while stored
			 * in the victims array. task_lock(vtsk) is held, so
			 * vtsk->mm is stable and non-NULL. This mmgrab ensures
			 * the mm_struct persists independently of mm_users,
			 * which is the correct lifetime guarantee for the
			 * reaper thread and simple_lmk_mm_freed() callback.
			 */
			mmgrab(vtsk->mm);

			/* Store this potential victim away for later */
			victims[*vindex].tsk = vtsk;
			victims[*vindex].mm = vtsk->mm;
			victims[*vindex].size = get_total_mm_pages(vtsk->mm);

			/* Count the number of pages that have been found */
			pages_found += victims[*vindex].size;

			/* Make sure there's space left in the victim array */
			if (++*vindex == MAX_VICTIMS)
				break;
		} while ((tsk = tsk->simple_lmk_next));

		/* Go to the next bucket if nothing was found */
		if (*vindex == old_vindex)
			continue;

		/*
		 * Sort the victims in descending order of size to prioritize
		 * killing the larger ones first.
		 */
		sort(&victims[old_vindex], *vindex - old_vindex,
		     sizeof(*victims), victim_cmp, victim_swap);

		/* Stop when we are out of space or have enough pages found */
		if (*vindex == MAX_VICTIMS || pages_found >= MIN_FREE_PAGES)
			break;
	}

	/*
	 * Unconditionally clear all buckets in the populated range. Some
	 * buckets were already cleared in the loop above, and some may still
	 * contain stale entries from an early break. A single memset over the
	 * entire [min_adj, max_adj] range eliminates dependence on loop
	 * ordering invariants. The range is at most OOM_ADJ_MAX + 1 entries
	 * and this is done once per reclaim, so the cost is negligible.
	 */
	if (min_adj <= max_adj) {
		int start = adj_to_bucket(min_adj);
		int end = adj_to_bucket(max_adj);

		memset(&task_bucket[start], 0,
		       (end - start + 1) * sizeof(*task_bucket));
	}

	rcu_read_unlock();

	return pages_found;
}

static int process_victims(int vlen)
{
	unsigned long pages_found = 0;
	int i, nr_to_kill = 0;

	/*
	 * Calculate the number of tasks that need to be killed and quickly
	 * release the references to those that'll live.
	 */
	for (i = 0; i < vlen; i++) {
		struct victim_info *victim = &victims[i];
		struct task_struct *vtsk = victim->tsk;

		/* The victim's task lock is held from find_victims; release it */
		if (pages_found >= MIN_FREE_PAGES) {
			task_unlock(vtsk);
			/* Release mm pin for tasks that won't be killed */
			mmdrop(victim->mm);
			victim->mm = NULL;
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
	int i, nr_to_kill, nr_found = 0;
	unsigned long pages_found;

	/*
	 * Release mm references held from the previous reclaim cycle. This
	 * handles victims that were still alive when the previous cycle ended
	 * (timeout or reaper completion) and whose simple_lmk_mm_freed()
	 * callback was either not reached or could not find its entry because
	 * nr_victims was already zeroed by the reaper.
	 */
	release_stale_victims();

	/* Populate the victims array with tasks sorted by adj and then size */
	pages_found = find_victims(&nr_found);
	if (unlikely(!nr_found)) {
		pr_err_ratelimited("No processes available to kill!\n");
		return;
	}

	/* Minimize the number of victims if we found more pages than needed */
	if (pages_found > MIN_FREE_PAGES) {
		/* First round of processing to weed out unneeded victims */
		nr_to_kill = process_victims(nr_found);

		/*
		 * Try to kill as few of the chosen victims as possible by
		 * sorting the chosen victims by size, which means larger
		 * victims that have a lower adj can be killed in place of
		 * smaller victims with a high adj.
		 */
		sort(victims, nr_to_kill, sizeof(*victims), victim_cmp,
		     victim_swap);

		/* Second round of processing to finally select the victims */
		nr_to_kill = process_victims(nr_to_kill);
	} else {
		/* Too few pages found, so all the victims need to be killed */
		nr_to_kill = nr_found;
	}

	/*
	 * Store the final number of victims for simple_lmk_mm_freed() and the
	 * reaper thread, and indicate that reclaim is active.
	 */
	write_lock(&mm_free_lock);
	nr_victims = nr_to_kill;
	reclaim_active = true;
	write_unlock(&mm_free_lock);

	/* Kill the victims */
	for (i = 0; i < nr_to_kill; i++) {
		struct victim_info *victim = &victims[i];
		struct task_struct *t, *vtsk = victim->tsk;
		struct mm_struct *mm = victim->mm;

		pr_info("Killing %s with adj %d to free %lu KiB\n",
			vtsk->comm, vtsk->signal->oom_score_adj,
			victim->size << (PAGE_SHIFT - 10));

		/* Accelerate the victim's death by forcing the kill signal */
		do_send_sig_info(SIGKILL, SEND_SIG_PRIV, vtsk, PIDTYPE_TGID);

		/*
		 * Mark the thread group as dying so that the memory allocator
		 * gives them access to memory reserves (TIF_MEMDIE), and
		 * elevate the thread group to SCHED_RR with minimum RT
		 * priority. The entire group needs to be elevated because
		 * there's no telling which threads have references to the mm
		 * as well as which thread will happen to put the final
		 * reference and release the mm's memory.
		 *
		 * Note: TIF_MEMDIE is cleared unconditionally in exit_mm()
		 * when CONFIG_ANDROID_SIMPLE_LMK is set, without calling
		 * exit_oom_victim(). This avoids corrupting the global
		 * oom_victims counter, which is never incremented here.
		 */
		rcu_read_lock();
		for_each_thread(vtsk, t)
			set_tsk_thread_flag(t, TIF_MEMDIE);
		for_each_thread(vtsk, t)
			set_task_rt_prio(t, 1);
		rcu_read_unlock();

		/* Allow the victim to run on any CPU. This won't schedule. */
		set_cpus_allowed_ptr(vtsk, cpu_all_mask);

		/* Signals can't wake frozen tasks; only a thaw operation can */
		__thaw_task(vtsk);

		/* Store the number of anon pages to sort victims for reaping */
		victim->size = get_mm_counter(mm, MM_ANONPAGES);

		/* Finally release the victim's task lock acquired earlier */
		task_unlock(vtsk);
	}

	/*
	 * Sort the victims by descending order of anonymous pages so the reaper
	 * thread can prioritize reaping the victims with the most anonymous
	 * pages first. Then wake the reaper thread if it's asleep.
	 */
	write_lock(&mm_free_lock);
	sort(victims, nr_to_kill, sizeof(*victims), victim_cmp, victim_swap);
	atomic_set(&needs_reap, 1);
	write_unlock(&mm_free_lock);
	wake_up(&reaper_waitq);

	/* Wait until all the victims die or until the timeout is reached */
	if (!wait_for_completion_timeout(&reclaim_done, RECLAIM_EXPIRES))
		pr_info("Timeout hit waiting for victims to die, proceeding\n");

	/*
	 * Clean up for future reclaims but let the reaper thread keep going.
	 *
	 * Ordering within the write_lock critical section:
	 *   1. reclaim_active = false  — prevents late-dying victims from
	 *      calling complete() after we reinitialize the completion.
	 *   2. atomic_set nr_killed    — resets the kill counter using a
	 *      proper atomic store (not a struct assignment).
	 *   3. reinit_completion       — prepares the completion for the
	 *      next reclaim cycle with done = 0.
	 *
	 * All three operations are under write_lock, so they appear atomic to
	 * simple_lmk_mm_freed() readers that hold read_lock. The ordering is
	 * maintained for clarity and to prevent issues under future refactors.
	 */
	write_lock(&mm_free_lock);
	reclaim_active = false;
	atomic_set(&nr_killed, 0);
	reinit_completion(&reclaim_done);
	write_unlock(&mm_free_lock);
}

static int simple_lmk_reclaim_thread(void *data)
{
	/* Use maximum RT priority */
	set_task_rt_prio(current, MAX_RT_PRIO - 1);
	set_freezable();

	while (1) {
		wait_event_freezable(oom_waitq,
				     atomic_read_acquire(&needs_reclaim));
		scan_and_kill();
		atomic_set_release(&needs_reclaim, 0);
	}

	return 0;
}

static struct mm_struct *next_reap_victim(void)
{
	struct mm_struct *mm = NULL;
	bool should_retry = false;
	int i;

	/* Take a write lock so no victim's mm can be freed while scanning */
	write_lock(&mm_free_lock);
	for (i = 0; i < nr_victims; i++, mm = NULL) {
		/* Check if this victim is alive and hasn't been reaped yet */
		mm = victims[i].mm;
		if (!mm || test_bit(MMF_OOM_SKIP, &mm->flags))
			continue;

		/* Do a trylock so the reaper thread doesn't sleep */
		if (!mmap_read_trylock(mm)) {
			should_retry = true;
			continue;
		}

		/*
		 * Check MMF_OOM_SKIP again under the mmap read lock in case
		 * exit_mmap() reaped this mm and set the flag between our
		 * check above and the trylock. The mm_struct itself is
		 * pinned via mmgrab() taken during victim selection, so it
		 * is safe to dereference unconditionally here. The mmap read
		 * lock serializes against exit_mmap()'s mmap_write_lock()
		 * and guarantees page tables are intact when the flag is
		 * not set.
		 */
		if (!test_bit(MMF_OOM_SKIP, &mm->flags))
			break;
		mmap_read_unlock(mm);
	}

	if (!mm) {
		if (should_retry) {
			/* Return ERR_PTR(-EAGAIN) to try reaping again later */
			mm = ERR_PTR(-EAGAIN);
		} else if (!reclaim_active) {
			/*
			 * Nothing left to reap and reclaim has finished. Set
			 * nr_victims to 0 so simple_lmk_mm_freed() stops
			 * iterating over stale entries. Any remaining non-NULL
			 * mm references (from mmgrab) will be released by
			 * release_stale_victims() at the start of the next
			 * reclaim cycle.
			 */
			nr_victims = 0;
		}
	}
	write_unlock(&mm_free_lock);

	return mm;
}

static void reap_victims(void)
{
	struct mm_struct *mm;

	while ((mm = next_reap_victim())) {
		if (IS_ERR(mm)) {
			/* Wait one jiffy before trying to reap again */
			schedule_timeout_uninterruptible(1);
			continue;
		}

		/*
		 * Try to reap the victim. Mark it as reaped with MMF_OOM_SKIP
		 * if successful.
		 */
		if (__oom_reap_task_mm(mm))
			set_bit(MMF_OOM_SKIP, &mm->flags);
		mmap_read_unlock(mm);
	}
}

static int simple_lmk_reaper_thread(void *data)
{
	/* Use a lower priority than the reclaim thread */
	set_task_rt_prio(current, MAX_RT_PRIO - 2);
	set_freezable();

	while (1) {
		wait_event_freezable(reaper_waitq,
				     atomic_cmpxchg(&needs_reap, 1, 0));
		reap_victims();
	}

	return 0;
}

void simple_lmk_mm_freed(struct mm_struct *mm)
{
	int i;
	bool do_mmdrop = false;

	/*
	 * Victims are guaranteed to have MMF_OOM_SKIP set after exit_mmap()
	 * finishes. Use this to ignore unrelated dying processes.
	 */
	if (!test_bit(MMF_OOM_SKIP, &mm->flags))
		return;

	read_lock(&mm_free_lock);
	for (i = 0; i < nr_victims; i++) {
		if (victims[i].mm == mm) {
			/*
			 * Clear out this victim from the victims array and
			 * only increment nr_killed if reclaim is active. If
			 * reclaim isn't active, then clearing out the victim
			 * is done solely for the reaper thread to skip freed
			 * victims.
			 */
			victims[i].mm = NULL;
			do_mmdrop = true;
			if (reclaim_active &&
			    atomic_inc_return_relaxed(&nr_killed) == nr_victims)
				complete(&reclaim_done);
			break;
		}
	}
	read_unlock(&mm_free_lock);

	/*
	 * Release the mmgrab() reference taken in find_victims() outside the
	 * spinlock. mmdrop() may call __mmdrop() which can do non-trivial
	 * cleanup, so it must not be called under a spinlock.
	 *
	 * This is safe because the caller (__mmput) will call mmdrop() later
	 * to release the original mm_count reference. Our mmgrab() added a
	 * separate reference, so our mmdrop() merely decrements mm_count
	 * without freeing the mm_struct (the caller's mmdrop() will do that).
	 */
	if (do_mmdrop)
		mmdrop(mm);
}

static int simple_lmk_vmpressure_cb(struct notifier_block *nb,
				     unsigned long pressure, void *data)
{
	/*
	 * Trigger reclaim when pressure exceeds the configured threshold.
	 * Enforce a cooldown interval between consecutive triggers to prevent
	 * kill storms when pressure stays elevated. The cooldown gives killed
	 * processes time to release memory and watermarks to recover.
	 *
	 * last_reclaim_jiffies is accessed without locking; a benign race
	 * may allow one extra trigger past cooldown, which is acceptable.
	 */
	if (pressure >= CONFIG_ANDROID_SIMPLE_LMK_PRESSURE &&
	    time_after_eq(jiffies, last_reclaim_jiffies + RECLAIM_COOLDOWN)) {
		last_reclaim_jiffies = jiffies;
		atomic_set_release(&needs_reclaim, 1);
		wake_up(&oom_waitq);
	}

	return NOTIFY_OK;
}

static struct notifier_block vmpressure_notif = {
	.notifier_call = simple_lmk_vmpressure_cb,
	.priority = INT_MAX
};

/* Initialize Simple LMK when lmkd in Android writes to the minfree parameter */
static int simple_lmk_init_set(const char *val, const struct kernel_param *kp)
{
	static atomic_t init_done = ATOMIC_INIT(0);
	struct task_struct *thread;
	int ret;

	if (!atomic_cmpxchg(&init_done, 0, 1)) {
		thread = kthread_run(simple_lmk_reaper_thread, NULL,
				     "simple_lmkd_reaper");
		if (WARN_ON(IS_ERR(thread))) {
			ret = PTR_ERR(thread);
			pr_err("Failed to create reaper thread: %d\n", ret);
			goto err_reset;
		}

		thread = kthread_run(simple_lmk_reclaim_thread, NULL,
				     "simple_lmkd");
		if (WARN_ON(IS_ERR(thread))) {
			ret = PTR_ERR(thread);
			pr_err("Failed to create reclaim thread: %d\n", ret);
			goto err_reset;
		}

		ret = vmpressure_notifier_register(&vmpressure_notif);
		if (WARN_ON(ret)) {
			pr_err("Failed to register vmpressure notifier: %d\n",
			       ret);
			goto err_reset;
		}
	}

	return 0;

err_reset:
	/*
	 * Threads that did start will remain idle since needs_reclaim and
	 * needs_reap will stay at 0. Reset init_done to allow retry.
	 */
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
