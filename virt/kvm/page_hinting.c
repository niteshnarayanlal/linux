// SPDX-License-Identifier: GPL-2.0
/*
 * Free page hinting support to enable a VM to report the freed pages back
 * to the host.
 *
 * Copyright Red Hat, Inc. 2019
 *
 * Author(s): Nitesh Narayan Lal <nitesh@redhat.com>
 */

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/page_hinting.h>
#include <linux/kvm_host.h>

/*
 * struct hinting_bitmap: holds the bitmap pointer which tracks the freed PFNs
 * and other required variables which could help in retrieving the original PFN
 * value using the bitmap.
 * @bitmap:		Pointer to the bitmap of free PFN.
 * @base_pfn:		Starting PFN value for the zone whose bitmap is stored.
 * @free_mem_cnt:	Tracks the total amount of memory freed corresponding
 *			to the zone on a granularity of PAGE_HINTING_MIN_ORDER.
 * @bm_size:		Indicates the total size of the bitmap allocated at the
 *			time of initialization.
 */

struct hinting_bitmap {
	unsigned long *bitmap;
	unsigned long base_pfn;
	atomic_t free_mem_cnt;
	unsigned long bm_size;
} bm_zone[MAX_NR_ZONES];

struct work_struct hinting_work;
void init_hinting_wq(struct work_struct *work);

void page_hinting_enable(void)
{
	struct zone *zone;
	int idx = 0;
	unsigned long bitmap_size = 0;

	for_each_zone(zone) {
		bitmap_size = (zone->spanned_pages << PAGE_SHIFT) >>
				PAGE_HINTING_MIN_ORDER;
		bm_zone[idx].bitmap = kmalloc(bitmap_size, GFP_KERNEL);
		memset(bm_zone[idx].bitmap, 0, bitmap_size);
		bm_zone[idx].bm_size = bitmap_size;
		bm_zone[idx].base_pfn = zone->zone_start_pfn;
		idx++;
	}
	INIT_WORK(&hinting_work, init_hinting_wq);
}
EXPORT_SYMBOL_GPL(page_hinting_enable);

void page_hinting_disable(void)
{
	struct zone *zone;
	int idx = 0;

	cancel_work_sync(&hinting_work);
	for_each_zone(zone)
		kfree(bm_zone[idx++].bitmap);
}
EXPORT_SYMBOL_GPL(page_hinting_disable);

struct page *get_buddy_page(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned int order;

	for (order = 0; order < MAX_ORDER; order++) {
		struct page *page_head = page - (pfn & ((1 << order) - 1));

		if (PageBuddy(page_head) && page_private(page_head) >= order)
			return page_head;
	}
	return NULL;
}

void bm_set_pfn(struct page *page)
{
	int bitmap_no = 0;
	int zonenum = page_zonenum(page);

	bitmap_no = (page_to_pfn(page) - bm_zone[zonenum].base_pfn) >>
			PAGE_HINTING_MIN_ORDER;
	if (bitmap_no <= bm_zone[zonenum].bm_size) {
		bitmap_set(bm_zone[zonenum].bitmap, bitmap_no, 1);
		atomic_inc(&bm_zone[zonenum].free_mem_cnt);
	}
}

static void scan_hinting_bitmap(int zonenum)
{
}

bool check_hinting_threshold(void)
{
	int zonenum = 0;

	while (zonenum < MAX_NR_ZONES) {
		if (atomic_read(&bm_zone[zonenum].free_mem_cnt) >=
				HINTING_MEM_THRESHOLD)
			return true;
		zonenum++;
	}
	return false;
}

void init_hinting_wq(struct work_struct *work)
{
	int zonenum = 0;

	while (zonenum < MAX_NR_ZONES) {
		if (atomic_read(&bm_zone[zonenum].free_mem_cnt) >=
		    HINTING_MEM_THRESHOLD) {
			atomic_sub(HINTING_MEM_THRESHOLD,
				   &bm_zone[zonenum].free_mem_cnt);
			scan_hinting_bitmap(zonenum);
		}
		zonenum++;
	}
}

void page_hinting_enqueue(struct page *page, int order)
{
	if (PageBuddy(page) && order >= PAGE_HINTING_MIN_ORDER) {
		bm_set_pfn(page);
	} else {
		struct page *buddy_page = get_buddy_page(page);

		if (buddy_page && page_private(buddy_page) >=
		    PAGE_HINTING_MIN_ORDER)
			bm_set_pfn(buddy_page);
	}
	if (check_hinting_threshold())
		queue_work(system_wq, &hinting_work);
}

