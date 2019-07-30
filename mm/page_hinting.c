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

/*
 * struct zone_free_area - For a single zone across NUMA nodes, it holds the
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

static void page_hinting_wq(struct work_struct *work);
static DEFINE_MUTEX(page_hinting_mutex);
static struct page_hinting_config *phconf;

/* zone_free_area_cleanup - free and reset the zone_free_area fields only for
 * the zones which have been initialized.
 *
 * nr_zones:	number of zones which have been initialized.
 */
void zone_free_area_cleanup(int nr_zones)
{
	int zone_idx;

	for (zone_idx = 0; zone_idx < nr_zones; zone_idx++) {
		bitmap_free(free_area[zone_idx].bitmap);
		free_area[zone_idx].bitmap = NULL;
		free_area[zone_idx].base_pfn = 0;
		free_area[zone_idx].end_pfn = 0;
		free_area[zone_idx].nbits = 0;
		atomic_set(&free_area[zone_idx].free_pages, 0);
	}
}

static int zone_free_area_init(void)
{
	struct zone *zone;
	int zone_idx;

	for_each_populated_zone(zone) {
		zone_idx = zone_idx(zone);
		/* TODO: Add comment that ZONE_DEVICE pages are not found in
		 * the buddy. */
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
		unsigned long bitmap_size;
		unsigned long pages;
		
		pages = free_area[zone_idx].end_pfn -
				free_area[zone_idx].base_pfn;
		bitmap_size = (pages >> PAGE_HINTING_MIN_ORDER) + 1;

		if (!bitmap_size)
			continue;
		free_area[zone_idx].bitmap = bitmap_zalloc(bitmap_size,
							   GFP_KERNEL);
		if (!free_area[zone_idx].bitmap) {
			/* 
			 * VM is probably running low in memory we should not
			 * enable page hinting at this time.
			 */
			zone_free_area_cleanup(zone_idx);
			return -ENOMEM;
		}
		free_area[zone_idx].nbits = bitmap_size;
	}

	return 0;
}

int page_hinting_enable(struct page_hinting_config *conf)
{
	int ret = 0;

	/* Check if someone is already using */
	if (phconf) 
		return -EBUSY;

	/* Prevent any race condition between multiple */
	mutex_lock(&page_hinting_mutex);

	/* Allocate scatterlist to hold isolated pages */
	conf->sg = kcalloc(conf->max_pages, sizeof(*conf->sg), GFP_KERNEL);
	if (!conf->sg) {
		mutex_unlock(&page_hinting_mutex);
		return -ENOMEM;
	}

	/* Initialize the zone_free_area fields for each zone */
	ret = zone_free_area_init();
	if (ret < 0) {
		kfree(conf->sg);
		mutex_unlock(&page_hinting_mutex);
		return ret;
	}

	phconf = conf;
	INIT_WORK(&phconf->hinting_work, page_hinting_wq);
	mutex_unlock(&page_hinting_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(page_hinting_enable);

void page_hinting_disable(void)
{
	/* Cancel any pending hinting request */
	cancel_work_sync(&phconf->hinting_work);

	kfree(phconf->sg);
	phconf->sg = NULL;

	/* Avoid tracking any new free page in the bitmap */
	phconf = NULL;
	/* Cleanup the bitmaps and old tracking data */
	zone_free_area_cleanup(MAX_NR_ZONES);
}
EXPORT_SYMBOL_GPL(page_hinting_disable);

/* TODO: Will it change in some way for sparsemem? */
static inline unsigned long pfn_to_bit(struct page *page, int zone_idx)
{
	unsigned long bitnr;

	bitnr = (page_to_pfn(page) - free_area[zone_idx].base_pfn)
			 >> PAGE_HINTING_MIN_ORDER;
	return bitnr;
}

/*
 * release_isolated_pages - Returns isolated pages back to the buddy. 
 * @count:	indicates the actual number of pages which are isolated.
 *
 * This function fetches migratetype, order and other information from the pages
 * in the list and put the page back to the zone from where it has been removed.
 */
static void release_isolated_pages(int count, struct zone *zone)
{
	struct scatterlist *sg = phconf->sg;
	struct page *page;
	int mt, order;

	lockdep_assert_held(&zone->lock);
	do {
		page = sg_page(sg);
		order = page_private(page);
		set_page_private(page, 0);
		mt = get_pageblock_migratetype(page);
		__free_one_page(page, page_to_pfn(page), zone,
				order, mt, false);
	} while (!sg_is_last(sg++));
}

static void bitmap_set_bit(struct page *page, int zone_idx)
{
	struct zone *zone = page_zone(page);
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

/*
 * scan_zone_free_area - Checks the bits set in the bitmap for the requested
 * zone. For each of the set bit, it finds the respective page and checks if
 * it is still free. If so, it is added to a local list. Once the list has
 * sufficient isolated pages it is passed on to the backend driver for
 * reporting to the host. After which the isolated pages are returned back
 * to the buddy.
 *
 * @zone_idx:	zone index for the zone whose bitmap needs to be scanned.
 */
static void scan_zone_free_area(int zone_idx)
{
	int order, ret, isolated_cnt = 0;
	unsigned long setbit, nbits;
	LIST_HEAD(isolated_pages);
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
			order = page_private(page);
			ret = __isolate_free_page(page, order);
		}
		clear_bit(setbit, free_area[zone_idx].bitmap);
		atomic_dec(&free_area[zone_idx].free_pages);

		if (ret) {
			/*
			 * Restoring page order to use it while releasing
			 * the pages back to the buddy.
			 */
			set_page_private(page, order);
			sg_set_page(&phconf->sg[isolated_cnt], page, PAGE_SIZE << order, 0);
			isolated_cnt++;
			if (isolated_cnt == phconf->max_pages) {
				spin_unlock(&zone->lock);
				
				/* Report isolated pages to the hypervisor */
				phconf->hint_pages(phconf, isolated_cnt);
		
				spin_lock(&zone->lock);
			       	/* Return processed pages back to the buddy */
				release_isolated_pages(isolated_cnt, zone);

				sg_init_table(phconf->sg, phconf->max_pages);
				isolated_cnt = 0;
			}
		}
		spin_unlock(&zone->lock);
		ret = 0;
	}
	/*
	 * If isolated pages count does not meet the max_pages threshold, we
	 * would still prefer to hint them as we have already isolated them.
	 */
	if (isolated_cnt) {
		sg_mark_end(&phconf->sg[isolated_cnt - 1]);	
		phconf->hint_pages(phconf, isolated_cnt);

		spin_lock(&zone->lock);
		release_isolated_pages(isolated_cnt, zone);
		spin_unlock(&zone->lock);
	}
}

/*
 * page_hinting_wq - checks the number of free_pages in all the zones and
 * invokes a request to scan the respective bitmap for each zone which has
 * free_pages >= the threshold specified by the backend.
 */
static void page_hinting_wq(struct work_struct *work)
{
	int zone_idx, free_pages;

	atomic_inc(&phconf->refcnt);
	for (zone_idx = 0; zone_idx < MAX_NR_ZONES; zone_idx++) {
		free_pages = atomic_read(&free_area[zone_idx].free_pages);
		if (free_pages >= phconf->max_pages)
			scan_zone_free_area(zone_idx);
	}
	/*
	 * We have processed all the zone bitmaps, we can process new page
	 * hinting request now.
	 */
	atomic_dec(&phconf->refcnt);
}

/*
 * __page_hinting_enqueue - if page hinting is properly enabled it sets the bit
 * corresponding to the page in the bitmap. After which if the number of
 * free_pages in the zone meets the threshold it enqueues a job in the wq
 * if another job is not already enqueued.
 */
void __page_hinting_enqueue(struct page *page)
{
	int zone_idx;

	/*
	 * We should not process the page as the page hinting is not
	 * yet properly setup by the backend.
	 */
	if (!phconf)
		return;

	zone_idx = zone_idx(page_zone(page));
	bitmap_set_bit(page, zone_idx);

	/*
	 * If an enqueued hinting work is in progress or we don't have enough
	 * free pages we should not enqueue another work until it is complete.
	 */
	if (!atomic_read(&phconf->refcnt) &&
	    atomic_read(&free_area[zone_idx].free_pages) >= phconf->max_pages)
		queue_work_on(smp_processor_id(), system_wq,
			      &phconf->hinting_work);
}
