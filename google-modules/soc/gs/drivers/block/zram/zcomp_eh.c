// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>

#include "zcomp.h"
#include <linux/eh.h>
#include <linux/bio.h>
#include <linux/blkdev.h>

static int zcomp_flush(struct zcomp *comp)
{
	int err = 0;
	LIST_HEAD(req_list);


	spin_lock(&comp->request_lock);
	list_splice_init(&comp->request_list, &req_list);
	comp->pend_request = 0;
	spin_unlock(&comp->request_lock);

	while (!list_empty(&req_list)) {
		struct zcomp_cookie *cookie;

		cookie = list_last_entry(&req_list, struct zcomp_cookie, list);
		list_del(&cookie->list);
		eh_compress_page(comp->private, cookie->page, cookie);
	}

	return err;
}
static void zcomp_unplug(struct blk_plug_cb *cb, bool from_schedule)
{
	zcomp_flush((struct zcomp *)(cb->data));
	kfree(cb);
}

/*
 * If the comp is plugged, append the cookie to request list and return true
 * otherwise, return false.
 */
static void zcomp_append_request(struct zcomp *comp, struct zcomp_cookie *cookie)
{
	spin_lock(&comp->request_lock);
	list_add(&cookie->list, &comp->request_list);
	comp->pend_request++;
	spin_unlock(&comp->request_lock);
}

static unsigned long nr_pend_request(struct zcomp *comp)
{
	unsigned long ret;

	spin_lock(&comp->request_lock);
	ret = comp->pend_request;
	spin_unlock(&comp->request_lock);

	return ret;
}

/*
 * The caller needs to hold cookie_pool.lock
 */
static bool refill_zcomp_cookie(struct zcomp *zcomp)
{
	int i;
	struct zcomp_cookie *cookie;

	WARN_ON(zcomp->cookie_pool.count != 0);

	for (i = 0; i < BATCH_ZCOMP_REQUEST; i++) {
		cookie = kmalloc(sizeof(struct zcomp_cookie), GFP_ATOMIC);
		if (!cookie)
			break;
		list_add(&cookie->list, &zcomp->cookie_pool.head);
		zcomp->cookie_pool.count++;
	}

	return !zcomp->cookie_pool.count;
}

static struct zcomp_cookie *alloc_zcomp_cookie(struct zcomp *zcomp)
{
	struct zcomp_cookie *cookie = NULL;

	WARN_ON(in_interrupt());

	spin_lock(&zcomp->cookie_pool.lock);
	if (list_empty(&zcomp->cookie_pool.head)) {
		if (refill_zcomp_cookie(zcomp))
			goto out;
	}

	cookie = list_first_entry(&zcomp->cookie_pool.head,
					struct zcomp_cookie, list);
	list_del(&cookie->list);
	zcomp->cookie_pool.count--;
out:
	spin_unlock(&zcomp->cookie_pool.lock);

	return cookie;
}

void free_zcomp_cookie(struct zcomp *zcomp, struct zcomp_cookie *cookie)
{
	spin_lock(&zcomp->cookie_pool.lock);
	list_add(&cookie->list, &zcomp->cookie_pool.head);
	zcomp->cookie_pool.count++;

	if (zcomp->cookie_pool.count >= BATCH_ZCOMP_REQUEST * 2) {
		int i;

		for (i = 0; i < BATCH_ZCOMP_REQUEST; i++) {
			cookie = list_last_entry(&zcomp->cookie_pool.head,
						struct zcomp_cookie, list);
			list_del(&cookie->list);
			kfree(cookie);
			zcomp->cookie_pool.count--;
		}
	}
	spin_unlock(&zcomp->cookie_pool.lock);
}

static void init_zcomp_cookie_pool(struct zcomp *zcomp)
{
	INIT_LIST_HEAD(&zcomp->cookie_pool.head);
	spin_lock_init(&zcomp->cookie_pool.lock);
	zcomp->cookie_pool.count = 0;
}

static void destroy_zcomp_cookie_pool(struct zcomp *zcomp)
{
	struct zcomp_cookie *cookie;

	spin_lock(&zcomp->cookie_pool.lock);
	while (!list_empty(&zcomp->cookie_pool.head)) {
		cookie = list_first_entry(&zcomp->cookie_pool.head,
					struct zcomp_cookie, list);
		list_del(&cookie->list);
		kfree(cookie);
		zcomp->cookie_pool.count--;
	}
	spin_unlock(&zcomp->cookie_pool.lock);
}

static void zcomp_eh_compress_done(int error, void *buffer,
				   unsigned int size, void *priv)
{
	struct zcomp_cookie *cookie = priv;
	struct zram *zram = cookie->zram;
	u32 index = cookie->index;
	struct bio *bio = cookie->bio;
	struct page *page = cookie->page;
	bool is_write = bio_op(bio) == REQ_OP_WRITE;

	if (!error)
		error = zcomp_copy_buffer(buffer, size, zram, page, index);

	if (unlikely(error)) {
		if (is_write)
			this_cpu_inc(zram->pcp_stats->items[NR_FAILED_WRITE]);
		else
			this_cpu_inc(zram->pcp_stats->items[NR_FAILED_READ]);
		bio_io_error(bio);
	} else
		bio_endio(bio);
	free_zcomp_cookie(zram->comp, cookie);
}

static int zcomp_eh_compress(struct zcomp *comp, u32 index, struct page *page,
				struct bio *bio)
{
	struct zcomp_cookie *cookie;

	cookie = alloc_zcomp_cookie(comp);
	if (!cookie)
		return -ENOMEM;

	cookie->zram = comp->zram;
	cookie->index = index;
	cookie->page = page;
	cookie->bio = bio;

	bio_inc_remaining(bio);

	if (blk_check_plugged(zcomp_unplug, comp, sizeof(struct blk_plug_cb)) &&
	    nr_pend_request(comp) < ZCOMP_BLK_MAX_REQUEST_COUNT) {
		zcomp_append_request(comp, cookie);
		return 0;
	}

	zcomp_flush(comp);
	return eh_compress_page(comp->private, page, cookie);
}

static int zcomp_eh_decompress(struct zcomp *comp, void *src,
			unsigned int src_len, struct page *page)
{
	return eh_decompress_page(comp->private, src, src_len, page);
}

static void zcomp_eh_destroy(struct zcomp *comp)
{
	eh_destroy(comp->private);
	destroy_zcomp_cookie_pool(comp);
	module_put(THIS_MODULE);
}

static int zcomp_eh_create(struct zcomp *comp, const char *name)
{
	struct eh_device *eh_dev = eh_create(zcomp_eh_compress_done);

	if (IS_ERR(eh_dev))
		return -ENODEV;

	init_zcomp_cookie_pool(comp);
	INIT_LIST_HEAD(&comp->request_list);
	spin_lock_init(&comp->request_lock);
	comp->pend_request = 0;

	comp->private = eh_dev;
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
	return zcomp_register("lz77eh", &zcomp_eh_op);
}

static void __exit zcomp_eh_exit(void)
{
	zcomp_unregister("lz77eh");
}

module_init(zcomp_eh_init);
module_exit(zcomp_eh_exit);
MODULE_LICENSE("GPL");
