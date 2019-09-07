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

static struct page_reporting_dev_info __rcu *ph_dev_info __read_mostly;
struct list_head **boundary __read_mostly;

static inline struct list_head **get_boundary_ptr(unsigned int order,
						  unsigned int migratetype)
{
	return boundary +
	       (order - PAGE_REPORTING_MIN_ORDER) * MIGRATE_TYPES + migratetype;
}

static void page_reporting_reset_boundary(struct zone *zone, unsigned int order,
					  unsigned int migratetype)
{
	struct list_head **tail = get_boundary_ptr(order, migratetype);

	*tail = &zone->free_area[order].free_list[migratetype];
}

#define for_each_reporting_migratetype_order(_order, _type) \
	for (_order = MAX_ORDER; _order-- != PAGE_REPORTING_MIN_ORDER;) \
		for (_type = MIGRATE_TYPES; _type--;) \
			if (!is_migrate_isolate(_type))

static int page_reporting_populate_metadata(struct zone *zone)
{
	unsigned int order, mt;

	/*
	 * We need to make sure we have somewhere to store the tracking
	 * data for how many reported pages are in the zone. To do that
	 * we need to make certain zone->reported_pages is populated.
	 */
	if (!zone->reported_pages) {
		zone->reported_pages =
			kcalloc(MAX_ORDER - PAGE_REPORTING_MIN_ORDER,
				sizeof(unsigned long),
				GFP_KERNEL);
		if (!zone->reported_pages)
			return -ENOMEM;
	}

	/* Update boundary data to reflect the zone we are currently working */
	for_each_reporting_migratetype_order(order, mt)
		page_reporting_reset_boundary(zone, order, mt);

	return 0;
}

struct list_head *__page_reporting_get_boundary(unsigned int order,
						int migratetype)
{
	return *get_boundary_ptr(order, migratetype);
}

void page_reporting_del_from_boundary(struct page *page)
{
	unsigned int order = page_private(page);
	int mt = get_pcppage_migratetype(page);
	struct list_head **tail = get_boundary_ptr(order, mt);

	if (*tail == &page->lru)
		*tail = page->lru.next;
}

void page_reporting_add_to_boundary(struct page *page, int migratetype)
{
	unsigned int order = page_private(page);
	struct list_head **tail = get_boundary_ptr(order, migratetype);

	*tail = &page->lru;
	set_pcppage_migratetype(page, migratetype);
}

void page_reporting_move_to_boundary(struct page *page, struct zone *zone,
				     int dest_mt)
{
	/*
	 * We essentially have two options available to us. The first is to
	 * move the page from the boundary list on one migratetype to the
	 * list for the new migratetype assuming reporting is still active.
	 *
	 * The other option is to clear the reported state of the page as
	 * we will not be adding it to the group of pages that were already
	 * reported. It is cheaper to just rereport such pages then go
	 * through and do a special search to skip over them. If the page
	 * is being moved into isolation we can defer this until the page
	 * comes out of isolation since we do not scan the isolated
	 * migratetype.
	 */
	if (test_bit(ZONE_PAGE_REPORTING_ACTIVE, &zone->flags)) {
		page_reporting_del_from_boundary(page);
		page_reporting_add_to_boundary(page, dest_mt);
	} else if (!is_migrate_isolate(dest_mt)) {
		__del_page_from_reported_list(page, zone);
	}
}

static unsigned int page_reporting_fill(struct zone *zone,
					struct page_reporting_dev_info *phdev)
{
	struct scatterlist *sg = phdev->sg;
	unsigned int order, mt, count = 0;

	sg_init_table(phdev->sg, phdev->capacity);

	for_each_reporting_migratetype_order(order, mt) {
		struct page *page;

		/*
		 * Pull pages from free list until we have drained
		 * it or we have reached capacity.
		 */
		while ((page = get_unreported_page(zone, order, mt))) {
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
	__clear_bit(ZONE_PAGE_REPORTING_REQUESTED, &zone->flags);
	atomic_dec(&phdev->refcnt);

	return count;
}

static void page_reporting_drain(struct page_reporting_dev_info *phdev)
{
	struct scatterlist *sg = phdev->sg;

	/*
	 * Drain the now reported pages back into their respective
	 * free lists/areas. We assume at least one page is populated.
	 */
	do {
		free_reported_page(sg_page(sg), get_order(sg->length));
	} while (!sg_is_last(sg++));
}

/*
 * The page reporting cycle consists of 4 stages, fill, report, drain, and idle.
 * We will cycle through the first 3 stages until we fail to obtain any
 * pages, in that case we will switch to idle.
 */
static void page_reporting_cycle(struct zone *zone,
				 struct page_reporting_dev_info *phdev)
{
	/*
	 * Guarantee boundaries and stats are populated before we
	 * start placing reported pages in the zone.
	 */
	if (page_reporting_populate_metadata(zone))
		return;

	spin_lock_irq(&zone->lock);

	/* set bit indicating boundaries are present */
	__set_bit(ZONE_PAGE_REPORTING_ACTIVE, &zone->flags);

	do {
		/* Pull pages out of allocator into a scaterlist */
		unsigned int nents = page_reporting_fill(zone, phdev);

		/* no pages were acquired, give up */
		if (!nents)
			break;

		spin_unlock_irq(&zone->lock);

		/* begin processing pages in local list */
		phdev->report(phdev, nents);

		spin_lock_irq(&zone->lock);

		/*
		 * We should have a scatterlist of pages that have been
		 * processed. Return them to their original free lists.
		 */
		page_reporting_drain(phdev);

		/* keep pulling pages till there are none to pull */
	} while (test_bit(ZONE_PAGE_REPORTING_REQUESTED, &zone->flags));

	/* processing of the zone is complete, we can disable boundaries */
	__clear_bit(ZONE_PAGE_REPORTING_ACTIVE, &zone->flags);

	spin_unlock_irq(&zone->lock);
}

static void page_reporting_process(struct work_struct *work)
{
	struct delayed_work *d_work = to_delayed_work(work);
	struct page_reporting_dev_info *phdev =
		container_of(d_work, struct page_reporting_dev_info, work);
	struct zone *zone = first_online_pgdat()->node_zones;

	do {
		if (test_bit(ZONE_PAGE_REPORTING_REQUESTED, &zone->flags))
			page_reporting_cycle(zone, phdev);

		/* Move to next zone, if at end of list start over */
		zone = next_zone(zone) ? : first_online_pgdat()->node_zones;

		/*
		 * As long as refcnt has not reached zero there are still
		 * zones to be processed.
		 */
	} while (atomic_read(&phdev->refcnt));
}

/* request page reporting on this zone */
void __page_reporting_request(struct zone *zone)
{
	struct page_reporting_dev_info *phdev;

	rcu_read_lock();

	/*
	 * We use RCU to protect the ph_dev_info pointer. In almost all
	 * cases this should be present, however in the unlikely case of
	 * a shutdown this will be NULL and we should exit.
	 */
	phdev = rcu_dereference(ph_dev_info);
	if (unlikely(!phdev))
		goto out;

	/*
	 * We can use separate test and set operations here as there
	 * is nothing else that can set or clear this bit while we are
	 * holding the zone lock. The advantage to doing it this way is
	 * that we don't have to dirty the cacheline unless we are
	 * changing the value.
	 */
	__set_bit(ZONE_PAGE_REPORTING_REQUESTED, &zone->flags);

	/*
	 * Delay the start of work to allow a sizable queue to
	 * build. For now we are limiting this to running no more
	 * than 10 times per second.
	 */
	if (!atomic_fetch_inc(&phdev->refcnt))
		schedule_delayed_work(&phdev->work, HZ / 10);
out:
	rcu_read_unlock();
}

void __page_reporting_free_stats(struct zone *zone)
{
	/* free reported_page statisitics */
	kfree(zone->reported_pages);
	zone->reported_pages = NULL;
}

static DEFINE_MUTEX(page_reporting_mutex);
DEFINE_STATIC_KEY_FALSE(page_reporting_notify_enabled);

void page_reporting_shutdown(struct page_reporting_dev_info *phdev)
{
	mutex_lock(&page_reporting_mutex);

	if (rcu_access_pointer(ph_dev_info) == phdev) {
		/* Disable page reporting notification */
		static_branch_disable(&page_reporting_notify_enabled);
		RCU_INIT_POINTER(ph_dev_info, NULL);
		synchronize_rcu();

		/* Flush any existing work, and lock it out */
		cancel_delayed_work_sync(&phdev->work);

		/* Free scatterlist */
		kfree(phdev->sg);
		phdev->sg = NULL;

		/* Free boundaries */
		kfree(boundary);
		boundary = NULL;
	}

	mutex_unlock(&page_reporting_mutex);
}
EXPORT_SYMBOL_GPL(page_reporting_shutdown);

int page_reporting_startup(struct page_reporting_dev_info *phdev)
{
	struct zone *zone;
	int err = 0;

	/* No point in enabling this if it cannot handle any pages */
	if (!phdev->capacity)
		return -EINVAL;

	mutex_lock(&page_reporting_mutex);

	/* nothing to do if already in use */
	if (rcu_access_pointer(ph_dev_info)) {
		err = -EBUSY;
		goto err_out;
	}

	boundary = kcalloc(MAX_ORDER - PAGE_REPORTING_MIN_ORDER,
			   sizeof(struct list_head *) * MIGRATE_TYPES,
			   GFP_KERNEL);
	if (!boundary) {
		err = -ENOMEM;
		goto err_out;
	}

	/* allocate scatterlist to store pages being reported on */
	phdev->sg = kcalloc(phdev->capacity, sizeof(*phdev->sg), GFP_KERNEL);
	if (!phdev->sg) {
		err = -ENOMEM;

		kfree(boundary);
		boundary = NULL;

		goto err_out;
	}


	/* initialize refcnt and work structures */
	atomic_set(&phdev->refcnt, 0);
	INIT_DELAYED_WORK(&phdev->work, &page_reporting_process);

	/* assign device, and begin initial flush of populated zones */
	rcu_assign_pointer(ph_dev_info, phdev);
	for_each_populated_zone(zone) {
		spin_lock_irq(&zone->lock);
		__page_reporting_request(zone);
		spin_unlock_irq(&zone->lock);
	}

	/* enable page reporting notification */
	static_branch_enable(&page_reporting_notify_enabled);
err_out:
	mutex_unlock(&page_reporting_mutex);

	return err;
}
EXPORT_SYMBOL_GPL(page_reporting_startup);
