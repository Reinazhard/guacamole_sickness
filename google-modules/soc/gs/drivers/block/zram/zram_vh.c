// SPDX-License-Identifier: GPL-2.0-or-later

#define KMSG_COMPONENT "zram_vh"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/blkdev.h>
#include <linux/pagemap.h>
#include <trace/hooks/mm.h>

#include "zram_drv.h"
#include "zram_vh.h"

/*
 * After completing I/O on a page, call this routine to update the page
 * flags appropriately
 */
static void zram_read_page_end_io(struct page *page)
{
	struct folio *folio = page_folio(page);

	folio_mark_uptodate(folio);
	folio_unlock(folio);
}

static void rvh_swap_readpage_bdev_sync(void *data, struct block_device *bdev,
					sector_t sector, struct page *page,
					bool *read)
{
	u32 index;
	struct zram *zram;
	int ret;

	if (PageTransHuge(page))
		return;

	zram = bdev->bd_disk->private_data;
	index = sector >> SECTORS_PER_PAGE_SHIFT;

	ret = zram_read_page(zram, page, index, NULL);
	/* fallback to bio path for ZRAM_WB and error cases */
	if (ret)
		return;

	flush_dcache_page(page);
	zram_slot_lock(zram, index);
	zram_accessed(zram, index);
	zram_slot_unlock(zram, index);
	zram_read_page_end_io(page);
	*read = true;
}

int zram_vh_init(void)
{
	int ret = 0;

	ret = register_trace_android_rvh_swap_readpage_bdev_sync(
		rvh_swap_readpage_bdev_sync, NULL);
	if (ret)
		pr_err("register swap_readpage_bdev_sync failed\n");

	return ret;
}

