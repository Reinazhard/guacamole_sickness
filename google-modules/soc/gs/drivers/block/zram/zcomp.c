// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/bio.h>
#include <linux/swap.h>

#include "zram_drv.h"

#define CREATE_TRACE_POINTS
#include <trace/events/zram.h>

/*
 * Pages that compress to sizes equals or greater than this are stored
 * uncompressed in memory.
 */
static size_t huge_class_size = 0;

struct zcomp_backend {
	const char algo_name[ZCOMP_ALGO_NAME_MAX];
	struct zcomp_operation *op;
};

/* zcomp_backend list registered by zcomp instances */
static LIST_HEAD(zcomp_list);
static DECLARE_RWSEM(zcomp_rwsem);

/*
 * The read path stages each compressed object through a scratch page before
 * handing it to the backend, so it needs one page per CPU to do that without
 * allocating. zcomp is registered once per algorithm, so this is a handful of
 * pages for the lifetime of the module.
 */
static void zcomp_free_scratch(struct zcomp *zcomp)
{
	int cpu;

	if (!zcomp->scratch)
		return;

	for_each_possible_cpu(cpu) {
		unsigned long buf = *per_cpu_ptr(zcomp->scratch, cpu);

		if (buf)
			free_pages(buf, 0);
	}
	free_percpu(zcomp->scratch);
	zcomp->scratch = NULL;
}

static int zcomp_alloc_scratch(struct zcomp *zcomp)
{
	int cpu;

	zcomp->scratch = alloc_percpu(unsigned long);
	if (!zcomp->scratch)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		unsigned long buf = __get_free_pages(GFP_KERNEL, 0);

		if (!buf) {
			zcomp_free_scratch(zcomp);
			return -ENOMEM;
		}
		*per_cpu_ptr(zcomp->scratch, cpu) = buf;
	}

	return 0;
}

/* caller should hold a zcomp_rwsem under semaphore */
static struct zcomp *find_zcomp(const char *algo_name)
{
	struct zcomp *cursor, *ret = NULL;

	list_for_each_entry(cursor, &zcomp_list, list) {
		if (!strcmp(cursor->algo_name, algo_name)) {
			ret = cursor;
			break;
		}
	}

	return ret;
}

int zcomp_register(const char *algo_name, const struct zcomp_operation *op)
{
	struct zcomp *zcomp;
	size_t len;
	int ret = 0;

	if (!algo_name || !op)
		return -EINVAL;

	len = strlen(algo_name);
	if (len >= ZCOMP_ALGO_NAME_MAX)
		return -EINVAL;

	zcomp = kzalloc(sizeof(*zcomp), GFP_KERNEL);
	if (!zcomp) {
		ret = -ENOMEM;
		goto out;
	}

	ret = zcomp_alloc_scratch(zcomp);
	if (ret) {
		kfree(zcomp);
		goto out;
	}

	strncpy(zcomp->algo_name, algo_name, len);
	zcomp->algo_name[len] = '\0';
	zcomp->op = op;

	down_write(&zcomp_rwsem);
	if (find_zcomp(algo_name)) {
		up_write(&zcomp_rwsem);
		zcomp_free_scratch(zcomp);
		kfree(zcomp);
		ret = -EEXIST;
		goto out;
	}

	list_add(&zcomp->list, &zcomp_list);
	up_write(&zcomp_rwsem);
out:
	return ret;
}
EXPORT_SYMBOL(zcomp_register);

int zcomp_unregister(const char *algo_name)
{
	int ret = -EINVAL;
	struct zcomp *cursor;

	down_write(&zcomp_rwsem);
	list_for_each_entry(cursor, &zcomp_list, list) {
		if (strcmp(cursor->algo_name, algo_name))
			continue;

		list_del(&cursor->list);
		zcomp_free_scratch(cursor);
		kfree(cursor);
		ret = 0;
		break;
	}
	up_write(&zcomp_rwsem);

	return ret;

}
EXPORT_SYMBOL(zcomp_unregister);

static bool zcomp_page_same_pattern(struct page *page, unsigned long *element)
{
	unsigned int pos;
	unsigned long *mem;
	unsigned long val;

	mem = kmap_local_page(page);
	val = mem[0];
	for (pos = 1; pos < PAGE_SIZE / sizeof(*mem); pos++) {
		if (val != mem[pos]) {
			kunmap_local(mem);
			return false;
		}
	}

	*element = val;
	kunmap_local(mem);
	return true;
}

bool zcomp_available_algorithm(const char *algo_name)
{
	bool found;

	down_read(&zcomp_rwsem);
	found = find_zcomp(algo_name);
	up_read(&zcomp_rwsem);

	return found;
}

/* show available compressors */
ssize_t zcomp_available_show(const char *comp, char *buf)
{
	bool known_algorithm = false;
	ssize_t sz = 0;
	struct zcomp *zcomp;

	down_read(&zcomp_rwsem);
	list_for_each_entry(zcomp, &zcomp_list, list) {
		if (!strcmp(comp, zcomp->algo_name)) {
			known_algorithm = true;
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"[%s] ", zcomp->algo_name);
		} else {
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"%s ", zcomp->algo_name);
		}
	}
	sz += scnprintf(buf + sz, PAGE_SIZE - sz - 1, "%c", '\n');
	up_read(&zcomp_rwsem);

	/*
	 * XXX: handle Out-of-tree module known to crypto api or a
	 * mssing entry in backends'.
	 */
	return sz;
}

int zcomp_compress(struct zcomp *comp, u32 index, struct page *page,
			struct bio *bio)
{
	unsigned long element;

	if (unlikely(zcomp_page_same_pattern(page, &element))) {
		zram_slot_update(comp->zram, index, element, 0, comp->prio);
		return 0;
	}

	if (comp->op->compress_async)
		return comp->op->compress_async(comp, index, page, bio);

	return comp->op->compress(comp, index, page, bio);
}

void zcomp_prepare_decompress(struct zcomp *comp)
{
	if (comp && comp->op->prepare_decompress)
		comp->op->prepare_decompress(comp);
}

int zcomp_decompress_buf(struct zcomp *comp, u32 index, void *src,
			 unsigned int size, struct page *page)
{
	int ret;

	trace_zcomp_decompress_start(page, index);
	ret = comp->op->decompress(comp, src, size, page);
	trace_zcomp_decompress_end(page, index);

	return ret;
}

int zcomp_decompress(struct zcomp *comp, u32 index, struct page *page)
{
	unsigned long handle;
	unsigned int src_len;
	struct zram *zram = comp->zram;
	struct zram_table_entry *entry = &zram->table[index];
	void *scratch, *src;
	int ret;

	handle = entry->handle;
	src_len = entry->flags & (BIT(ZRAM_FLAG_SHIFT) - 1);

	/*
	 * Stage the object in the per-CPU scratch page and let go of zsmalloc
	 * before calling the backend, rather than decompressing straight out
	 * of the mapping.
	 *
	 * zs_obj_read_begin() holds the zspage read lock and a kmap until
	 * zs_obj_read_end(). The EH backend reaches its result by polling
	 * hardware, so decompressing in place pins both for the whole poll,
	 * stalling compaction on that zspage and holding a kmap slot across
	 * an arbitrary wait.
	 *
	 * The copy is not wasted work: a zsmalloc object is always at an
	 * offset of 8 mod 16 (class sizes are multiples of 16, plus
	 * ZS_HANDLE_SIZE), so the backend would bounce it through one of its
	 * own buffers anyway. Scratch is page aligned, so handing it over
	 * up front is one copy instead of two.
	 *
	 * Preemption stays off so the scratch page and the kmap below belong
	 * to this CPU throughout.
	 */
	preempt_disable();
	scratch = (void *)*this_cpu_ptr(comp->scratch);

	src = zs_obj_read_begin(zram->mem_pool, handle, src_len, scratch);
	if (src != scratch)
		memcpy(scratch, src, src_len);
	zs_obj_read_end(zram->mem_pool, handle, src_len, src);

	ret = zcomp_decompress_buf(comp, index, scratch, src_len, page);
	preempt_enable();

	return ret;
}

bool zcomp_has_recompress(const char *algo_name)
{
	struct zcomp *zcomp;
	bool ret = false;

	down_read(&zcomp_rwsem);
	zcomp = find_zcomp(algo_name);
	ret = zcomp && zcomp->op->recompress;
	up_read(&zcomp_rwsem);

	return ret;
}

int zcomp_recompress(struct zcomp *comp, u32 index, struct page *page,
		     u32 prio, u32 threshold)
{
	if (!comp->op->recompress)
		return -ENOSYS;

	return comp->op->recompress(comp, index, page, prio, threshold);
}

void zcomp_destroy(struct zcomp *comp)
{
	comp->op->destroy(comp);
}

/*
 * search available compressors for requested algorithm.
 * allocate new zcomp and initialize it. return compressing
 * backend pointer or ERR_PTR if things went bad. ERR_PTR(-EINVAL)
 * if requested algorithm is not supported, ERR_PTR(-ENOMEM) in
 * case of allocation error, or any other error potentially
 * returned by zcomp_create().
 */
struct zcomp *zcomp_create(const char *algo_name, struct zcomp_params *params,
			   struct zram *zram, u32 prio)
{
	struct zcomp *comp;
	int error;

	down_read(&zcomp_rwsem);
	comp = find_zcomp(algo_name);
	if (!comp) {
		up_read(&zcomp_rwsem);
		return ERR_PTR(-EINVAL);
	}

	/* assign the params before comp->op->create */
	comp->params = params;
	comp->prio = prio;

	error = comp->op->create(comp, algo_name);
	if (error) {
		up_read(&zcomp_rwsem);
		return ERR_PTR(error);
	}

	comp->zram = zram;
	up_read(&zcomp_rwsem);

	if (!huge_class_size)
		huge_class_size = zs_huge_class_size(zram->mem_pool);

	return comp;
}

/*
 * Once zcomp instance finishes the compression, it need to copy the compressed
 * buffer to zram's memory space.
 *
 * @buffer: memory address compressed objecd is stored
 * @comp_len: compressed object size
 * @zram: zram instance
 * @page: original buffer to be compressed
 * @index: swap slot index
 */
int zcomp_copy_buffer(void *buffer, int comp_len, struct zram *zram,
		      struct page *page, u32 index, u32 prio)
{

	unsigned long handle;

	if (unlikely(comp_len == 0)) {
		zram_slot_update(zram, index, 0, 0, prio);
		return 0;
	}

	if (comp_len >= huge_class_size)
		comp_len = PAGE_SIZE;

	handle = zs_malloc(zram->mem_pool, comp_len,
			__GFP_KSWAPD_RECLAIM |
			__GFP_NOWARN |
			__GFP_HIGHMEM |
			__GFP_MOVABLE |
			__GFP_CMA,
			NUMA_NO_NODE);
	if (unlikely(IS_ERR_VALUE(handle)))
		return PTR_ERR((void *)handle);

	if (comp_len == PAGE_SIZE) {
		void *src = kmap_local_page(page);

		zs_obj_write(zram->mem_pool, handle, src, comp_len);
		kunmap_local(src);
	} else {
		zs_obj_write(zram->mem_pool, handle, buffer, comp_len);
	}
	zram_slot_update(zram, index, handle, comp_len, prio);

	return 0;
}
EXPORT_SYMBOL(zcomp_copy_buffer);

/*
 * Similar to zcomp_copy_buffer. The index was already locked during the
 * recompress process.
 *
 * Once zcomp instance finishes the recompression, it need to copy the
 * new compressed buffer to zram's memory space.
 * There are two differences:
 * 1. If the recompressed size is not smaller than original size, or not
 * smaller than the threshold, we won't copy the data.
 * 2. No direct reclaim in the recompression path.
 *
 * @buffer: memory address compressed objecd is stored
 * @comp_len_new: the recompressed object size
 * @zram: zram instance
 * @index: swap slot index
 * @prio: the recompress algorithm index
 * @threshold: the max recompressed object size we accept.
 */
int zcomp_recompress_copy_buffer(void *buffer, int comp_len_new,
				 struct zram *zram, u32 index,
				 u32 prio, u32 threshold)
{
	unsigned int comp_len_old;
	unsigned int class_index_old;
	unsigned int class_index_new;
	unsigned long handle_new;


	comp_len_old = zram_get_obj_size(zram, index);
	class_index_old = zs_lookup_class_index(zram->mem_pool, comp_len_old);
	class_index_new = zs_lookup_class_index(zram->mem_pool, comp_len_new);

	/* Try next prio until we make progress */
	if (class_index_new >= class_index_old ||
	    (threshold && comp_len_new >= threshold))
		return -EAGAIN;

	/*
	 * No direct reclaim (slow path) for handle allocation and no
	 * re-compression attempt (unlike in zram_write_bvec()) since
	 * we already have stored that object in zsmalloc. If we cannot
	 * alloc memory for recompressed object then we bail out and
	 * simply keep the old (existing) object in zsmalloc.
	 */
	handle_new = zs_malloc(zram->mem_pool, comp_len_new,
			       __GFP_KSWAPD_RECLAIM |
			       __GFP_NOWARN |
			       __GFP_HIGHMEM |
			       __GFP_MOVABLE,
			       NUMA_NO_NODE);
	if (IS_ERR_VALUE(handle_new))
		return PTR_ERR((void *)handle_new);

	zs_obj_write(zram->mem_pool, handle_new, buffer, comp_len_new);

	zram_recompress_slot_update(zram, index, handle_new, comp_len_new,
				    prio);

	return 0;
}
EXPORT_SYMBOL(zcomp_recompress_copy_buffer);

size_t get_huge_class_size(void)
{
	return huge_class_size;
}
