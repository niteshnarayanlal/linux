// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/page-isolation.h>
#include <linux/gfp.h>
#include <linux/export.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include "internal.h"

static struct page_hinting_dev_info __rcu *ph_dev_info __read_mostly;
struct static_key page_hinting_notify_enabled;

struct list_head *boundary[MAX_ORDER - PAGE_HINTING_MIN_ORDER][MIGRATE_TYPES];

static void page_hinting_reset_boundary(struct zone *zone, unsigned int order,
				   unsigned int migratetype)
{
	boundary[order - PAGE_HINTING_MIN_ORDER][migratetype] =
			&zone->free_area[order].free_list[migratetype];
}

#define for_each_hinting_migratetype_order(_order, _type) \
	for (_order = MAX_ORDER; _order-- != PAGE_HINTING_MIN_ORDER;) \
		for (_type = MIGRATE_TYPES; _type--;)

static int page_hinting_populate_metadata(struct zone *zone)
{
	unsigned int order, mt;

	/*
	 * We need to make sure we have somewhere to store the tracking
	 * data for how many hinted pages are in the zone. To do that
	 * we need to make certain zone->hinted_pages is populated.
	 */
	if (!zone->hinted_pages) {
		zone->hinted_pages = kcalloc(MAX_ORDER - PAGE_HINTING_MIN_ORDER,
					     sizeof(unsigned long),
					     GFP_KERNEL);
		if (!zone->hinted_pages)
			return -ENOMEM;
	}

	/* Update boundary data to reflect the zone we are currently working */
	for_each_hinting_migratetype_order(order, mt)
		page_hinting_reset_boundary(zone, order, mt);

	return 0;
}

struct list_head *__page_hinting_get_boundary(unsigned int order,
					      int migratetype)
{
	return boundary[order - PAGE_HINTING_MIN_ORDER][migratetype];
}

void page_hinting_del_from_boundary(struct page *page, struct zone *zone)
{
	unsigned int order = page_private(page) - PAGE_HINTING_MIN_ORDER;
	int mt = get_pcppage_migratetype(page);
	struct list_head **tail = &boundary[order][mt];

	if (*tail == &page->lru)
		*tail = page->lru.next;
}

void page_hinting_add_to_boundary(struct page *page, struct zone *zone,
			     int migratetype)
{
	unsigned int order = page_private(page) - PAGE_HINTING_MIN_ORDER;
	struct list_head **tail = &boundary[order][migratetype];

	*tail = &page->lru;
}

static unsigned int page_hinting_fill(struct zone *zone,
				      struct page_hinting_dev_info *phdev)
{
	struct scatterlist *sg = phdev->sg;
	unsigned int order, mt, count = 0;

	sg_init_table(phdev->sg, phdev->capacity);

	for_each_hinting_migratetype_order(order, mt) {
		struct page *page;

		/*
		 * Pull pages from free list until we have drained
		 * it or we have reached capacity.
		 */
		while ((page = get_unhinted_page(zone, order, mt))) {
			sg_set_page(&sg[count], page, PAGE_SIZE << order, 0);

			if (++count == phdev->capacity)
				return count;
		}
	}

	/* mark end of scatterlist due to underflow */
	if (count)
		sg_mark_end(&sg[count - 1]);

	/*
	 * If there are no longer enough free pages to fully populate
	 * the scatterlist, then we can just shut it down for this zone.
	 */
	clear_bit(ZONE_PAGE_HINTING_REQUESTED, &zone->flags);
	atomic_dec(&phdev->refcnt);

	return count;
}

static void page_hinting_drain(struct zone *zone,
			       struct page_hinting_dev_info *phdev)
{
	struct scatterlist *sg = phdev->sg;

	/*
	 * Drain the now hinted pages back into their respective
	 * free lists/areas. We assume at least one page is populated.
	 */
	do {
		put_hinted_page(zone, sg_page(sg));
	} while (!sg_is_last(sg++));
}

/*
 * The page hinting cycle consists of 4 stages, fill, react, drain, and idle.
 * We will cycle through the first 3 stages until we fail to obtain any
 * pages, in that case we will switch to idle.
 */
static void page_hinting_cycle(struct zone *zone,
			       struct page_hinting_dev_info *phdev)
{
	/*
	 * Guarantee boundaries and stats are populated before we
	 * start placing hinted pages in the zone.
	 */
	if (page_hinting_populate_metadata(zone))
		return;

	spin_lock(&zone->lock);

	/* set bit indicating boundaries are present */
	set_bit(ZONE_PAGE_HINTING_ACTIVE, &zone->flags);

	do {
		/* Pull pages out of allocator into a scaterlist */
		unsigned int num_hints = page_hinting_fill(zone, phdev);

		/* no pages were acquired, give up */
		if (!num_hints)
			break;

		spin_unlock(&zone->lock);

		/* begin processing pages in local list */
		phdev->react(phdev, num_hints);

		spin_lock(&zone->lock);

		/*
		 * We should have a scatterlist of pages that have been
		 * processed. Return them to their original free lists.
		 */
		page_hinting_drain(zone, phdev);

		/* keep pulling pages till there are none to pull */
	} while (test_bit(ZONE_PAGE_HINTING_REQUESTED, &zone->flags));

	/* processing of the zone is complete, we can disable boundaries */
	clear_bit(ZONE_PAGE_HINTING_ACTIVE, &zone->flags);

	spin_unlock(&zone->lock);
}

static void page_hinting_process(struct work_struct *work)
{
	struct delayed_work *d_work = to_delayed_work(work);
	struct page_hinting_dev_info *phdev =
		container_of(d_work, struct page_hinting_dev_info, work);
	struct zone *zone = first_online_pgdat()->node_zones;

	do {
		if (test_bit(ZONE_PAGE_HINTING_REQUESTED, &zone->flags))
			page_hinting_cycle(zone, phdev);

		/*
		 * Move to next zone, if at the end of the list
		 * test to see if we can just go into idle.
		 */
		zone = next_zone(zone);
		if (zone)
			continue;
		zone = first_online_pgdat()->node_zones;

		/*
		 * As long as refcnt has not reached zero there are still
		 * zones to be processed.
		 */
	} while (atomic_read(&phdev->refcnt));
}

/* request page hinting on this zone */
void __page_hinting_request(struct zone *zone)
{
	struct page_hinting_dev_info *phdev;

	rcu_read_lock();

	/*
	 * We use RCU to protect the ph_dev_info pointer. In almost all
	 * cases this should be present, however in the unlikely case of
	 * a shutdown this will be NULL and we should exit.
	 */
	phdev = rcu_dereference(ph_dev_info);
	if (unlikely(!phdev))
		return;

	/*
	 * We can use separate test and set operations here as there
	 * is nothing else that can set or clear this bit while we are
	 * holding the zone lock. The advantage to doing it this way is
	 * that we don't have to dirty the cacheline unless we are
	 * changing the value.
	 */
	set_bit(ZONE_PAGE_HINTING_REQUESTED, &zone->flags);

	/*
	 * Delay the start of work to allow a sizable queue to
	 * build. For now we are limiting this to running no more
	 * than 10 times per second.
	 */
	if (!atomic_fetch_inc(&phdev->refcnt))
		schedule_delayed_work(&phdev->work, HZ / 10);

	rcu_read_unlock();
}

void __page_hinting_free_stats(struct zone *zone)
{
	/* free hinted_page statisitics */
	kfree(zone->hinted_pages);
	zone->hinted_pages = NULL;
}

void page_hinting_shutdown(struct page_hinting_dev_info *phdev)
{
	if (rcu_access_pointer(ph_dev_info) != phdev)
		return;

	/* Disable page hinting notification */
	static_key_slow_dec(&page_hinting_notify_enabled);
	RCU_INIT_POINTER(ph_dev_info, NULL);
	synchronize_rcu();

	/* Flush any existing work, and lock it out */
	cancel_delayed_work_sync(&phdev->work);

	/* Free scatterlist */
	kfree(phdev->sg);
	phdev->sg = NULL;
}
EXPORT_SYMBOL_GPL(page_hinting_shutdown);

int page_hinting_startup(struct page_hinting_dev_info *phdev)
{
	struct zone *zone;

	/* nothing to do if already in use */
	if (rcu_access_pointer(ph_dev_info))
		return -EBUSY;

	/* allocate scatterlist to store pages being hinted on */
	phdev->sg = kcalloc(phdev->capacity, sizeof(*phdev->sg), GFP_KERNEL);
	if (!phdev->sg)
		return -ENOMEM;

	/* initialize refcnt and work structures */
	atomic_set(&phdev->refcnt, 0);
	INIT_DELAYED_WORK(&phdev->work, &page_hinting_process);

	/* assign device, and begin initial flush of populated zones */
	rcu_assign_pointer(ph_dev_info, phdev);
	for_each_populated_zone(zone) {
		spin_lock(&zone->lock);
		__page_hinting_request(zone);
		spin_unlock(&zone->lock);
	}

	/* enable page hinting notification */
	static_key_slow_inc(&page_hinting_notify_enabled);

	return 0;
}
EXPORT_SYMBOL_GPL(page_hinting_startup);

