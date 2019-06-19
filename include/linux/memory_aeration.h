/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MEMORY_AERATION_H
#define _LINUX_MEMORY_AERATION_H

#include <linux/mmzone.h>
#include <linux/jump_label.h>
#include <linux/pageblock-flags.h>
#include <asm/pgtable_types.h>

#define AERATOR_MIN_ORDER	pageblock_order
#define AERATOR_HWM		32

struct aerator_dev_info {
	void (*react)(struct aerator_dev_info *a_dev_info);
	struct list_head batch;
	unsigned long capacity;
	atomic_t refcnt;
};

extern struct static_key aerator_notify_enabled;

void __aerator_notify(struct zone *zone);
struct page *get_aeration_page(struct zone *zone, unsigned int order,
			       int migratetype);
void put_aeration_page(struct zone *zone, struct page *page);

void __aerator_del_from_boundary(struct page *page, struct zone *zone);
void aerator_add_to_boundary(struct page *page, struct zone *zone);

struct list_head *__aerator_get_tail(unsigned int order, int migratetype);
static inline struct list_head *aerator_get_tail(struct zone *zone,
						 unsigned int order,
						 int migratetype)
{
#ifdef CONFIG_AERATION
	if (order >= AERATOR_MIN_ORDER &&
	    test_bit(ZONE_AERATION_ACTIVE, &zone->flags))
		return __aerator_get_tail(order, migratetype);
#endif
	return &zone->free_area[order].free_list[migratetype];
}

static inline void aerator_del_from_boundary(struct page *page,
					     struct zone *zone)
{
	if (PageAerated(page) && test_bit(ZONE_AERATION_ACTIVE, &zone->flags))
		__aerator_del_from_boundary(page, zone);
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

	/* update boundary of new migratetype and record it */
	aerator_add_to_boundary(page, zone);
#endif
}

static inline void clear_page_aerated(struct page *page,
				      struct zone *zone,
				      struct free_area *area)
{
#ifdef CONFIG_AERATION
	if (likely(!PageAerated(page)))
		return;

	/* push boundary back if we removed the upper boundary */
	aerator_del_from_boundary(page, zone);

	__ClearPageAerated(page);
	area->nr_free_aerated--;
#endif
}

static inline unsigned long aerator_raw_pages(struct free_area *area)
{
	return area->nr_free - area->nr_free_aerated;
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
#ifdef CONFIG_AERATION
	if (!static_key_false(&aerator_notify_enabled))
		return;
	if (order < AERATOR_MIN_ORDER)
		return;
	if (test_bit(ZONE_AERATION_REQUESTED, &zone->flags))
		return;
	if (aerator_raw_pages(&zone->free_area[order]) < AERATOR_HWM)
		return;

	__aerator_notify(zone);
#endif
}

void aerator_shutdown(void);
int aerator_startup(struct aerator_dev_info *sdev);
#endif /*_LINUX_MEMORY_AERATION_H */
