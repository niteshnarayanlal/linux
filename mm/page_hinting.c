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

unsigned long find_bitmap_size(struct zone *zone)
{
	unsigned long nbits = ALIGN(zone->spanned_pages,
			    PAGE_HINTING_MIN_ORDER);

	nbits = nbits >> PAGE_HINTING_MIN_ORDER;
	return nbits;
}

void page_hinting_enable(void)
{
	struct zone *zone;
	int idx = 0;
	unsigned long bitmap_size = 0;

	for_each_populated_zone(zone) {
		bitmap_size = find_bitmap_size(zone);
		bm_zone[idx].bitmap = bitmap_zalloc(bitmap_size, GFP_KERNEL);
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
	for_each_populated_zone(zone) {
		bitmap_free(bm_zone[idx].bitmap);
		bm_zone[idx].base_pfn = 0;
		bm_zone[idx].bm_size = 0;
		atomic_set(&bm_zone[idx].free_mem_cnt, 0);
		idx++;
	}
}
EXPORT_SYMBOL_GPL(page_hinting_disable);

unsigned long pfn_to_bit(struct page *page, int zonenum)
{
	unsigned long bitmap_no;

	bitmap_no = (page_to_pfn(page) - bm_zone[zonenum].base_pfn)
			 >> PAGE_HINTING_MIN_ORDER;
	return bitmap_no;
}

void bm_set_pfn(struct page *page)
{
	unsigned long bitmap_no = 0;
	int zonenum = page_zonenum(page);

	bitmap_no = pfn_to_bit(page, zonenum);
	if (bm_zone[zonenum].bitmap &&
	    bitmap_no <= bm_zone[zonenum].bm_size &&
	    !test_bit(bitmap_no, bm_zone[zonenum].bitmap)) {
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

	for (; zonenum < MAX_NR_ZONES; zonenum++) {
		if (atomic_read(&bm_zone[zonenum].free_mem_cnt) >=
				HINTING_MEM_THRESHOLD)
			return true;
	}
	return false;
}

void init_hinting_wq(struct work_struct *work)
{
	int zonenum = 0;

	for (; zonenum < MAX_NR_ZONES; zonenum++) {
		if (atomic_read(&bm_zone[zonenum].free_mem_cnt) >=
		    HINTING_MEM_THRESHOLD)
			scan_hinting_bitmap(zonenum);
	}
}

#ifdef CONFIG_PAGE_HINTING
void page_hinting_enqueue(struct page *page, int order)
{
	if (PageBuddy(page) && order >= PAGE_HINTING_MIN_ORDER)
		bm_set_pfn(page);
	if (check_hinting_threshold()) {
		int cpu = smp_processor_id();

		queue_work_on(cpu, system_wq, &hinting_work);
	}
}
#else
void page_hinting_enqueue(struct page *page, int order)
{
}
#endif
