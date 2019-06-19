/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MEMORY_AERATION_H
#define _LINUX_MEMORY_AERATION_H

#include <linux/mmzone.h>
#include <linux/pageblock-flags.h>

struct page *get_aeration_page(struct zone *zone, unsigned int order,
			       int migratetype);
void put_aeration_page(struct zone *zone, struct page *page);

static inline struct list_head *aerator_get_tail(struct zone *zone,
						 unsigned int order,
						 int migratetype)
{
	return &zone->free_area[order].free_list[migratetype];
}

static inline void set_page_aerated(struct page *page,
				    struct zone *zone,
				    unsigned int order,
				    int migratetype)
{
#ifdef CONFIG_AERATION
	/* update areated page accounting */
	zone->free_area[order].nr_free_aerated++;

	/* record migratetype and flag page as aerated */
	set_pcppage_migratetype(page, migratetype);
	__SetPageAerated(page);
#endif
}

static inline void clear_page_aerated(struct page *page,
				      struct zone *zone,
				      struct free_area *area)
{
#ifdef CONFIG_AERATION
	if (likely(!PageAerated(page)))
		return;

	__ClearPageAerated(page);
	area->nr_free_aerated--;
#endif
}

/**
 * aerator_notify_free - Free page notification that will start page processing
 * @zone: Pointer to current zone of last page processed
 * @order: Order of last page added to zone
 *
 * This function is meant to act as a screener for __aerator_notify which
 * will determine if a give zone has crossed over the high-water mark that
 * will justify us beginning page treatment. If we have crossed that
 * threshold then it will start the process of pulling some pages and
 * placing them in the batch list for treatment.
 */
static inline void aerator_notify_free(struct zone *zone, int order)
{
}
#endif /*_LINUX_MEMORY_AERATION_H */
