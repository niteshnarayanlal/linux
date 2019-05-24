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
 * @nbits:		Indicates the total size of the bitmap in bits allocated
 *			at the time of initialization.
 */
struct hinting_bitmap {
	unsigned long *bitmap;
	unsigned long base_pfn;
	atomic_t free_mem_cnt;
	unsigned long nbits;
} bm_zone[MAX_NR_ZONES];

static void init_hinting_wq(struct work_struct *work);
struct hinting_cb *hcb;
struct work_struct hinting_work;

static unsigned long find_bitmap_size(struct zone *zone)
{
	unsigned long nbits = ALIGN(zone->spanned_pages,
			    PAGE_HINTING_MIN_ORDER);

	nbits = nbits >> PAGE_HINTING_MIN_ORDER;
	return nbits;
}

void page_hinting_enable(struct hinting_cb *callback)
{
	struct zone *zone;
	int idx = 0;
	unsigned long bitmap_size = 0;

	for_each_populated_zone(zone) {
		spin_lock(&zone->lock);
		bitmap_size = find_bitmap_size(zone);
		bm_zone[idx].bitmap = bitmap_zalloc(bitmap_size, GFP_KERNEL);
		if (!bm_zone[idx].bitmap)
			return;
		bm_zone[idx].nbits = bitmap_size;
		bm_zone[idx].base_pfn = zone->zone_start_pfn;
		spin_unlock(&zone->lock);
		idx++;
	}
	hcb = callback;
	INIT_WORK(&hinting_work, init_hinting_wq);
}
EXPORT_SYMBOL_GPL(page_hinting_enable);

void page_hinting_disable(void)
{
	struct zone *zone;
	int idx = 0;

	cancel_work_sync(&hinting_work);
	hcb = NULL;
	for_each_populated_zone(zone) {
		spin_lock(&zone->lock);
		bitmap_free(bm_zone[idx].bitmap);
		bm_zone[idx].base_pfn = 0;
		bm_zone[idx].nbits = 0;
		atomic_set(&bm_zone[idx].free_mem_cnt, 0);
		spin_unlock(&zone->lock);
		idx++;
	}
}
EXPORT_SYMBOL_GPL(page_hinting_disable);

static unsigned long pfn_to_bit(struct page *page, int zonenum)
{
	unsigned long bitnr;

	bitnr = (page_to_pfn(page) - bm_zone[zonenum].base_pfn)
			 >> PAGE_HINTING_MIN_ORDER;
	return bitnr;
}

static void bm_set_pfn(struct page *page)
{
	unsigned long bitnr = 0;
	int zonenum = page_zonenum(page);
	struct zone *zone = page_zone(page);

	lockdep_assert_held(&zone->lock);
	bitnr = pfn_to_bit(page, zonenum);
	if (bm_zone[zonenum].bitmap &&
	    bitnr < bm_zone[zonenum].nbits &&
	    !test_and_set_bit(bitnr, bm_zone[zonenum].bitmap))
		atomic_inc(&bm_zone[zonenum].free_mem_cnt);
}

static void scan_hinting_bitmap(int zonenum)
{
}

static bool check_hinting_threshold(void)
{
	int zonenum = 0;

	for (; zonenum < MAX_NR_ZONES; zonenum++) {
		if (atomic_read(&bm_zone[zonenum].free_mem_cnt) >=
				HINTING_MEM_THRESHOLD)
			return true;
	}
	return false;
}

static void init_hinting_wq(struct work_struct *work)
{
	int zonenum = 0;

	for (; zonenum < MAX_NR_ZONES; zonenum++) {
		if (atomic_read(&bm_zone[zonenum].free_mem_cnt) >=
		    HINTING_MEM_THRESHOLD)
			scan_hinting_bitmap(zonenum);
	}
}

void page_hinting_enqueue(struct page *page, int order)
{
	if (hcb && order >= PAGE_HINTING_MIN_ORDER)
		bm_set_pfn(page);
	else
		return;

	if (check_hinting_threshold()) {
		int cpu = smp_processor_id();

		queue_work_on(cpu, system_wq, &hinting_work);
	}
}
