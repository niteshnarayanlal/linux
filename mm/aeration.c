// SPDX-License-Identifier: GPL-2.0
#include <linux/memory_aeration.h>
#include <linux/mmzone.h>
#include <linux/gfp.h>
#include <linux/export.h>
#include <linux/delay.h>
#include <linux/slab.h>

static unsigned long *aerator_hwm;
static struct aerator_dev_info *a_dev_info;
struct static_key aerator_notify_enabled;

void aerator_shutdown(void)
{
	static_key_slow_dec(&aerator_notify_enabled);

	while (atomic_read(&a_dev_info->refcnt))
		msleep(20);

	kfree(aerator_hwm);
	aerator_hwm = NULL;

	a_dev_info = NULL;
}
EXPORT_SYMBOL_GPL(aerator_shutdown);

int aerator_startup(struct aerator_dev_info *sdev)
{
	size_t size = BITS_TO_LONGS(AERATOR_HWM_BITS) * sizeof(unsigned long);
	unsigned long *hwm;

	if (a_dev_info || aerator_hwm)
		return -EBUSY;

	a_dev_info = sdev;

	atomic_set(&sdev->refcnt, 0);

	hwm = kzalloc(size, GFP_KERNEL);
	if (!hwm) {
		aerator_shutdown();
		return -ENOMEM;
	}

	aerator_hwm = hwm;

	static_key_slow_inc(&aerator_notify_enabled);

	return 0;
}
EXPORT_SYMBOL_GPL(aerator_startup);

static inline unsigned long *get_aerator_hwm(int nid)
{
	if (!aerator_hwm)
		return NULL;

	return aerator_hwm + (BITS_TO_LONGS(MAX_NR_ZONES) * nid);
}

static int __aerator_fill(struct zone *zone, unsigned int size)
{
	struct list_head *batch = &a_dev_info->batch_reactor;
	unsigned long nr_raw = 0;
	unsigned int len = 0;
	unsigned int order;

	for (order = MAX_ORDER; order-- != AERATOR_MIN_ORDER;) {
		struct free_area *area = &(zone->free_area[order]);
		int mt = area->treatment_mt;

		/*
		 * If there are no untreated pages to pull
		 * then we might as well skip the area.
		 */
		while (area->nr_free_raw) {
			unsigned int count = 0;
			struct page *page;

			/*
			 * If we completed aeration we can let the current
			 * free list work on settling so that a batch of
			 * new raw pages can build. In the meantime move on
			 * to the next migratetype.
			 */
			if (++mt >= MIGRATE_TYPES)
				mt = 0;

			/*
			 * Pull pages from free list until we have drained
			 * it or we have filled the batch reactor.
			 */
			while ((page = get_raw_pages(zone, order, mt))) {
				list_add(&page->lru, batch);

				if (++count == (size - len))
					return size;
			}

			/*
			 * If we pulled any pages from this migratetype then
			 * we must move on to a new free area as we cannot
			 * move the membrane until after we have decanted the
			 * pages currently being aerated.
			 */
			if (count) {
				len += count;
				break;
			}
		}

		/*
		 * Keep a running total of the raw packets we have left
		 * behind. We will use this to determine if we should
		 * clear the HWM flag.
		 */
		nr_raw += area->nr_free_raw;
	}

	/*
	 * If there are no longer enough free pages to fully populate
	 * the aerator, then we can just shut it down for this zone.
	 */
	if (nr_raw < a_dev_info->capacity) {
		unsigned long *hwm = get_aerator_hwm(zone_to_nid(zone));

		clear_bit(zone_idx(zone), hwm);
		atomic_dec(&a_dev_info->refcnt);
	}

	return len;
}

static unsigned int aerator_fill(int nid, int zid, int budget)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	struct zone *zone = &pgdat->node_zones[zid];
	unsigned long flags;
	int len;

	spin_lock_irqsave(&zone->lock, flags);

	/* fill aerator with "raw" pages */
	len = __aerator_fill(zone, budget);

	spin_unlock_irqrestore(&zone->lock, flags);

	return len;
}

static void aerator_fill_and_react(void)
{
	int budget = a_dev_info->capacity;
	int nr;

	/*
	 * We should never be calling this function while there are already
	 * pages in the reactor being aerated. If we are called under such
	 * a circumstance report an error.
	 */
	BUG_ON(!list_empty(&a_dev_info->batch_reactor));
retry:
	/*
	 * We want to hold one additional reference against the number of
	 * active hints as we may clear the hint that originally brought us
	 * here. We will clear it after we have either vaporized the content
	 * of the pages, or if we discover all pages were stolen out from
	 * under us.
	 */
	atomic_inc(&a_dev_info->refcnt);

	for_each_set_bit(nr, aerator_hwm, AERATOR_HWM_BITS) {
		int node_id = nr / AERATOR_ZONE_BITS;
		int zone_id = nr % AERATOR_ZONE_BITS;

		budget -= aerator_fill(node_id, zone_id, budget);
		if (!budget)
			goto start_aerating;
	}

	if (unlikely(list_empty(&a_dev_info->batch_reactor))) {
		/*
		 * If we never generated any pages, and we were holding the
		 * only remaining reference to active hints then we can
		 * just let this go for now and go idle.
		 */
		if (atomic_dec_and_test(&a_dev_info->refcnt))
			return;

		/*
		 * There must be a bit populated somewhere, try going
		 * back through and finding it.
		 */
		goto retry;
	}

start_aerating:
	a_dev_info->react(a_dev_info);
}

void aerator_decant(void)
{
	struct list_head *list = &a_dev_info->batch_reactor;
	struct page *page;

	/*
	 * This function should never be called on an empty list. If so it
	 * points to a bug as we should never be running the aerator when
	 * the list is empty.
	 */
	WARN_ON(list_empty(&a_dev_info->batch_reactor));

	while ((page = list_first_entry_or_null(list, struct page, lru))) {
		list_del(&page->lru);

		__SetPageTreated(page);

		free_treated_page(page);
	}
}

/**
 * aerator_cycle - drain, fill, and start aerating another batch of pages
 *
 * This function is at the heart of the aerator. It should be called after
 * the previous batch of pages has finished being processed by the aerator.
 * It will drain the aerator, refill it, and start the next set of pages
 * being processed.
 */
void aerator_cycle(void)
{
	aerator_decant();

	/*
	 * Now that the pages have been flushed we can drop our reference to
	 * the active hints list. If there are no further hints that need to
	 * be processed we can simply go idle.
	 */
	if (atomic_dec_and_test(&a_dev_info->refcnt))
		return;

	aerator_fill_and_react();
}
EXPORT_SYMBOL_GPL(aerator_cycle);

static void __aerator_fill_and_react(struct zone *zone)
{
	/*
	 * We should never be calling this function while there are already
	 * pages in the list being aerated. If we are called under such a
	 * circumstance report an error.
	 */
	BUG_ON(!list_empty(&a_dev_info->batch_reactor));

	/*
	 * We want to hold one additional reference against the number of
	 * active hints as we may clear the hint that originally brought us
	 * here. We will clear it after we have either vaporized the content
	 * of the pages, or if we discover all pages were stolen out from
	 * under us.
	 */
	atomic_inc(&a_dev_info->refcnt);

	__aerator_fill(zone, a_dev_info->capacity);

	if (unlikely(list_empty(&a_dev_info->batch_reactor))) {
		/*
		 * If we never generated any pages, and we were holding the
		 * only remaining reference to active hints then we can just
		 * let this go for now and go idle.
		 */
		if (atomic_dec_and_test(&a_dev_info->refcnt))
			return;

		/*
		 * Another zone must have populated some raw pages that
		 * need to be processed. Release the zone lock and process
		 * that zone instead.
		 */
		spin_unlock(&zone->lock);
		aerator_fill_and_react();
	} else {
		/* Release the zone lock and begin the page aerator */
		spin_unlock(&zone->lock);
		a_dev_info->react(a_dev_info);
	}

	/* Reaquire lock so we can resume processing this zone */
	spin_lock(&zone->lock);
}

void __aerator_notify(struct zone *zone, int order)
{
	int node_id = zone_to_nid(zone);
	int zone_id = zone_idx(zone);
	unsigned long *hwm;

	if (zone->free_area[order].nr_free_raw < (2 * a_dev_info->capacity))
		return;

	hwm = get_aerator_hwm(node_id);

	/*
	 * We an use separate test and set operations here as there
	 * is nothing else that can set or clear this bit while we are
	 * holding the zone lock. The advantage to doing it this way is
	 * that we don't have to dirty the cacheline unless we are
	 * changing the value.
	 */
	if (test_bit(zone_id, hwm))
		return;
	set_bit(zone_id, hwm);

	if (atomic_fetch_inc(&a_dev_info->refcnt))
		return;

	__aerator_fill_and_react(zone);
}
EXPORT_SYMBOL_GPL(__aerator_notify);

