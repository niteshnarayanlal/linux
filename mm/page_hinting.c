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

static struct page_hinting_config __rcu *page_hinting_conf __read_mostly;
static DEFINE_MUTEX(page_hinting_mutex);

static inline unsigned long pfn_to_bit(struct page *page, struct zone *zone)
{
	unsigned long bitnr;

	bitnr = (page_to_pfn(page) - zone->base_pfn) >> PAGE_HINTING_MIN_ORDER;

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

static void bitmap_set_bit(struct page *page, struct zone *zone)
{
	unsigned long bitnr = 0;

	/* zone lock should be held when this function is called */
	lockdep_assert_held(&zone->lock);

	bitnr = pfn_to_bit(page, zone);
	/* set bit if it is not already set and is a valid bit */
	if (zone->bitmap && bitnr < zone->nbits &&
	    !test_and_set_bit(bitnr, zone->bitmap))
		atomic_inc(&zone->free_pages);
}

static int process_free_page(struct page *page,
			     struct page_hinting_config *phconf, int count)
{
	int mt, order, ret = 0;

	mt = get_pageblock_migratetype(page);
	order = page_private(page);
	ret = __isolate_free_page(page, order);

	if (ret) {
		/*
		 * Preserving order and migratetype for reuse while
		 * releasing the pages back to the buddy.
		 */
		set_pageblock_migratetype(page, mt);
		set_page_private(page, order);
		sg_set_page(&phconf->sg[count++], page,
			    PAGE_SIZE << order, 0);
	}

	return count;
}

/**
 * scan_zone_bitmap - Scans the bitmap for the requested zone for free
 * pages. All the free pages are isolated and added to a scatterlist. As soon
 * as the number of isolated pages reaches the threshold set by the backend,
 * they are reported to the hypervisor by the backend. Once the hypervisor
 * responds after processing they are returned back to the buddy for reuse.
 * @phconf: page hinting configuration object initialized by the backend.
 * @zone: zone for which page hinting is requested.
 */
static void scan_zone_bitmap(struct page_hinting_config *phconf,
			     struct zone *zone)
{
	unsigned long setbit;
	struct page *page;
	int count = 0;

	sg_init_table(phconf->sg, phconf->max_pages);

	for_each_set_bit(setbit, zone->bitmap, zone->nbits) {
		page = pfn_to_online_page((setbit << PAGE_HINTING_MIN_ORDER) +
					  zone->base_pfn);
		if (!page)
			continue;

		spin_lock(&zone->lock);

		if (PageBuddy(page) && page_private(page) >=
		    PAGE_HINTING_MIN_ORDER)
			count = process_free_page(page, phconf, count);

		spin_unlock(&zone->lock);
		/* Page has been processed, adjust the bit and the counter */
		clear_bit(setbit, zone->bitmap);
		atomic_dec(&zone->free_pages);

		if (count == phconf->max_pages) {
			/* Report isolated pages to the hypervisor */
			phconf->hint_pages(phconf, count);

			/* Return processed pages back to the buddy */
			return_isolated_page(zone, phconf);

			/* Reset for next reporting */
			sg_init_table(phconf->sg, phconf->max_pages);
			count = 0;
		}
	}
	/*
	 * If the number of isolated pages does not meet the max_pages
	 * threshold, we would still prefer to hint them as we have already
	 * isolated them.
	 */
	if (count) {
		sg_mark_end(&phconf->sg[count - 1]);
		phconf->hint_pages(phconf, count);

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
	struct zone *zone;

	for_each_populated_zone(zone) {
		if (atomic_read(&zone->free_pages) >= phconf->max_pages)
			scan_zone_bitmap(phconf, zone);
	}
	/*
	 * We have processed all the zone bitmaps, we can process new page
	 * hinting request now.
	 */
	atomic_set(&phconf->refcnt, 0);
}

/**
 * __page_hinting_enqueue - tracks the freed page in the respective zone's
 * bitmap and enqueues a new page hinting job to the workqueue.
 */
void __page_hinting_enqueue(struct page *page)
{
	struct page_hinting_config *phconf;
	struct zone *zone;

	rcu_read_lock();
	/*
	 * We should not process this page if either page hinting is not
	 * yet enabled or it has been disabled by the backend.
	 */
	phconf = rcu_dereference(page_hinting_conf);
	if (!phconf)
		return;

	zone = page_zone(page);
	bitmap_set_bit(page, zone);

	/*
	 * We should not enqueue a job if a previously enqueued hinting work is
	 * in progress or we don't have enough free pages in the zone.
	 */
	if (atomic_read(&zone->free_pages) >= phconf->max_pages &&
	    !atomic_cmpxchg(&phconf->refcnt, 0, 1))
		schedule_work(&phconf->hinting_work);

	rcu_read_unlock();
}

/**
 * page_hinting_cleanup - resets the page hinting fields and free the
 * bitmap for all the initialized zones.
 */
static void page_hinting_cleanup(void)
{
	struct zone *zone;

	for_each_populated_zone(zone) {
		/*
		 * We may have to cleanup in between if we fail to initialize
		 * all the zones bitmap properly.
		 */
		if (zone->bitmap)
			bitmap_free(zone->bitmap);
		zone->bitmap = NULL;
		zone->nbits = 0;
		atomic_set(&zone->free_pages, 0);
	}
}

static int zone_bitmap_alloc(struct zone *zone)
{
	unsigned long bitmap_size;
	unsigned long pages;

	pages = zone->end_pfn - zone->base_pfn;
	bitmap_size = (pages >> PAGE_HINTING_MIN_ORDER) + 1;

	if (!bitmap_size)
		return 0;

	zone->bitmap = bitmap_zalloc(bitmap_size, GFP_KERNEL);
	if (!zone->bitmap)
		return -ENOMEM;

	zone->nbits = bitmap_size;

	return 0;
}

/**
 * zone_page_hinting_init - For each zone initializes the page hinting fields
 * and allocates the respective bitmap.
 *
 * This function returns 0 on successful initialization, -ENOMEM otherwise.
 */
static int zone_page_hinting_init(void)
{
	struct zone *zone;
	int ret;

	for_each_populated_zone(zone) {
#ifdef CONFIG_ZONE_DEVICE
		/* we can not hint pages which are not in the buddy */
		if (zone_idx(zone) == ZONE_DEVICE)
			continue;
#endif
		spin_lock(&zone->lock);
		zone->base_pfn = zone->zone_start_pfn;
		zone->end_pfn = zone_end_pfn(zone);
		spin_unlock(&zone->lock);

		ret = zone_bitmap_alloc(zone);
		if (ret < 0) {
			page_hinting_cleanup();
			return ret;
		}
	}

	return 0;
}

void page_hinting_disable(struct page_hinting_config *phconf)
{
	mutex_lock(&page_hinting_mutex);

	if (rcu_access_pointer(page_hinting_conf) != phconf)
		return;

	/* Cancel any pending hinting request */
	cancel_work_sync(&phconf->hinting_work);

	RCU_INIT_POINTER(page_hinting_conf, NULL);
	synchronize_rcu();

	/* Free the scatterlist used for isolated pages */
	kfree(phconf->sg);
	phconf->sg = NULL;

	/* Cleanup the bitmaps and old tracking data */
	page_hinting_cleanup();
	mutex_unlock(&page_hinting_mutex);
}
EXPORT_SYMBOL_GPL(page_hinting_disable);

int page_hinting_enable(struct page_hinting_config *conf)
{
	int ret = 0;

	mutex_lock(&page_hinting_mutex);

	/* check if someone is already using  page hinting*/
	if (rcu_access_pointer(page_hinting_conf)) {
		ret = -EBUSY;
		goto out;
	}

	/* allocate scatterlist to hold isolated pages */
	conf->sg = kcalloc(conf->max_pages, sizeof(*conf->sg), GFP_KERNEL);
	if (!conf->sg) {
		ret = -ENOMEM;
		goto out;
	}

	/* initialize each zone's fields required for page hinting */
	ret = zone_page_hinting_init();
	if (ret < 0) {
		kfree(conf->sg);
		goto out;
	}

	atomic_set(&conf->refcnt, 0);
	INIT_WORK(&conf->hinting_work, page_hinting_wq);

	/* assign the configuration object provided by the backend */
	rcu_assign_pointer(page_hinting_conf, conf);

out:
	mutex_unlock(&page_hinting_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(page_hinting_enable);
