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
#include <linux/scatterlist.h>
#include "internal.h"

/**
 * struct zone_free_area - for a single zone across NUMA nodes, it holds the
 * bitmap pointer to track the free pages and other required parameters
 * used to recover these pages by scanning the bitmap.
 * @bitmap: pointer to the bitmap in PAGE_HINTING_MIN_ORDER granularity.
 * @base_pfn: starting PFN value for the zone whose bitmap is stored.
 * @end_pfn: indicates the last PFN value for the zone.
 * @free_pages: tracks the number of free pages.
 * @nbits: indicates the total size of the bitmap in bits.
 */
struct zone_free_area {
	unsigned long *bitmap;
	unsigned long base_pfn;
	unsigned long end_pfn;
	atomic_t free_pages;
	unsigned long nbits;
};

static struct zone_free_area free_area[MAX_NR_ZONES];
static struct page_hinting_config __rcu *page_hinting_conf __read_mostly;

static inline unsigned long pfn_to_bit(struct page *page, int zone_idx)
{
	unsigned long bitnr;

	bitnr = (page_to_pfn(page) - free_area[zone_idx].base_pfn)
			 >> PAGE_HINTING_MIN_ORDER;
	return bitnr;
}

static void return_isolated_page(struct zone *zone,
				  struct page_hinting_config *phconf)
{
	struct scatterlist *sg = phconf->sg;

	spin_lock(&zone->lock);
	do {
		__return_isolated_page(zone, sg_page(sg));
	} while (!sg_is_last(sg++));
	spin_unlock(&zone->lock);
}

static void bitmap_set_bit(struct page *page, int zone_idx)
{
	struct zone *zone = page_zone(page);
	unsigned long bitnr = 0;

	/* zone lock should be held when this function is called */
	lockdep_assert_held(&zone->lock);

	bitnr = pfn_to_bit(page, zone_idx);
	/* TODO: fix possible underflows */
	if (free_area[zone_idx].bitmap &&
	    bitnr < free_area[zone_idx].nbits &&
	    !test_and_set_bit(bitnr, free_area[zone_idx].bitmap))
		atomic_inc(&free_area[zone_idx].free_pages);
}

/**
 * scan_zone_free_area - Scans the bitmap for the requested zone for free
 * pages. All the free pages are isolated and added to a scatterlist. As soon
 * as the number of isolated pages reaches the threshold set by the backend,
 * they are reported to the hypervisor by the backend. Once the hypervisor
 * responds after processing they are returned back to the buddy for reuse.
 * @phconf: page hinting configuration object initialized by the backend.
 * @zone_idx: index for the zone on which page hinting is requested.
 */
static void scan_zone_free_area(struct page_hinting_config *phconf,
				int zone_idx)
{
	int order, mt, isolated_cnt = 0, ret = 0;
	unsigned long setbit, nbits;
	struct page *page;
	struct zone *zone;

	sg_init_table(phconf->sg, phconf->max_pages);
	nbits = free_area[zone_idx].nbits;

	for_each_set_bit(setbit, free_area[zone_idx].bitmap, nbits) {
		page = pfn_to_online_page((setbit << PAGE_HINTING_MIN_ORDER) +
				free_area[zone_idx].base_pfn);
		if (!page)
			continue;

		zone = page_zone(page);
		spin_lock(&zone->lock);

		if (PageBuddy(page) && page_private(page) >=
		    PAGE_HINTING_MIN_ORDER) {
			mt = get_pageblock_migratetype(page);
			order = page_private(page);
			ret = __isolate_free_page(page, order);
		}
		/* page has been scanned, adjust its bit and counter */
		clear_bit(setbit, free_area[zone_idx].bitmap);
		atomic_dec(&free_area[zone_idx].free_pages);
		spin_unlock(&zone->lock);

		if (ret) {
			/*
			 * Restoring page order and migratetype for reuse while
			 * releasing the pages back to the buddy
			 */
			set_pageblock_migratetype(page, mt);
			set_page_private(page, order);
			sg_set_page(&phconf->sg[isolated_cnt], page,
				    PAGE_SIZE << order, 0);
			isolated_cnt++;
			if (isolated_cnt == phconf->max_pages) {
				/* Report isolated pages to the hypervisor */
				phconf->hint_pages(phconf, isolated_cnt);

				/* Return processed pages back to the buddy */
				return_isolated_page(zone, phconf);

				/* Reset for next reporting */
				sg_init_table(phconf->sg, phconf->max_pages);
				isolated_cnt = 0;
			}
			ret = 0;
		}
	}
	/*
	 * If the number of solated pages does not meet the max_pages
	 * threshold, we would still prefer to hint them as we have already
	 * isolated them.
	 */
	if (isolated_cnt) {
		sg_mark_end(&phconf->sg[isolated_cnt - 1]);
		phconf->hint_pages(phconf, isolated_cnt);

		return_isolated_page(zone, phconf);
	}
}

/**
 * page_hinting_wq - checks the number of free_pages in all the zones and
 * invokes a request to scan the respective bitmap if free_pages reaches or
 * exceeds the threshold specified by the backend.
 */
static void page_hinting_wq(struct work_struct *work)
{
	struct page_hinting_config *phconf =
		container_of(work, struct page_hinting_config, hinting_work);
	int zone_idx, free_pages;

	atomic_inc(&phconf->refcnt);
	for (zone_idx = 0; zone_idx < MAX_NR_ZONES; zone_idx++) {
		free_pages = atomic_read(&free_area[zone_idx].free_pages);
		if (free_pages >= phconf->max_pages)
			scan_zone_free_area(phconf, zone_idx);
	}
	/*
	 * We have processed all the zone bitmaps, we can process new page
	 * hinting request now.
	 */
	atomic_dec(&phconf->refcnt);
}

/**
 * __page_hinting_enqueue - tracks the freed page in the respective zone's
 * bitmap and enqueues a new page hinting job to the workqueue.
 */
void __page_hinting_enqueue(struct page *page)
{
	struct page_hinting_config *phconf;
	int zone_idx;

	rcu_read_lock();
	/*
	 * We should not process this page if either page hinting is not
	 * yet enabled or it has been disabled by the backend.
	 */
	phconf = rcu_dereference(page_hinting_conf);
	if (!phconf)
		return;

	zone_idx = zone_idx(page_zone(page));
	bitmap_set_bit(page, zone_idx);

	/*
	 * We should not enqueue a job if a previously enqueued hinting work is
	 * in progress or we don't have enough free pages in the zone.
	 */
	if (!atomic_read(&phconf->refcnt) &&
	    atomic_read(&free_area[zone_idx].free_pages) >= phconf->max_pages)
		schedule_work(&phconf->hinting_work);

	rcu_read_unlock();
}

/**
 * zone_free_area_cleanup - free and reset the zone_free_area fields only for
 * the zones which have been initialized.
 *
 * nr_zones: number of zones which have been initialized.
 */
static void zone_free_area_cleanup(int nr_zones)
{
	int zone_idx;

	for (zone_idx = 0; zone_idx < nr_zones; zone_idx++) {
		bitmap_free(free_area[zone_idx].bitmap);
		free_area[zone_idx].bitmap = NULL;
		free_area[zone_idx].base_pfn = -1;
		free_area[zone_idx].end_pfn = -1;
		free_area[zone_idx].nbits = 0;
		atomic_set(&free_area[zone_idx].free_pages, 0);
	}
}

static int zone_bitmap_alloc(int zone_idx)
{
	unsigned long bitmap_size;
	unsigned long pages;

	pages = free_area[zone_idx].end_pfn - free_area[zone_idx].base_pfn;
	bitmap_size = (pages >> PAGE_HINTING_MIN_ORDER) + 1;

	if (!bitmap_size)
		return 0;

	free_area[zone_idx].bitmap = bitmap_zalloc(bitmap_size, GFP_KERNEL);
	if (!free_area[zone_idx].bitmap)
		return -ENOMEM;

	free_area[zone_idx].nbits = bitmap_size;

	return 0;
}

/**
 * zone_free_area_init - Initializes struct zone_free_area fields and allocates
 * bitmap for each zone.
 *
 * This function returns 0 on successful initialization, -ENOMEM otherwise.
 */
static int zone_free_area_init(void)
{
	struct zone *zone;
	int zone_idx, ret;

	for_each_populated_zone(zone) {
		zone_idx = zone_idx(zone);
#ifdef CONFIG_ZONE_DEVICE
		/* we can not hint pages which are not in the buddy */
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
		ret = zone_bitmap_alloc(zone_idx);
		if (ret < 0) {
			/*
			 * VM is probably running low in memory we should not
			 * enable page hinting for any zone at this time.
			 */
			zone_free_area_cleanup(zone_idx);
			return ret;
		}
	}

	return 0;
}

void page_hinting_disable(struct page_hinting_config *phconf)
{
	if (rcu_access_pointer(page_hinting_conf) != phconf)
		return;

	RCU_INIT_POINTER(page_hinting_conf, NULL);
	synchronize_rcu();

	/* Cancel any pending hinting request */
	cancel_work_sync(&phconf->hinting_work);

	/* Free the scatterlist used for isolated pages */
	kfree(phconf->sg);
	phconf->sg = NULL;

	/* Cleanup the bitmaps and old tracking data */
	zone_free_area_cleanup(MAX_NR_ZONES);
}
EXPORT_SYMBOL_GPL(page_hinting_disable);

int page_hinting_enable(struct page_hinting_config *conf)
{
	int ret = 0;

	/* check if someone is already using  page hinting*/
	if (rcu_access_pointer(page_hinting_conf))
		return -EBUSY;

	/* allocate scatterlist to hold isolated pages */
	conf->sg = kcalloc(conf->max_pages, sizeof(*conf->sg), GFP_KERNEL);
	if (!conf->sg)
		return -ENOMEM;

	/* initialize the zone_free_area fields for each zone */
	ret = zone_free_area_init();
	if (ret < 0) {
		kfree(conf->sg);
		return ret;
	}

	atomic_set(&conf->refcnt, 0);
	INIT_WORK(&conf->hinting_work, page_hinting_wq);

	/* assign the configuration object provided by the backend */
	rcu_assign_pointer(page_hinting_conf, conf);

	return 0;
}
EXPORT_SYMBOL_GPL(page_hinting_enable);
