// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/sched/mm.h>

#include "zram_drv.h"
#include "zcomp_eh.h"

static struct kmem_cache *zcomp_cookie_cachep;

static int zcomp_flush(struct zcomp_eh *zcomp_eh)
{
	int err = 0;
	LIST_HEAD(req_list);

	spin_lock(&zcomp_eh->request_lock);
	list_splice_init(&zcomp_eh->request_list, &req_list);
	zcomp_eh->pend_request = 0;
	spin_unlock(&zcomp_eh->request_lock);

	while (!list_empty(&req_list)) {
		struct zcomp_cookie *cookie;

		cookie = list_last_entry(&req_list, struct zcomp_cookie, list);
		list_del(&cookie->list);
		eh_compress_page(zcomp_eh->eh_dev, cookie->page, cookie);
	}

	return err;
}

static void zcomp_unplug(struct blk_plug_cb *cb, bool from_schedule)
{
	zcomp_flush((struct zcomp_eh *)(cb->data));
	kfree(cb);
}

/*
 * Hand every batched request to the device, whoever is holding it.
 *
 * The request list is shared, so this releases cookies still sitting on
 * other tasks' plugs as well -- which is the point. eh_suspend() calls it,
 * because a task the freezer has stopped will never unplug, and anything it
 * is still holding would otherwise be stranded with the clock gated.
 */
static void zcomp_eh_drain(void *priv)
{
	zcomp_flush((struct zcomp_eh *)priv);
}

/*
 * If there is room in the batch, append the cookie to the request list and
 * return true; otherwise return false.
 *
 * The limit is checked under the same lock that takes the cookie, rather
 * than in a separate locked read beforehand. request_lock is shared by every
 * CPU submitting to this device, so the old check-then-append pair paid for
 * it twice on every page.
 */
static bool zcomp_append_request(struct zcomp_eh *zcomp_eh,
				 struct zcomp_cookie *cookie)
{
	bool appended = false;

	spin_lock(&zcomp_eh->request_lock);
	if (zcomp_eh->pend_request < ZCOMP_BLK_MAX_REQUEST_COUNT) {
		list_add(&cookie->list, &zcomp_eh->request_list);
		zcomp_eh->pend_request++;
		appended = true;
	}
	spin_unlock(&zcomp_eh->request_lock);

	return appended;
}

/*
 * The caller needs to hold cookie_pool.lock
 */
static bool refill_zcomp_cookie(struct zcomp_eh *zcomp_eh)
{
	int i, allocated = 0;
	struct zcomp_cookie *cookie;
	LIST_HEAD(local_list);

	/*
	 * Sleeping is fine here: alloc_zcomp_cookie() drops the pool lock
	 * before calling in, and cannot be reached from interrupt context.
	 * GFP_ATOMIC gave this a way to fail that would have propagated all
	 * the way up as a failed swap-out, which reclaim only retries.
	 */
	for (i = 0; i < BATCH_ZCOMP_REQUEST; i++) {
		cookie = kmem_cache_alloc(zcomp_cookie_cachep, GFP_KERNEL);
		if (!cookie)
			break;
		list_add(&cookie->list, &local_list);
		allocated++;
	}

	if (allocated) {
		spin_lock(&zcomp_eh->cookie_pool.lock);
		list_splice(&local_list, &zcomp_eh->cookie_pool.head);
		zcomp_eh->cookie_pool.count += allocated;
		spin_unlock(&zcomp_eh->cookie_pool.lock);
	}

	return allocated == 0;
}

static struct zcomp_cookie *alloc_zcomp_cookie(struct zcomp_eh *zcomp_eh)
{
	struct zcomp_cookie *cookie = NULL;

	WARN_ON(in_interrupt());

	spin_lock(&zcomp_eh->cookie_pool.lock);
	if (list_empty(&zcomp_eh->cookie_pool.head)) {
		spin_unlock(&zcomp_eh->cookie_pool.lock);
		if (refill_zcomp_cookie(zcomp_eh))
			return NULL;
		spin_lock(&zcomp_eh->cookie_pool.lock);
	}

	if (unlikely(list_empty(&zcomp_eh->cookie_pool.head)))
		goto out;

	cookie = list_first_entry(&zcomp_eh->cookie_pool.head,
					struct zcomp_cookie, list);
	list_del(&cookie->list);
	zcomp_eh->cookie_pool.count--;
out:
	spin_unlock(&zcomp_eh->cookie_pool.lock);

	return cookie;
}

static void free_zcomp_cookie(struct zcomp_eh *zcomp_eh, struct zcomp_cookie *cookie)
{
	LIST_HEAD(released);
	struct zcomp_cookie *victim;

	spin_lock(&zcomp_eh->cookie_pool.lock);
	list_add(&cookie->list, &zcomp_eh->cookie_pool.head);
	zcomp_eh->cookie_pool.count++;

	if (zcomp_eh->cookie_pool.count >= BATCH_ZCOMP_REQUEST * 2) {
		int i;

		/*
		 * Take the surplus off the pool but free it after dropping
		 * the lock. Freeing a whole batch with the lock held stalls
		 * every other CPU trying to allocate or return a cookie, and
		 * this runs on the compression thread's completion path.
		 */
		for (i = 0; i < BATCH_ZCOMP_REQUEST; i++) {
			victim = list_last_entry(&zcomp_eh->cookie_pool.head,
						 struct zcomp_cookie, list);
			list_del(&victim->list);
			zcomp_eh->cookie_pool.count--;
			list_add(&victim->list, &released);
		}
	}
	spin_unlock(&zcomp_eh->cookie_pool.lock);

	while ((victim = list_first_entry_or_null(&released,
						  struct zcomp_cookie, list))) {
		list_del(&victim->list);
		kmem_cache_free(zcomp_cookie_cachep, victim);
	}
}

static void init_zcomp_cookie_pool(struct zcomp_eh *zcomp_eh)
{
	INIT_LIST_HEAD(&zcomp_eh->cookie_pool.head);
	spin_lock_init(&zcomp_eh->cookie_pool.lock);
	zcomp_eh->cookie_pool.count = 0;
}

static void destroy_zcomp_cookie_pool(struct zcomp_eh *zcomp_eh)
{
	struct zcomp_cookie *cookie;

	spin_lock(&zcomp_eh->cookie_pool.lock);
	while (!list_empty(&zcomp_eh->cookie_pool.head)) {
		cookie = list_first_entry(&zcomp_eh->cookie_pool.head,
					struct zcomp_cookie, list);
		list_del(&cookie->list);
		kmem_cache_free(zcomp_cookie_cachep, cookie);
		zcomp_eh->cookie_pool.count--;
	}
	spin_unlock(&zcomp_eh->cookie_pool.lock);
}

static struct zcomp_eh_done *zcomp_eh_get_done(struct zcomp_eh *zcomp_eh)
{
	struct zcomp_eh_done *done;

	spin_lock(&zcomp_eh->done_lock);
	done = list_first_entry_or_null(&zcomp_eh->done_free,
					struct zcomp_eh_done, list);
	if (done)
		list_del(&done->list);
	spin_unlock(&zcomp_eh->done_lock);

	return done;
}

static void zcomp_eh_put_done(struct zcomp_eh *zcomp_eh,
			      struct zcomp_eh_done *done)
{
	spin_lock(&zcomp_eh->done_lock);
	list_add(&done->list, &zcomp_eh->done_free);
	spin_unlock(&zcomp_eh->done_lock);
}

static void zcomp_eh_done_work(struct work_struct *work)
{
	struct zcomp_eh_done *done = container_of(work, struct zcomp_eh_done,
						  work);
	struct zcomp_eh *zcomp_eh = done->zcomp_eh;
	unsigned int noreclaim;

	/*
	 * This work used to run on the compression thread, which holds
	 * PF_MEMALLOC. Nothing in it allocates, but it is on the swap-out
	 * completion path, where that guarantee is the difference between a
	 * slow write and a failed one. Keep it.
	 */
	noreclaim = memalloc_noreclaim_save();

	zcomp_publish_buffer(done->zram, done->index, done->handle, done->len,
			     zcomp_eh->prio);
	bio_endio(done->bio);
	free_zcomp_cookie(zcomp_eh, done->cookie);
	memalloc_noreclaim_restore(noreclaim);

	/*
	 * Last, so that a non-zero in-flight count still means the BIO is
	 * outstanding even while it is being completed here. This is what
	 * eh_suspend() waits on, so it must not be released before the BIO.
	 */
	eh_request_done(zcomp_eh->eh_dev);

	/*
	 * Returned after the request is released, so no second completion can
	 * pick this item up and queue work that is still running.
	 */
	zcomp_eh_put_done(zcomp_eh, done);
}

static bool zcomp_eh_compress_done(int error, void *buffer,
				   unsigned int size, void *priv)
{
	struct zcomp_cookie *cookie = priv;
	struct zram *zram = cookie->zram;
	struct zcomp_eh *zcomp_eh = cookie->zcomp_eh;
	u32 index = cookie->index;
	struct bio *bio = cookie->bio;
	struct page *page = cookie->page;
	unsigned long handle = 0;
	unsigned int len = 0;
	struct zcomp_eh_done *done;

	/*
	 * The store has to happen here. @buffer belongs to the descriptor
	 * ring slot this request just vacated and may be overwritten as soon
	 * as the completion returns, so nothing that reads it may be
	 * deferred.
	 */
	if (!error)
		error = zcomp_store_buffer(buffer, size, zram, page, &handle,
					   &len);

	if (unlikely(error)) {
		if (bio_op(bio) == REQ_OP_WRITE)
			atomic64_inc(&zram->stats.failed_writes);
		else
			atomic64_inc(&zram->stats.failed_reads);
		bio_io_error(bio);
		free_zcomp_cookie(zcomp_eh, cookie);
		return false;
	}

	done = zcomp_eh_get_done(zcomp_eh);
	if (!done) {
		/*
		 * Pool exhausted. Finish inline rather than fail the write --
		 * reclaim only retries the same page.
		 */
		zcomp_publish_buffer(zram, index, handle, len, zcomp_eh->prio);
		bio_endio(bio);
		free_zcomp_cookie(zcomp_eh, cookie);
		return false;
	}

	done->zcomp_eh = zcomp_eh;
	done->zram = zram;
	done->cookie = cookie;
	done->bio = bio;
	done->handle = handle;
	done->len = len;
	done->index = index;

	INIT_WORK(&done->work, zcomp_eh_done_work);
	queue_work(zcomp_eh->done_wq, &done->work);

	return true;
}

static int zcomp_eh_compress(struct zcomp *comp, u32 index, struct page *page,
				struct bio *bio)
{
	struct zcomp_eh *zcomp_eh = comp->private;
	struct zcomp_cookie *cookie;

	cookie = alloc_zcomp_cookie(zcomp_eh);
	if (!cookie)
		return -ENOMEM;

	cookie->zram = comp->zram;
	cookie->zcomp_eh = zcomp_eh;
	cookie->index = index;
	cookie->page = page;
	cookie->bio = bio;

	bio_inc_remaining(bio);

	if (blk_check_plugged(zcomp_unplug, zcomp_eh, sizeof(struct blk_plug_cb)) &&
	    zcomp_append_request(zcomp_eh, cookie))
		return 0;

	zcomp_flush(zcomp_eh);
	return eh_compress_page(zcomp_eh->eh_dev, page, cookie);
}

static int zcomp_eh_decompress(struct zcomp *comp, void *src,
			unsigned int src_len, struct page *page)
{
	struct zcomp_eh *zcomp_eh = comp->private;

	return eh_decompress_page(zcomp_eh->eh_dev, src, src_len, page);
}

static void zcomp_eh_destroy(struct zcomp *comp)
{
	struct zcomp_eh *zcomp_eh = comp->private;

	/*
	 * Drains, so every deferred completion has finished and returned its
	 * item before the pool and the cookies behind it are freed.
	 */
	destroy_workqueue(zcomp_eh->done_wq);
	eh_destroy(zcomp_eh->eh_dev);
	destroy_zcomp_cookie_pool(zcomp_eh);
	kfree(zcomp_eh->done_items);
	kfree(zcomp_eh);
	module_put(THIS_MODULE);
}

static int zcomp_eh_init_done(struct zcomp_eh *zcomp_eh)
{
	int nr_items = num_possible_cpus() * ZCOMP_EH_DONE_PER_CPU;
	int i;

	zcomp_eh->done_items = kcalloc(nr_items, sizeof(struct zcomp_eh_done),
				       GFP_KERNEL);
	if (!zcomp_eh->done_items)
		return -ENOMEM;

	INIT_LIST_HEAD(&zcomp_eh->done_free);
	spin_lock_init(&zcomp_eh->done_lock);
	for (i = 0; i < nr_items; i++)
		list_add(&zcomp_eh->done_items[i].list, &zcomp_eh->done_free);

	/*
	 * Unbound, because every completion is handed off from the single
	 * compression thread -- a bound queue would run them all back on
	 * that one CPU and defeat the point.
	 *
	 * Not WQ_FREEZABLE: the suspend path waits on the in-flight count
	 * that these workers release, so freezing them there would deadlock.
	 */
	zcomp_eh->done_wq = alloc_workqueue("zcomp_eh_done",
					    WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!zcomp_eh->done_wq) {
		kfree(zcomp_eh->done_items);
		zcomp_eh->done_items = NULL;
		return -ENOMEM;
	}

	return 0;
}

static int zcomp_eh_create(struct zcomp *comp, const char *name)
{
	struct zcomp_eh *zcomp_eh;

	zcomp_eh = kmalloc(sizeof(*zcomp_eh), GFP_KERNEL);
	if (!zcomp_eh)
		return -ENOMEM;

	init_zcomp_cookie_pool(zcomp_eh);
	INIT_LIST_HEAD(&zcomp_eh->request_list);
	spin_lock_init(&zcomp_eh->request_lock);
	zcomp_eh->pend_request = 0;
	zcomp_eh->prio = comp->prio;
	zcomp_eh->done_items = NULL;

	if (zcomp_eh_init_done(zcomp_eh)) {
		kfree(zcomp_eh);
		return -ENOMEM;
	}

	zcomp_eh->eh_dev = eh_create(zcomp_eh_compress_done, zcomp_eh_drain,
				     zcomp_eh);
	if (IS_ERR(zcomp_eh->eh_dev)) {
		destroy_workqueue(zcomp_eh->done_wq);
		kfree(zcomp_eh->done_items);
		kfree(zcomp_eh);
		return -ENODEV;
	}

	comp->private = zcomp_eh;
	__module_get(THIS_MODULE);

	return 0;
}

const struct zcomp_operation zcomp_eh_op = {
	.create = zcomp_eh_create,
	.destroy = zcomp_eh_destroy,
	.compress_async = zcomp_eh_compress,
	.decompress = zcomp_eh_decompress,
};

static int __init zcomp_eh_init(void)
{
	int ret;

	zcomp_cookie_cachep = kmem_cache_create("zcomp_cookie",
						sizeof(struct zcomp_cookie),
						0, 0, NULL);
	if (!zcomp_cookie_cachep)
		return -ENOMEM;

	ret = zcomp_register("lz77eh", &zcomp_eh_op);
	if (ret)
		kmem_cache_destroy(zcomp_cookie_cachep);

	return ret;
}

static void __exit zcomp_eh_exit(void)
{
	zcomp_unregister("lz77eh");
	kmem_cache_destroy(zcomp_cookie_cachep);
}

module_init(zcomp_eh_init);
module_exit(zcomp_eh_exit);
MODULE_LICENSE("GPL");
