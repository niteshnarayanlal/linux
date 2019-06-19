// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/page-isolation.h>
#include <linux/gfp.h>
#include <linux/export.h>
#include <linux/delay.h>
#include <linux/slab.h>

static struct aerator_dev_info *a_dev_info;
struct static_key aerator_notify_enabled;

struct list_head *boundary[MAX_ORDER - AERATOR_MIN_ORDER][MIGRATE_TYPES];

static void aerator_reset_boundary(struct zone *zone, unsigned int order,
				   unsigned int migratetype)
{
	boundary[order - AERATOR_MIN_ORDER][migratetype] =
			&zone->free_area[order].free_list[migratetype];
}

#define for_each_aerate_migratetype_order(_order, _type) \
	for (_order = MAX_ORDER; _order-- != AERATOR_MIN_ORDER;) \
		for (_type = MIGRATE_TYPES; _type--;)

static void aerator_populate_boundaries(struct zone *zone)
{
	unsigned int order, mt;

	if (test_bit(ZONE_AERATION_ACTIVE, &zone->flags))
		return;

	for_each_aerate_migratetype_order(order, mt)
		aerator_reset_boundary(zone, order, mt);

	set_bit(ZONE_AERATION_ACTIVE, &zone->flags);
}

struct list_head *__aerator_get_tail(unsigned int order, int migratetype)
{
	return boundary[order - AERATOR_MIN_ORDER][migratetype];
}

void __aerator_del_from_boundary(struct page *page, struct zone *zone)
{
	unsigned int order = page_private(page) - AERATOR_MIN_ORDER;
	int mt = get_pcppage_migratetype(page);
	struct list_head **tail = &boundary[order][mt];

	if (*tail == &page->lru)
		*tail = page->lru.next;
}

void aerator_add_to_boundary(struct page *page, struct zone *zone)
{
	unsigned int order = page_private(page) - AERATOR_MIN_ORDER;
	int mt = get_pcppage_migratetype(page);
	struct list_head **tail = &boundary[order][mt];

	*tail = &page->lru;
}

void aerator_shutdown(void)
{
	static_key_slow_dec(&aerator_notify_enabled);

	while (atomic_read(&a_dev_info->refcnt))
		msleep(20);

	WARN_ON(!list_empty(&a_dev_info->batch));

	a_dev_info = NULL;
}
EXPORT_SYMBOL_GPL(aerator_shutdown);

static void aerator_schedule_initial_aeration(void)
{
	struct zone *zone;

	for_each_populated_zone(zone) {
		spin_lock(&zone->lock);
		__aerator_notify(zone);
		spin_unlock(&zone->lock);
	}
}

int aerator_startup(struct aerator_dev_info *sdev)
{
	if (a_dev_info)
		return -EBUSY;

	INIT_LIST_HEAD(&sdev->batch);
	atomic_set(&sdev->refcnt, 0);

	a_dev_info = sdev;
	aerator_schedule_initial_aeration();

	static_key_slow_inc(&aerator_notify_enabled);

	return 0;
}
EXPORT_SYMBOL_GPL(aerator_startup);

static void aerator_fill(struct zone *zone)
{
	struct list_head *batch = &a_dev_info->batch;
	int budget = a_dev_info->capacity;
	unsigned int order, mt;

	for_each_aerate_migratetype_order(order, mt) {
		struct page *page;

		/*
		 * Pull pages from free list until we have drained
		 * it or we have filled the batch reactor.
		 */
		while ((page = get_aeration_page(zone, order, mt))) {
			list_add_tail(&page->lru, batch);

			if (!--budget)
				return;
		}
	}

	/*
	 * If there are no longer enough free pages to fully populate
	 * the aerator, then we can just shut it down for this zone.
	 */
	clear_bit(ZONE_AERATION_REQUESTED, &zone->flags);
	atomic_dec(&a_dev_info->refcnt);
}

static void aerator_drain(struct zone *zone)
{
	struct list_head *list = &a_dev_info->batch;
	struct page *page;

	/*
	 * Drain the now aerated pages back into their respective
	 * free lists/areas.
	 */
	while ((page = list_first_entry_or_null(list, struct page, lru))) {
		list_del(&page->lru);
		put_aeration_page(zone, page);
	}
}

static void aerator_scrub_zone(struct zone *zone)
{
	/* See if there are any pages to pull */
	if (!test_bit(ZONE_AERATION_REQUESTED, &zone->flags))
		return;

	spin_lock(&zone->lock);

	do {
		aerator_fill(zone);

		if (list_empty(&a_dev_info->batch))
			break;

		spin_unlock(&zone->lock);

		/*
		 * Start aerating the pages in the batch, and then
		 * once that is completed we can drain the reactor
		 * and refill the reactor, restarting the cycle.
		 */
		a_dev_info->react(a_dev_info);

		spin_lock(&zone->lock);

		/*
		 * Guarantee boundaries are populated before we
		 * start placing aerated pages in the zone.
		 */
		aerator_populate_boundaries(zone);

		/*
		 * We should have a list of pages that have been
		 * processed. Return them to their original free lists.
		 */
		aerator_drain(zone);

		/* keep pulling pages till there are none to pull */
	} while (test_bit(ZONE_AERATION_REQUESTED, &zone->flags));

	clear_bit(ZONE_AERATION_ACTIVE, &zone->flags);

	spin_unlock(&zone->lock);
}

/**
 * aerator_cycle - start aerating a batch of pages, drain, and refill
 *
 * The aerator cycle consists of 4 stages, fill, react, drain, and idle.
 * We will cycle through the first 3 stages until we fail to obtain any
 * pages, in that case we will switch to idle and the thread will go back
 * to sleep awaiting the next request for aeration.
 */
static void aerator_cycle(struct work_struct *work)
{
	struct zone *zone = first_online_pgdat()->node_zones;
	int refcnt;

	/*
	 * We want to hold one additional reference against the number of
	 * active hints as we may clear the hint that originally brought us
	 * here. We will clear it after we have either vaporized the content
	 * of the pages, or if we discover all pages were stolen out from
	 * under us.
	 */
	atomic_inc(&a_dev_info->refcnt);

	for (;;) {
		aerator_scrub_zone(zone);

		/*
		 * Move to next zone, if at the end of the list
		 * test to see if we can just go into idle.
		 */
		zone = next_zone(zone);
		if (zone)
			continue;
		zone = first_online_pgdat()->node_zones;

		/*
		 * If we never generated any pages and we are
		 * holding the only remaining reference to active
		 * hints then we can just let this go for now and
		 * go idle.
		 */
		refcnt = atomic_read(&a_dev_info->refcnt);
		if (refcnt != 1)
			continue;
		if (atomic_try_cmpxchg(&a_dev_info->refcnt, &refcnt, 0))
			break;
	}
}

static DECLARE_DELAYED_WORK(aerator_work, &aerator_cycle);

void __aerator_notify(struct zone *zone)
{
	/*
	 * We can use separate test and set operations here as there
	 * is nothing else that can set or clear this bit while we are
	 * holding the zone lock. The advantage to doing it this way is
	 * that we don't have to dirty the cacheline unless we are
	 * changing the value.
	 */
	set_bit(ZONE_AERATION_REQUESTED, &zone->flags);

	if (atomic_fetch_inc(&a_dev_info->refcnt))
		return;

	/*
	 * We should never be calling this function while there are already
	 * pages in the list being aerated. If we are called under such a
	 * circumstance report an error.
	 */
	WARN_ON(!list_empty(&a_dev_info->batch));

	/*
	 * Delay the start of work to allow a sizable queue to build. For
	 * now we are limiting this to running no more than 10 times per
	 * second.
	 */
	schedule_delayed_work(&aerator_work, HZ / 10);
}
