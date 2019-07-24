/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_HINTING_H
#define _LINUX_PAGE_HINTING_H

#include <linux/mmzone.h>
#include <linux/jump_label.h>
#include <linux/pageblock-flags.h>
#include <asm/pgtable_types.h>

#define PAGE_HINTING_MIN_ORDER	pageblock_order
#define PAGE_HINTING_HWM		32

#ifdef CONFIG_PAGE_HINTING
struct page_hinting_dev_info {
	/* function that alters pages to make them "hinted" */
	void (*react)(struct page_hinting_dev_info *phdev,
		      unsigned int num_hints);

	/* scatterlist containing pages to be processed */
	struct scatterlist *sg;

	/*
	 * Upper limit on the number of pages that the react function
	 * expects to be placed into the batch list to be processed.
	 */
	unsigned long capacity;

	/* work struct for processing hints */
	struct delayed_work work;

	/*
	 * The number of zones requesting hinting, plus one additional if
	 * processing thread is active.
	 */
	atomic_t refcnt;
};

extern struct static_key page_hinting_notify_enabled;

/* Boundary functions */
struct list_head *__page_hinting_get_boundary(unsigned int order,
					      int migratetype);
void page_hinting_del_from_boundary(struct page *page, struct zone *zone);
void page_hinting_add_to_boundary(struct page *page, struct zone *zone,
			     int migratetype);

/* Hinted page accessors, defined in page_alloc.c */
struct page *get_unhinted_page(struct zone *zone, unsigned int order,
			       int migratetype);
void put_hinted_page(struct zone *zone, struct page *page);

void __page_hinting_request(struct zone *zone);
void __page_hinting_free_stats(struct zone *zone);

/* Tear-down and bring-up for page hinting devices */
void page_hinting_shutdown(struct page_hinting_dev_info *phdev);
int page_hinting_startup(struct page_hinting_dev_info *phdev);
#endif /* CONFIG_PAGE_HINTING */

static inline struct list_head *get_unhinted_tail(struct zone *zone,
						  unsigned int order,
						  int migratetype)
{
#ifdef CONFIG_PAGE_HINTING
	if (order >= PAGE_HINTING_MIN_ORDER &&
	    test_bit(ZONE_PAGE_HINTING_ACTIVE, &zone->flags))
		return __page_hinting_get_boundary(order, migratetype);
#endif
	return &zone->free_area[order].free_list[migratetype];
}

static inline void clear_page_hinted(struct page *page,
				     struct zone *zone)
{
#ifdef CONFIG_PAGE_HINTING
	if (likely(!PageHinted(page)))
		return;

	/* push boundary back if we removed the upper boundary */
	if (test_bit(ZONE_PAGE_HINTING_ACTIVE, &zone->flags))
		page_hinting_del_from_boundary(page, zone);

	__ClearPageHinted(page);

	/* page_private will contain the page order, so just use it directly */
	zone->hinted_pages[page_private(page) - PAGE_HINTING_MIN_ORDER]--;
#endif
}

/* Free hinted_pages and reset hinted page tracking count to 0 */
static inline void page_hinting_reset(struct zone *zone)
{
#ifdef CONFIG_PAGE_HINTING
	if (zone->hinted_pages)
		__page_hinting_free_stats(zone);
#endif
}

/**
 * page_hinting_notify_free - Free page notification to start page processing
 * @zone: Pointer to current zone of last page processed
 * @order: Order of last page added to zone
 *
 * This function is meant to act as a screener for __page_hinting_request which
 * will determine if a give zone has crossed over the high-water mark that
 * will justify us beginning page treatment. If we have crossed that
 * threshold then it will start the process of pulling some pages and
 * placing them in the batch list for treatment.
 */
static inline void page_hinting_notify_free(struct zone *zone, int order)
{
#ifdef CONFIG_PAGE_HINTING
	unsigned long nr_hinted;

	/* Called from hot path in __free_one_page() */
	if (!static_key_false(&page_hinting_notify_enabled))
		return;

	/* Limit notifications only to higher order pages */
	if (order < PAGE_HINTING_MIN_ORDER)
		return;

	/* Do not bother with tests if we have already requested hinting */
	if (test_bit(ZONE_PAGE_HINTING_REQUESTED, &zone->flags))
		return;

	/* If hinted_pages is not populated, assume 0 */
	nr_hinted = zone->hinted_pages ?
		    zone->hinted_pages[order - PAGE_HINTING_MIN_ORDER] : 0;

	/* Only request it if we have enough to begin the page hinting */
	if (zone->free_area[order].nr_free < nr_hinted + PAGE_HINTING_HWM)
		return;

	/* This is slow, but should be called very rarely */
	__page_hinting_request(zone);
#endif
}
#endif /*_LINUX_PAGE_HINTING_H */
