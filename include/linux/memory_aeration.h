/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MEMORY_AERATION_H
#define _LINUX_MEMORY_AERATION_H

#include <linux/pageblock-flags.h>
#include <linux/jump_label.h>
#include <asm/pgtable_types.h>

struct zone;

#define AERATOR_MIN_ORDER	pageblock_order

struct aerator_dev_info {
	unsigned long capacity;
	struct list_head batch_reactor;
	atomic_t refcnt;
	void (*react)(struct aerator_dev_info *a_dev_info);
};

extern struct static_key aerator_notify_enabled;

void aerator_cycle(void);
void __aerator_notify(struct zone *zone, int order);

/**
 * aerator_notify_free - Free page notification that will start page processing
 * @page: Last page processed
 * @zone: Pointer to current zone of last page processed
 * @order: Order of last page added to zone
 *
 * This function is meant to act as a screener for __aerator_notify which
 * will determine if a give zone has crossed over the high-water mark that
 * will justify us beginning page treatment. If we have crossed that
 * threshold then it will start the process of pulling some pages and
 * placing them in the batch_reactor list for treatment.
 */
static inline void
aerator_notify_free(struct page *page, struct zone *zone, int order)
{
	if (!static_key_false(&aerator_notify_enabled))
		return;

	if (order < AERATOR_MIN_ORDER)
		return;

	__aerator_notify(zone, order);
}

void aerator_shutdown(void);
int aerator_startup(struct aerator_dev_info *sdev);

#define AERATOR_ZONE_BITS	(BITS_TO_LONGS(MAX_NR_ZONES) * BITS_PER_LONG)
#define AERATOR_HWM_BITS	(AERATOR_ZONE_BITS * MAX_NUMNODES)
#endif /*_LINUX_MEMORY_AERATION_H */
