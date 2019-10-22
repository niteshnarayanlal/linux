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
#include <linux/page-isolation.h>
#include "internal.h"

static struct page_reporting_config __rcu *page_reporting_conf __read_mostly;
static DEFINE_MUTEX(page_reporting_mutex);

#include <linux/kobject.h>
unsigned long freed, captured, scanned, isolated, returned, reported, failed, already_isolated, marked_isolated, trying_to_isolate, isol_failed;
int sys_init_cnt;

#ifdef CONFIG_SYSFS
#define HINTING_ATTR_RO(_name) \
		static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

static ssize_t hinting_memory_stats_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sprintf(buf, "Freed memory:%lu KB\nCaptured memory:%lu KB\n"
		       "Scanned memory:%lu KB\nTrying to isolate memory:%lu KB\nIsolated memory:%lu KB\n"
		      "Failed memory:%lu KB\nIsolation Failed:%lu KB\nReturned memory:%lu KB\nAlready isolated memory:%lu \nMarked isolated memory:%lu KB\n",
		       freed, captured, scanned, trying_to_isolate, isolated,
		       failed, isol_failed, returned, already_isolated, marked_isolated);
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

static void return_isolated_page(struct zone *zone,
				 struct page_reporting_config *phconf)
{
	struct scatterlist *sg = phconf->sg;

	do {
		returned += 1 << page_private(sg_page(sg));
		__return_isolated_page(zone, sg_page(sg));
	} while (!sg_is_last(sg++));
}

static void bitmap_set_bit(struct page *page,
			   struct zone_reporting_bitmap *rbitmap)
{
	unsigned long pfn, bitnr = 0;

	if (unlikely(!rbitmap->bitmap))
		return;

	pfn = page_to_pfn(page);
	bitnr = (pfn - rbitmap->base_pfn) >> PAGE_REPORTING_MIN_ORDER;

	/* set bit if it is not already set and is a valid bit */
	if (bitnr < rbitmap->nbits &&
	    !test_and_set_bit(bitnr, rbitmap->bitmap)) {
		captured += (1 << page_private(page));
		atomic_inc(&rbitmap->free_pages);
	}
}

static int process_free_page(struct page *page, struct zone *zone,
			     struct page_reporting_config *phconf, int count)
{
	unsigned long mt, start, end;
	int order, ret = 0;

	order = page_private(page);
	mt = get_pageblock_migratetype(page);
	start = page_to_pfn(page);

	trying_to_isolate += 1 << page_private(page);
	printk("\nPage reporting requested for order:%d original mt:%ld page->index:%lu\n", order, mt, page->index);
	end = start + (1 << order);
	ret = start_isolate_page_range(start, end, mt, 0);
	if (ret < 0) {
		printk("\n***************Failed*****************\n");
		isol_failed += (1 << order);
	} else {
		marked_isolated += 1 << page_private(page);
		if (test_pages_isolated(start, end, 0) != -EBUSY) {
			//printk("\nMarking range to be reported start:%lu end:%lu return:%d", start, end, ret);
			isolated += (1 << order);
			sg_set_page(&phconf->sg[count++], pfn_to_page(start),
				    PAGE_SIZE << order, 0);
		} else {
			//printk("\nRange can not be reported start:%lu end:%lu return:%d\n", start, end, ret);
			failed += (1 << order);
			undo_isolate_page_range(start, end, mt);
		}
	}
	return count;
}

/**
 * scan_zone_bitmap - scans the bitmap for the requested zone.
 * @phconf: page reporting configuration object initialized by the backend.
 * @rbitmap: reporting bitmap object carrying zone's bitmap and required fields.
 * @zone: zone for which page reporting is requested.
 *
 * For every page marked in the bitmap it checks if it is still free if so it
 * isolates and adds them to a scatterlist. As soon as the number of isolated
 * pages reach the threshold set by the backend, they are reported to the
 * hypervisor by the backend. Once the hypervisor responds after processing
 * they are returned back to the buddy for reuse.
 */
static void scan_zone_bitmap(struct page_reporting_config *phconf,
			     struct zone_reporting_bitmap *rbitmap,
			     struct zone *zone)
{
	unsigned long setbit;
	struct page *page;
	int count = 0;

	sg_init_table(phconf->sg, phconf->max_pages);

	for_each_set_bit(setbit, rbitmap->bitmap, rbitmap->nbits) {
		/* Process only if the page is still online */
		page = pfn_to_online_page((setbit << PAGE_REPORTING_MIN_ORDER) +
					  rbitmap->base_pfn);
		/* We should process a page only if it is still online */
		if (!page) {
			clear_bit(setbit, rbitmap->bitmap);
			atomic_dec(&rbitmap->free_pages);
			continue;
		}

//		spin_lock_irqsave(&zone->lock, flags);

		scanned += 1 << page_private(page);
		/* Ensure page is still free and not of type MIGRATE_ISOLATE */
		if (PageBuddy(page) &&
				//!is_migrate_isolate_page(page) &&
		    page_private(page) >= PAGE_REPORTING_MIN_ORDER)
			count = process_free_page(page, zone, phconf, count);
		else
			already_isolated += 1 << page_private(page);
//		spin_unlock_irqrestore(&zone->lock, flags);
		/* Page has been processed, adjust its bit and zone counter */
		clear_bit(setbit, rbitmap->bitmap);
		atomic_dec(&rbitmap->free_pages);

		printk("\nChecking for possible reporting count:%d max_pages:%d\n", count, phconf->max_pages);
		if (count >= phconf->max_pages) {
			printk("\nReporting pages...\n");
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
	struct zone_reporting_bitmap *rbitmap;
	int free_pages;

	for_each_populated_zone(zone) {
		if (sys_init_cnt == 0) {
			int err = sysfs_create_group(mm_kobj,
					&hinting_attr_group);
			if (err)
				pr_err("hinting: register sysfs failed\n");
			sys_init_cnt = 1;
		}
		/*
		 * A newly enqueued/ongoing job will be canceled before
		 * the rcu protected zone_reporting_bitmap pointer
		 * object is freed. Hence, it should be safe to access
		 * it here without a read lock.
		 */
		rbitmap = rcu_dereference(zone->reporting_bitmap);
		free_pages = atomic_read(&rbitmap->free_pages);

		if (rbitmap && free_pages >= phconf->max_pages)
			scan_zone_bitmap(phconf, rbitmap, zone);
	}
	__clear_bit(PAGE_REPORTING_ACTIVE, &phconf->flags);
}

/**
 * __page_reporting_enqueue - tracks the freed page in the respective zone's
 * bitmap and enqueues a new page reporting job to the workqueue if possible.
 */
void __page_reporting_enqueue(struct page *page)
{
	struct page_reporting_config *phconf;
	struct zone_reporting_bitmap *rbitmap;
	struct zone *zone;

	freed += (1 << page_private(page));
	rcu_read_lock();
	/*
	 * We should not process this page if either page reporting is not
	 * yet completely enabled or it has been disabled by the backend.
	 */
	phconf = rcu_dereference(page_reporting_conf);
	if (!phconf)
		goto out;

	zone = page_zone(page);
	rbitmap = rcu_dereference(zone->reporting_bitmap);
	if (!rbitmap)
		goto out;

	bitmap_set_bit(page, rbitmap);

	/*
	 * We should not enqueue a job if we don't have enough free pages in
	 * the zone or if this zone has an ongoing page reporting request.
	 */
	if (atomic_read(&rbitmap->free_pages) < phconf->max_pages ||
	    test_bit(PAGE_REPORTING_ACTIVE, &phconf->flags))
		goto out;

	__set_bit(PAGE_REPORTING_ACTIVE, &phconf->flags);
	schedule_work(&phconf->reporting_work);

out:
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
		struct zone_reporting_bitmap *rbitmap;

		rbitmap = rcu_dereference(zone->reporting_bitmap);
		/*
		 * Bitmap fields may not necessarily be allocated for all the
		 * populated zones. Specifically when an allocation fails before
		 * completely initializing the last zone.
		 */
		if (!rbitmap)
			continue;
		RCU_INIT_POINTER(zone->reporting_bitmap, NULL);
		synchronize_rcu();

		if (rbitmap->bitmap) {
			bitmap_free(rbitmap->bitmap);
			rbitmap->bitmap = NULL;
		}
		atomic_set(&rbitmap->free_pages, 0);
		kfree(rbitmap);
	}
}

static int zone_bitmap_alloc(struct zone_reporting_bitmap *rbitmap)
{
	unsigned long bitmap_size, pages;

	pages = rbitmap->end_pfn - rbitmap->base_pfn;
	bitmap_size = (pages >> PAGE_REPORTING_MIN_ORDER) + 1;

	if (!bitmap_size)
		return 0;

	rbitmap->bitmap = bitmap_zalloc(bitmap_size, GFP_KERNEL);
	if (!rbitmap->bitmap)
		return -ENOMEM;

	rbitmap->nbits = bitmap_size;
	return 0;
}

/**
 * zone_reporting_init - for each zone initializes the page reporting fields
 * and allocates the respective bitmap.
 *
 * This function returns 0 on successful initialization, -ENOMEM otherwise.
 */
static int zone_reporting_init(void)
{
	unsigned long flags;
	struct zone *zone;
	int ret = 0;

	for_each_populated_zone(zone) {
		/*
		 * Zones eg. ZONE_DEVICE with pages not in the buddy are never
		 * considered populated.
		 */
		struct zone_reporting_bitmap *rbitmap;

		rbitmap = kmalloc(sizeof(*rbitmap), GFP_KERNEL);
		if (!rbitmap) {
			zone_reporting_cleanup();
			return -ENOMEM;
		}

		/*
		 * Preserve start and end PFN values in case they change due
		 * to memory hotplug.
		 */
		spin_lock_irqsave(&zone->lock, flags);
		rbitmap->base_pfn = zone->zone_start_pfn;
		rbitmap->end_pfn = zone_end_pfn(zone);
		spin_unlock_irqrestore(&zone->lock, flags);

		atomic_set(&rbitmap->free_pages, 0);

		ret = zone_bitmap_alloc(rbitmap);
		if (ret < 0) {
			zone_reporting_cleanup();
			return ret;
		}
		rcu_assign_pointer(zone->reporting_bitmap, rbitmap);
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

	/* initialize each zone's bitmap required for page reporting */
	ret = zone_reporting_init();
	if (ret < 0) {
		kfree(phconf->sg);
		goto out;
	}

	INIT_WORK(&phconf->reporting_work, page_reporting_wq);

	/* assign the configuration object provided by the backend */
	rcu_assign_pointer(page_reporting_conf, phconf);

out:
	mutex_unlock(&page_reporting_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(page_reporting_enable);
