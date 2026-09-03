/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ZCOMP_EH_H_
#define _ZCOMP_EH_H_

#include <linux/eh.h>
#include <linux/bio.h>

#define BATCH_ZCOMP_REQUEST (128)

/* The 32 is align with SWAP_CLUSTER_MAX and BLK_MAX_REQUEST_COUNT */
#define ZCOMP_BLK_MAX_REQUEST_COUNT 32

/*
 * Multiplier for the pool of deferred completion items. Sized off the number
 * of CPUs so the pool scales with the machines this runs on.
 */
#define ZCOMP_EH_DONE_PER_CPU	32

/*
 * For compression request, zcomp generates a cookie and pass it to
 * the zcomp instance. The zcomp instance need to call zcomp_copy_buffer
 * with this cookie when it completes the compression.
 */
struct zcomp_cookie {
	struct zram *zram; /* zram instance generated the cookie */
	struct zcomp_eh *zcomp_eh;
	u32 index; /* requested page-sized block index in zram block */
	struct page *page; /* requested page for compression */
	struct bio *bio;
	struct list_head list; /* list for page pool at idle */
			       /* list for pended io at active */
};

struct zcomp_cookie_pool {
	struct list_head head;
	int count;
	spinlock_t lock;
};

/*
 * A completion that has been handed off the compression thread.
 *
 * Everything here is either a scalar or owned by this item until the work
 * finishes. It holds no reference to the compression engine's buffers: the
 * payload is copied into zsmalloc before the handoff, which is what makes
 * deferring safe.
 */
struct zcomp_eh_done {
	struct work_struct work;
	struct zcomp_eh *zcomp_eh;
	struct zram *zram;
	struct zcomp_cookie *cookie;
	struct bio *bio;
	unsigned long handle;
	unsigned int len;
	u32 index;
	struct list_head list; /* free list when idle */
};

struct zcomp_eh {
	struct eh_device *eh_dev;
	struct zcomp_cookie_pool cookie_pool;
	unsigned long pend_request;
	struct list_head request_list;
	u32 prio;
	spinlock_t request_lock;

	struct workqueue_struct *done_wq;
	struct zcomp_eh_done *done_items;
	struct list_head done_free;
	spinlock_t done_lock; /* guards done_free */
};

#endif /* _ZCOMP_EH_H_ */
