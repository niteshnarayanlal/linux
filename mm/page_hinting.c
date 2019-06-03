// SPDX-License-Identifier: GPL-2.0
/*
 * Page hinting support to enable a VM to report the freed pages back
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
 * and other required parameters which could help in retrieving the original
 * PFN value using the bitmap.
 * @bitmap:		Pointer to the bitmap of free PFN.
 * @base_pfn:		Starting PFN value for the zone whose bitmap is stored.
 * @free_pages:		Tracks the number of free pages of granularity
 *			PAGE_HINTING_MIN_ORDER.
 * @nbits:		Indicates the total size of the bitmap in bits allocated
 *			at the time of initialization.
 */
struct hinting_bitmap {
	unsigned long *bitmap;
	unsigned long base_pfn;
	atomic_t free_pages;
	unsigned long nbits;
} bm_zone[MAX_NR_ZONES];

static void init_hinting_wq(struct work_struct *work);
extern int __isolate_free_page(struct page *page, unsigned int order);
extern void __free_one_page(struct page *page, unsigned long pfn,
			    struct zone *zone, unsigned int order,
			    int migratetype, bool hint);
const struct page_hinting_cb *hcb;
struct work_struct hinting_work;

static unsigned long find_bitmap_size(struct zone *zone)
{
	unsigned long nbits = ALIGN(zone->spanned_pages,
			    PAGE_HINTING_MIN_ORDER);

	nbits = nbits >> PAGE_HINTING_MIN_ORDER;
	return nbits;
}

void page_hinting_enable(const struct page_hinting_cb *callback)
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
		atomic_set(&bm_zone[idx].free_pages, 0);
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

static void release_buddy_pages(struct list_head *pages)
{
	int mt = 0, zonenum, order;
	struct page *page, *next;
	struct zone *zone;
	unsigned long bitnr;

	list_for_each_entry_safe(page, next, pages, lru) {
		zonenum = page_zonenum(page);
		zone = page_zone(page);
		bitnr = pfn_to_bit(page, zonenum);
		spin_lock(&zone->lock);
		list_del(&page->lru);
		order = page_private(page);
		set_page_private(page, 0);
		mt = get_pageblock_migratetype(page);
		__free_one_page(page, page_to_pfn(page), zone,
				order, mt, false);
		spin_unlock(&zone->lock);
	}
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
		atomic_inc(&bm_zone[zonenum].free_pages);
}

static void scan_hinting_bitmap(int zonenum, int free_pages)
{
	unsigned long set_bit, start = 0;
	struct page *page;
	struct zone *zone;
	int scanned_pages = 0, ret = 0, order, isolated_cnt = 0;
	LIST_HEAD(isolated_pages);

	ret = hcb->prepare();
	if (ret < 0)
		return;
	for (;;) {
		ret = 0;
		set_bit = find_next_bit(bm_zone[zonenum].bitmap,
					bm_zone[zonenum].nbits, start);
		if (set_bit >= bm_zone[zonenum].nbits)
			break;
		page = pfn_to_online_page((set_bit << PAGE_HINTING_MIN_ORDER) +
				bm_zone[zonenum].base_pfn);
		if (!page)
			continue;
		zone = page_zone(page);
		spin_lock(&zone->lock);

		if (PageBuddy(page) && page_private(page) >=
		    PAGE_HINTING_MIN_ORDER) {
			order = page_private(page);
			ret = __isolate_free_page(page, order);
		}
		clear_bit(set_bit, bm_zone[zonenum].bitmap);
		spin_unlock(&zone->lock);
		if (ret) {
			/*
			 * restoring page order to use it while releasing
			 * the pages back to the buddy.
			 */
			set_page_private(page, order);
			list_add_tail(&page->lru, &isolated_pages);
			isolated_cnt++;
			if (isolated_cnt == hcb->max_pages) {
				hcb->hint_pages(&isolated_pages);
				release_buddy_pages(&isolated_pages);
				isolated_cnt = 0;
			}
		}
		start = set_bit + 1;
		scanned_pages++;
	}
	if (isolated_cnt) {
		hcb->hint_pages(&isolated_pages);
		release_buddy_pages(&isolated_pages);
	}
	hcb->cleanup();
	if (scanned_pages > free_pages)
		atomic_sub((scanned_pages - free_pages),
			   &bm_zone[zonenum].free_pages);
}

static bool check_hinting_threshold(void)
{
	int zonenum = 0;

	for (; zonenum < MAX_NR_ZONES; zonenum++) {
		if (atomic_read(&bm_zone[zonenum].free_pages) >=
				hcb->max_pages)
			return true;
	}
	return false;
}

static void init_hinting_wq(struct work_struct *work)
{
	int zonenum = 0, free_pages = 0;

	for (; zonenum < MAX_NR_ZONES; zonenum++) {
		free_pages = atomic_read(&bm_zone[zonenum].free_pages);
		if (free_pages >= hcb->max_pages) {
			/* Find a better way to synchronize per zone
			 * free_pages.
			 */
			atomic_sub(free_pages,
				   &bm_zone[zonenum].free_pages);
			scan_hinting_bitmap(zonenum, free_pages);
		}
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
