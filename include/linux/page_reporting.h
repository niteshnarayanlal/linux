/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_REPORTING_H
#define _LINUX_PAGE_REPORTING_H

#include <linux/mmzone.h>
#include <linux/jump_label.h>
#include <linux/pageblock-flags.h>

#define PAGE_REPORTING_MIN_ORDER	pageblock_order
#define PAGE_REPORTING_HWM		32

#ifdef CONFIG_PAGE_REPORTING
struct page_reporting_dev_info {
	/* function that alters pages to make them "reported" */
	void (*report)(struct page_reporting_dev_info *phdev,
		       unsigned int nents);

	/* scatterlist containing pages to be processed */
	struct scatterlist *sg;

	/*
	 * Upper limit on the number of pages that the react function
	 * expects to be placed into the batch list to be processed.
	 */
	unsigned long capacity;

	/* work struct for processing reports */
	struct delayed_work work;

	/*
	 * The number of zones requesting reporting, plus one additional if
	 * processing thread is active.
	 */
	atomic_t refcnt;
};

/* Boundary functions */
struct list_head *__page_reporting_get_boundary(unsigned int order,
						int migratetype);
void page_reporting_del_from_boundary(struct page *page);
void page_reporting_add_to_boundary(struct page *page, int migratetype);
void page_reporting_move_to_boundary(struct page *page, struct zone *zone,
				     int migratetype);

/* Reported page accessors, defined in page_alloc.c */
struct page *get_unreported_page(struct zone *zone, unsigned int order,
				 int migratetype);
void free_reported_page(struct page *page, unsigned int order);

/* Tear-down and bring-up for page reporting devices */
void page_reporting_shutdown(struct page_reporting_dev_info *phdev);
int page_reporting_startup(struct page_reporting_dev_info *phdev);

void __page_reporting_free_stats(struct zone *zone);
void __page_reporting_request(struct zone *zone);

static inline void __del_page_from_reported_list(struct page *page,
						 struct zone *zone)
{
	/* page_private will contain the page order, so just use it directly */
	zone->reported_pages[page_private(page) - PAGE_REPORTING_MIN_ORDER]--;

	/* clear the flag so we can report on it when it returns */
	__ClearPageReported(page);
}
#endif /* CONFIG_PAGE_REPORTING */

/*
 * Method for obtaining the tail of the free list. Using this allows for
 * tail insertions of unreported pages into the region that is currently
 * being scanned so as to avoid interleaving reported and unreported pages.
 */
static inline struct list_head *
get_unreported_tail(struct zone *zone, unsigned int order, int migratetype)
{
#ifdef CONFIG_PAGE_REPORTING
	if (order >= PAGE_REPORTING_MIN_ORDER &&
	    test_bit(ZONE_PAGE_REPORTING_ACTIVE, &zone->flags))
		return __page_reporting_get_boundary(order, migratetype);
#endif
	return &zone->free_area[order].free_list[migratetype];
}

/*
 * Functions for adding/removing pages from reported end of list.
 * All of them expect the zone lock to be held to maintain
 * consistency of the reported list as a subset of the free list.
 */
static inline void add_page_to_reported_list(struct page *page,
					     struct zone *zone,
					     int order,
					     int migratetype)
{
#ifdef CONFIG_PAGE_REPORTING
	/* flag page as reported */
	__SetPageReported(page);

	/* update areated page accounting */
	zone->reported_pages[order - PAGE_REPORTING_MIN_ORDER]++;

	/* update boundary of new migratetype and record it */
	page_reporting_add_to_boundary(page, migratetype);
#endif
}

static inline void del_page_from_reported_list(struct page *page,
					       struct zone *zone)
{
#ifdef CONFIG_PAGE_REPORTING
	/* push boundary back if we removed the upper boundary */
	if (test_bit(ZONE_PAGE_REPORTING_ACTIVE, &zone->flags))
		page_reporting_del_from_boundary(page);

	__del_page_from_reported_list(page, zone);
#endif
}

static inline void move_page_to_reported_list(struct page *page,
					      struct zone *zone,
					      int migratetype)
{
#ifdef CONFIG_PAGE_REPORTING
	page_reporting_move_to_boundary(page, zone, migratetype);
#endif
}

/* Free reported_pages and reset reported page tracking count to 0 */
static inline void page_reporting_reset(struct zone *zone)
{
#ifdef CONFIG_PAGE_REPORTING
	if (zone->reported_pages)
		__page_reporting_free_stats(zone);
#endif
}

DECLARE_STATIC_KEY_FALSE(page_reporting_notify_enabled);

/**
 * page_reporting_notify_free - Free page notification to start page processing
 * @zone: Pointer to current zone of last page processed
 * @order: Order of last page added to zone
 *
 * This function is meant to act as a screener for __page_reporting_request
 * which will determine if a give zone has crossed over the high-water mark
 * that will justify us beginning page treatment. If we have crossed that
 * threshold then it will start the process of pulling some pages and
 * placing them in the batch list for treatment.
 */
static inline void page_reporting_notify_free(struct zone *zone, int order)
{
#ifdef CONFIG_PAGE_REPORTING
	unsigned long nr_reported;

	/* Called from hot path in __free_one_page() */
	if (!static_branch_unlikely(&page_reporting_notify_enabled))
		return;

	/* Limit notifications only to higher order pages */
	if (order < PAGE_REPORTING_MIN_ORDER)
		return;

	/* Do not bother with tests if we have already requested reporting */
	if (test_bit(ZONE_PAGE_REPORTING_REQUESTED, &zone->flags))
		return;

	/* If reported_pages is not populated, assume 0 */
	nr_reported = zone->reported_pages ?
		    zone->reported_pages[order - PAGE_REPORTING_MIN_ORDER] : 0;

	/* Only request it if we have enough to begin the page reporting */
	if (zone->free_area[order].nr_free < nr_reported + PAGE_REPORTING_HWM)
		return;

	/* This is slow, but should be called very rarely */
	__page_reporting_request(zone);
#endif
}
#endif /*_LINUX_PAGE_REPORTING_H */
