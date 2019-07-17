// SPDX-License-Identifier: GPL-2.0
/*
 * Page hinting core infrastructure to enable a VM to report free pages to its
 * hypervisor.
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
 * struct zone_free_area: For a single zone across NUMA nodes, it holds the
 * bitmap pointer to track the free pages and other required parameters
 * used to recover these pages by scanning the bitmap.
 * @bitmap:		Pointer to the bitmap in PAGE_HINTING_MIN_ORDER
 *			granularity.
 * @base_pfn:		Starting PFN value for the zone whose bitmap is stored.
 * @end_pfn:		Indicates the last PFN value for the zone.
 * @free_pages:		Tracks the number of free pages of granularity
 *			PAGE_HINTING_MIN_ORDER.
 * @nbits:		Indicates the total size of the bitmap in bits allocated
 *			at the time of initialization.
 */
struct zone_free_area {
	unsigned long *bitmap;
	unsigned long base_pfn;
	unsigned long end_pfn;
	atomic_t free_pages;
	unsigned long nbits;
} free_area[MAX_NR_ZONES];

static void init_hinting_wq(struct work_struct *work);
static DEFINE_MUTEX(page_hinting_init);
const struct page_hinting_config *page_hitning_conf;
struct work_struct hinting_work;
atomic_t page_hinting_active;

void free_area_cleanup(int nr_zones)
{
	int zone_idx;

	for (zone_idx = 0; zone_idx < nr_zones; zone_idx++) {
		bitmap_free(free_area[zone_idx].bitmap);
		free_area[zone_idx].base_pfn = 0;
		free_area[zone_idx].end_pfn = 0;
		free_area[zone_idx].nbits = 0;
		atomic_set(&free_area[zone_idx].free_pages, 0);
	}
}

int page_hinting_enable(const struct page_hinting_config *conf)
{
	unsigned long bitmap_size = 0;
	int zone_idx = 0, ret = -EBUSY;
	struct zone *zone;

	mutex_lock(&page_hinting_init);
	if (!page_hitning_conf) {
		for_each_populated_zone(zone) {
			zone_idx = zone_idx(zone);
#ifdef CONFIG_ZONE_DEVICE
			if (zone_idx == ZONE_DEVICE)
				continue;
#endif
			spin_lock(&zone->lock);
			if (free_area[zone_idx].base_pfn) {
				free_area[zone_idx].base_pfn =
					min(free_area[zone_idx].base_pfn,
					    zone->zone_start_pfn);
				free_area[zone_idx].end_pfn =
					max(free_area[zone_idx].end_pfn,
					    zone->zone_start_pfn +
					    zone->spanned_pages);
			} else {
				free_area[zone_idx].base_pfn =
					zone->zone_start_pfn;
				free_area[zone_idx].end_pfn =
					zone->zone_start_pfn +
					zone->spanned_pages;
			}
			spin_unlock(&zone->lock);
		}

		for (zone_idx = 0; zone_idx < MAX_NR_ZONES; zone_idx++) {
			unsigned long pages = free_area[zone_idx].end_pfn -
					free_area[zone_idx].base_pfn;
			bitmap_size = (pages >> PAGE_HINTING_MIN_ORDER) + 1;
			if (!bitmap_size)
				continue;
			free_area[zone_idx].bitmap = bitmap_zalloc(bitmap_size,
								   GFP_KERNEL);
			if (!free_area[zone_idx].bitmap) {
				free_area_cleanup(zone_idx);
				mutex_unlock(&page_hinting_init);
				return -ENOMEM;
			}
			free_area[zone_idx].nbits = bitmap_size;
		}
		page_hitning_conf = conf;
		INIT_WORK(&hinting_work, init_hinting_wq);
		ret = 0;
	}
	mutex_unlock(&page_hinting_init);
	return ret;
}
EXPORT_SYMBOL_GPL(page_hinting_enable);

void page_hinting_disable(void)
{
	cancel_work_sync(&hinting_work);
	page_hitning_conf = NULL;
	free_area_cleanup(MAX_NR_ZONES);
}
EXPORT_SYMBOL_GPL(page_hinting_disable);

static unsigned long pfn_to_bit(struct page *page, int zone_idx)
{
	unsigned long bitnr;

	bitnr = (page_to_pfn(page) - free_area[zone_idx].base_pfn)
			 >> PAGE_HINTING_MIN_ORDER;
	return bitnr;
}

static void release_buddy_pages(struct list_head *pages)
{
	int mt = 0, zone_idx, order;
	struct page *page, *next;
	unsigned long bitnr;
	struct zone *zone;

	list_for_each_entry_safe(page, next, pages, lru) {
		zone_idx = page_zonenum(page);
		zone = page_zone(page);
		bitnr = pfn_to_bit(page, zone_idx);
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
	struct zone *zone = page_zone(page);
	int zone_idx = page_zonenum(page);
	unsigned long bitnr = 0;

	lockdep_assert_held(&zone->lock);
	bitnr = pfn_to_bit(page, zone_idx);
	/*
	 * TODO: fix possible underflows.
	 */
	if (free_area[zone_idx].bitmap &&
	    bitnr < free_area[zone_idx].nbits &&
	    !test_and_set_bit(bitnr, free_area[zone_idx].bitmap))
		atomic_inc(&free_area[zone_idx].free_pages);
}

static void scan_zone_free_area(int zone_idx, int free_pages)
{
	int ret = 0, order, isolated_cnt = 0;
	unsigned long set_bit, start = 0;
	LIST_HEAD(isolated_pages);
	struct page *page;
	struct zone *zone;

	for (;;) {
		ret = 0;
		set_bit = find_next_bit(free_area[zone_idx].bitmap,
					free_area[zone_idx].nbits, start);
		if (set_bit >= free_area[zone_idx].nbits)
			break;
		page = pfn_to_online_page((set_bit << PAGE_HINTING_MIN_ORDER) +
				free_area[zone_idx].base_pfn);
		if (!page)
			continue;
		zone = page_zone(page);
		spin_lock(&zone->lock);

		if (PageBuddy(page) && page_private(page) >=
		    PAGE_HINTING_MIN_ORDER) {
			order = page_private(page);
			ret = __isolate_free_page(page, order);
		}
		clear_bit(set_bit, free_area[zone_idx].bitmap);
		atomic_dec(&free_area[zone_idx].free_pages);
		spin_unlock(&zone->lock);
		if (ret) {
			/*
			 * restoring page order to use it while releasing
			 * the pages back to the buddy.
			 */
			set_page_private(page, order);
			list_add_tail(&page->lru, &isolated_pages);
			isolated_cnt++;
			if (isolated_cnt == page_hitning_conf->max_pages) {
				page_hitning_conf->hint_pages(&isolated_pages);
				release_buddy_pages(&isolated_pages);
				isolated_cnt = 0;
			}
		}
		start = set_bit + 1;
	}
	if (isolated_cnt) {
		page_hitning_conf->hint_pages(&isolated_pages);
		release_buddy_pages(&isolated_pages);
	}
}

static void init_hinting_wq(struct work_struct *work)
{
	int zone_idx, free_pages;

	atomic_set(&page_hinting_active, 1);
	for (zone_idx = 0; zone_idx < MAX_NR_ZONES; zone_idx++) {
		free_pages = atomic_read(&free_area[zone_idx].free_pages);
		if (free_pages >= page_hitning_conf->max_pages)
			scan_zone_free_area(zone_idx, free_pages);
	}
	atomic_set(&page_hinting_active, 0);
}

void page_hinting_enqueue(struct page *page, int order)
{
	int zone_idx;

	if (!page_hitning_conf || order < PAGE_HINTING_MIN_ORDER)
		return;

	bm_set_pfn(page);
	if (atomic_read(&page_hinting_active))
		return;
	zone_idx = zone_idx(page_zone(page));
	if (atomic_read(&free_area[zone_idx].free_pages) >=
			page_hitning_conf->max_pages) {
		int cpu = smp_processor_id();

		queue_work_on(cpu, system_wq, &hinting_work);
	}
}
