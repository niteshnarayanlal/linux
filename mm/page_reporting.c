// SPDX-License-Identifier: GPL-2.0
/*
 * Page reporting core infrastructure to enable a VM to report free pages to its
 * hypervisor.
 *
 * Copyright Red Hat, Inc. 2019
 *
 * Author(s): Nitesh Narayan Lal <nitesh@redhat.com>
 */

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/page_reporting.h>
#include <linux/scatterlist.h>
#include "internal.h"

#include <linux/sysfs.h>
#include <linux/kobject.h>

unsigned long scanning_requests, reporting_requests, extra_reporting_requests, reallocation_requests;
int sys_init_cnt;
static struct page_reporting_config __rcu *page_reporting_conf __read_mostly;
static DEFINE_MUTEX(page_reporting_mutex);

static inline unsigned long pfn_to_bit(struct page *page, struct zone *zone)
{
	unsigned long bitnr;

	bitnr = (page_to_pfn(page) - zone->base_pfn) >>
		PAGE_REPORTING_MIN_ORDER;

	return bitnr;
}

static void return_isolated_page(struct zone *zone,
				 struct page_reporting_config *phconf)
{
	struct scatterlist *sg = phconf->sg;

	spin_lock(&zone->lock);
	do {
		__return_isolated_page(zone, sg_page(sg));
	} while (!sg_is_last(sg++));
	spin_unlock(&zone->lock);
}

static void bitmap_clear_bit(struct page *page,
			   struct zone *zone)
{
	unsigned long pfn, bitnr = 0;

	pfn = page_to_pfn(page);
	bitnr = (pfn - zone->base_pfn) >> PAGE_REPORTING_MIN_ORDER;

	/* set bit if it is not already set and is a valid bit */
	if (zone->bitmap && bitnr < zone->nbits &&
	    test_and_clear_bit(bitnr, zone->bitmap)) {
		reallocation_requests += 1;
		atomic_dec(&zone->free_pages);
	}
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
			     struct page_reporting_config *phconf, int count)
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



#ifdef CONFIG_SYSFS
#define HINTING_ATTR_RO(_name) \
		static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

static ssize_t hinting_memory_stats_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sprintf(buf, "Scaning requests:%lu\nReporting requests:%lu\nExtra Reporting requests:%lu\nReallocation requests:%lu\n",
		       scanning_requests, reporting_requests, extra_reporting_requests, reallocation_requests);
}

HINTING_ATTR_RO(hinting_memory_stats);

static struct attribute *hinting_attrs[] = {
	&hinting_memory_stats_attr.attr,
	NULL,
};

static const struct attribute_group hinting_attr_group = {
	.attrs = hinting_attrs,
	.name = "hinting",
};
#endif

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

/**
 * scan_zone_bitmap - scans the bitmap for the requested zone.
 * @phconf: page reporting configuration object initialized by the backend.
 * @zone: zone for which page reporting is requested.
 *
 * For every page marked in the bitmap it checks if it is still free if so it
 * isolates and adds them to a scatterlist. As soon as the number of isolated
 * pages reach the threshold set by the backend, they are reported to the
 * hypervisor by the backend. Once the hypervisor responds after processing
 * they are returned back to the buddy for reuse.
 */
static void scan_zone_bitmap(struct page_reporting_config *phconf,
			     struct zone *zone)
{
	unsigned long setbit;
	struct page *page;
	int count = 0;

	sg_init_table(phconf->sg, phconf->max_pages);
	scanning_requests += 1;

	for_each_set_bit(setbit, zone->bitmap, zone->nbits) {
		/* Process only if the page is still online */
		page = pfn_to_online_page((setbit << PAGE_REPORTING_MIN_ORDER) +
					  zone->base_pfn);
		if (!page || !PageBuddy(page)) {
			/* Page has been processed, adjust its bit and zone counter */
			clear_bit(setbit, zone->bitmap);
			atomic_dec(&zone->free_pages);
			continue;
		}

		spin_lock(&zone->lock);

		/* Ensure page is still free and can be processed */
		if (PageBuddy(page) && page_private(page) >=
		    PAGE_REPORTING_MIN_ORDER)
			count = process_free_page(page, phconf, count);

		spin_unlock(&zone->lock);
		/* Page has been processed, adjust its bit and zone counter */
		clear_bit(setbit, zone->bitmap);
		atomic_dec(&zone->free_pages);

		if (count == phconf->max_pages) {
			reporting_requests += 1;
			/* Report isolated pages to the hypervisor */
			phconf->report(phconf, count);

			/* Return processed pages back to the buddy */
			return_isolated_page(zone, phconf);

			/* Reset for next reporting */
			sg_init_table(phconf->sg, phconf->max_pages);
			count = 0;
		}
	}
	/*
	 * If the number of isolated pages does not meet the max_pages
	 * threshold, we would still prefer to report them as we have already
	 * isolated them.
	 */
	if (count) {
		extra_reporting_requests += 1;
		sg_mark_end(&phconf->sg[count - 1]);
		phconf->report(phconf, count);

		return_isolated_page(zone, phconf);
	}
}

/**
 * page_reporting_wq - checks the number of free_pages in all the zones and
 * invokes a request to scan the respective bitmap if free_pages reaches or
 * exceeds the threshold specified by the backend.
 */
static void page_reporting_wq(struct work_struct *work)
{
	struct page_reporting_config *phconf =
		container_of(work, struct page_reporting_config,
			     reporting_work);
	struct zone *zone;

	if (sys_init_cnt == 0) {
		int err = sysfs_create_group(mm_kobj, &hinting_attr_group);

		if (err)
			pr_err("hinting: register sysfs failed\n");
		sys_init_cnt = 1;
	}
	for_each_populated_zone(zone) {
		if (atomic_read(&zone->free_pages) >= phconf->max_pages)
			scan_zone_bitmap(phconf, zone);
	}
	/*
	 * We have processed all the zones, we can process new page reporting
	 * request now.
	 */
	atomic_set(&phconf->refcnt, 0);
}

void __page_reporting_dequeue(struct page *page)
{
	struct page_reporting_config *phconf;
	struct zone *zone;

	rcu_read_lock();
	/*
	 * We should not process this page if either page reporting is not
	 * yet completely enabled or it has been disabled by the backend.
	 */
	phconf = rcu_dereference(page_reporting_conf);
	if (!phconf)
		goto out;

	zone = page_zone(page);
	bitmap_clear_bit(page, zone);
out:
	rcu_read_unlock();
}

/**
 * __page_reporting_enqueue - tracks the freed page in the respective zone's
 * bitmap and enqueues a new page reporting job to the workqueue if possible.
 */
void __page_reporting_enqueue(struct page *page)
{
	struct page_reporting_config *phconf;
	struct zone *zone;

	rcu_read_lock();
	/*
	 * We should not process this page if either page reporting is not
	 * yet completely enabled or it has been disabled by the backend.
	 */
	phconf = rcu_dereference(page_reporting_conf);
	if (!phconf)
		return;

	zone = page_zone(page);
	bitmap_set_bit(page, zone);

	/*
	 * We should not enqueue a job if a previously enqueued reporting work
	 * is in progress or we don't have enough free pages in the zone.
	 */
	if (atomic_read(&zone->free_pages) >= phconf->max_pages &&
	    !atomic_cmpxchg(&phconf->refcnt, 0, 1))
		schedule_work(&phconf->reporting_work);

	rcu_read_unlock();
}

/**
 * zone_reporting_cleanup - resets the page reporting fields and free the
 * bitmap for all the initialized zones.
 */
static void zone_reporting_cleanup(void)
{
	struct zone *zone;

	for_each_populated_zone(zone) {
		/*
		 * Bitmap may not be allocated for all the zones if the
		 * initialization fails before reaching to the last one.
		 */
		if (!zone->bitmap)
			continue;
		bitmap_free(zone->bitmap);
		zone->bitmap = NULL;
		atomic_set(&zone->free_pages, 0);
	}
}

static int zone_bitmap_alloc(struct zone *zone)
{
	unsigned long bitmap_size, pages;

	pages = zone->end_pfn - zone->base_pfn;
	bitmap_size = (pages >> PAGE_REPORTING_MIN_ORDER) + 1;

	if (!bitmap_size)
		return 0;

	zone->bitmap = bitmap_zalloc(bitmap_size, GFP_KERNEL);
	if (!zone->bitmap)
		return -ENOMEM;

	zone->nbits = bitmap_size;

	return 0;
}

/**
 * zone_reporting_init - For each zone initializes the page reporting fields
 * and allocates the respective bitmap.
 *
 * This function returns 0 on successful initialization, -ENOMEM otherwise.
 */
static int zone_reporting_init(void)
{
	struct zone *zone;
	int ret;

	for_each_populated_zone(zone) {
#ifdef CONFIG_ZONE_DEVICE
		/* we can not report pages which are not in the buddy */
		if (zone_idx(zone) == ZONE_DEVICE)
			continue;
#endif
		spin_lock(&zone->lock);
		zone->base_pfn = zone->zone_start_pfn;
		zone->end_pfn = zone_end_pfn(zone);
		spin_unlock(&zone->lock);

		ret = zone_bitmap_alloc(zone);
		if (ret < 0) {
			zone_reporting_cleanup();
			return ret;
		}
	}

	return 0;
}

void page_reporting_disable(struct page_reporting_config *phconf)
{
	mutex_lock(&page_reporting_mutex);

	if (rcu_access_pointer(page_reporting_conf) != phconf)
		return;

	RCU_INIT_POINTER(page_reporting_conf, NULL);
	synchronize_rcu();

	/* Cancel any pending reporting request */
	cancel_work_sync(&phconf->reporting_work);

	/* Free the scatterlist used for isolated pages */
	kfree(phconf->sg);
	phconf->sg = NULL;

	/* Cleanup the bitmaps and old tracking data */
	zone_reporting_cleanup();

	mutex_unlock(&page_reporting_mutex);
}
EXPORT_SYMBOL_GPL(page_reporting_disable);

int page_reporting_enable(struct page_reporting_config *phconf)
{
	int ret = 0;

	mutex_lock(&page_reporting_mutex);

	/* check if someone is already using page reporting*/
	if (rcu_access_pointer(page_reporting_conf)) {
		ret = -EBUSY;
		goto out;
	}

	/* allocate scatterlist to hold isolated pages */
	phconf->sg = kcalloc(phconf->max_pages, sizeof(*phconf->sg),
			     GFP_KERNEL);
	if (!phconf->sg) {
		ret = -ENOMEM;
		goto out;
	}

	/* initialize each zone's fields required for page reporting */
	ret = zone_reporting_init();
	if (ret < 0) {
		kfree(phconf->sg);
		goto out;
	}

	atomic_set(&phconf->refcnt, 0);
	INIT_WORK(&phconf->reporting_work, page_reporting_wq);

	/* assign the configuration object provided by the backend */
	rcu_assign_pointer(page_reporting_conf, phconf);

out:
	mutex_unlock(&page_reporting_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(page_reporting_enable);
