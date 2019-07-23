// SPDX-License-Identifier: GPL-2.0
/*
 * Page hinting core infrastructure to enable a VM to report free pages to its
 * hypervisor.
 *
 * Copyright Red Hat, Inc. 2019
 *
 * Author(s): Nitesh Narayan Lal <nitesh@redhat.com>
 */

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/page_hinting.h>
#include <linux/kvm_host.h>
#include <linux/memory.h>

/*
 * struct zone_free_area: For a single zone across NUMA nodes, it holds the
 * bitmap pointer to track the free pages and other required parameters
 * used to recover these pages by scanning the bitmap.
 * @bitmap:		Pointer to the bitmap in PAGE_HINTING_MIN_ORDER
 *			granularity.
 * @base_pfn:		Starting PFN value for the zone whose bitmap is stored.
 * @end_pfn:		Indicates the last PFN value for the zone.
 * @free_pages:		Tracks the number of free pages of granularity
 *			PAGE_HINTING_MIN_ORDER.
 * @nbits:		Indicates the total size of the bitmap in bits allocated
 *			at the time of initialization.
 */
struct zone_free_area {
	unsigned long *bitmap;
	unsigned long base_pfn;
	unsigned long end_pfn;
	atomic_t free_pages;
	unsigned long nbits;
} free_area[MAX_NR_ZONES];

static void init_hinting_wq(struct work_struct *work);
static DEFINE_MUTEX(page_hinting_init);
const struct page_hinting_config *page_hinting_conf;
struct work_struct hinting_work;
atomic_t hinting_work_enqueued, mem_change_active;

static void free_area_cleanup(int nr_zones)
{
	int zone_idx;

	for (zone_idx = 0; zone_idx < nr_zones; zone_idx++) {
		bitmap_free(free_area[zone_idx].bitmap);
		free_area[zone_idx].base_pfn = 0;
		free_area[zone_idx].end_pfn = 0;
		free_area[zone_idx].nbits = 0;
		atomic_set(&free_area[zone_idx].free_pages, 0);
	}
}

static int zone_free_area_init(void)
{
	unsigned long bitmap_size;
	struct zone *zone;
	int zone_idx;

	for_each_populated_zone(zone) {
		zone_idx = zone_idx(zone);
#ifdef CONFIG_ZONE_DEVICE
		if (zone_idx == ZONE_DEVICE)
			continue;
#endif
		spin_lock(&zone->lock);
		if (free_area[zone_idx].base_pfn) {
			free_area[zone_idx].base_pfn =
				min(free_area[zone_idx].base_pfn,
				    zone->zone_start_pfn);
			free_area[zone_idx].end_pfn =
				max(free_area[zone_idx].end_pfn,
				    zone->zone_start_pfn +
				    zone->spanned_pages);
		} else {
			free_area[zone_idx].base_pfn =
				zone->zone_start_pfn;
			free_area[zone_idx].end_pfn =
				zone->zone_start_pfn +
				zone->spanned_pages;
		}
		spin_unlock(&zone->lock);
	}
	for (zone_idx = 0; zone_idx < MAX_NR_ZONES; zone_idx++) {
		unsigned long pages = free_area[zone_idx].end_pfn -
				free_area[zone_idx].base_pfn;
		bitmap_size = (pages >> PAGE_HINTING_MIN_ORDER) + 1;
		if (!bitmap_size)
			continue;
		free_area[zone_idx].bitmap = bitmap_zalloc(bitmap_size,
							   GFP_KERNEL);
		if (!free_area[zone_idx].bitmap) {
			free_area_cleanup(zone_idx);
			mutex_unlock(&page_hinting_init);
			return -ENOMEM;
		}
		free_area[zone_idx].nbits = bitmap_size;
	}
	return 0;
}

static unsigned long pfn_to_bit(struct page *page, int zone_idx, unsigned long base_pfn)
{
	unsigned long bitnr;

	bitnr = (page_to_pfn(page) - base_pfn) >> PAGE_HINTING_MIN_ORDER;
	return bitnr;
}

#if defined(CONFIG_MEMORY_HOTPLUG) || defined(CONFIG_MEMORY_HOTREMOVE)
/* copy_bitmap - copies the bitmap to a new location from the specified
 * zone_free_area object.
 *
 * @dst:	the destination where the bits from old bitmap will be copied.
 * @zone_idx:	index of zone_free_area consisting the source bitmap pointer.
 * @size:	number of bits to be copied in destination.
 * @prev_base:	base PFN value before hotplug/hotremove, this is used to recover
 *		the free PFNs in old bitmap.
 */
static void copy_bitmap(unsigned long *dst, int zone_idx, unsigned long size,
			unsigned long prev_base)
{
	unsigned long pfn, base_pfn ,*src;
	unsigned int oldbit, newbit;
	struct page *page;

	src =  free_area[zone_idx].bitmap;
	base_pfn = free_area[zone_idx].base_pfn;
	for_each_set_bit(oldbit, src, size) {
		pfn = (oldbit << PAGE_HINTING_MIN_ORDER) + prev_base;
		page = pfn_to_page(pfn);
		newbit = pfn_to_bit(page, zone_idx, base_pfn);
		if (newbit < size)
			set_bit(newbit, dst);
	}

}

/* TODO: Can we merge the following three functions? */
static void expand_bitmap(struct memory_notify *mn)
{
	unsigned long start_pfn, end_pfn, pages, bitmap_size, prev_base;
	unsigned long *remap;
	struct zone *zone;
	int zone_idx;

	start_pfn = mn->start_pfn;
	end_pfn = start_pfn + mn->nr_pages;
	zone = page_zone(pfn_to_page(start_pfn));
	zone_idx = zone_idx(zone);
	prev_base = free_area[zone_idx].base_pfn;
	zone = page_zone(pfn_to_page(start_pfn));
	/* Considering the memory hotplugged is contiguous to the existing
	 * memory.
	 */
	if (free_area[zone_idx].base_pfn != 0) {
		/* Do we even need the second check of end_pfn??? */
		if (free_area[zone_idx].base_pfn < start_pfn && free_area[zone_idx].end_pfn > end_pfn) 
			free_area[zone_idx].base_pfn = start_pfn;
		else if (end_pfn > free_area[zone_idx].end_pfn)
			free_area[zone_idx].end_pfn = end_pfn;
	} else {
		free_area[zone_idx].base_pfn = start_pfn;
		free_area[zone_idx].end_pfn = end_pfn;
	}
	pages = free_area[zone_idx].end_pfn - free_area[zone_idx].base_pfn;
	bitmap_size = (pages >> PAGE_HINTING_MIN_ORDER) + 1;
	if (!bitmap_size)
		return;
	remap = bitmap_zalloc(bitmap_size, GFP_KERNEL);
	if (!remap) {
		/*TODO: Print an error message*/
		/*TODO: Guest is probably running low on memory, shouldn't we
		 * disable page hinting all together.
		 */
		return;
	}
//	copy_bitmap(remap, zone_idx, free_area[zone_idx].nbits, prev_base);
	bitmap_free(free_area[zone_idx].bitmap);
	free_area[zone_idx].nbits = bitmap_size;
	free_area[zone_idx].bitmap = remap;
}

static void shrink_bitmap(struct memory_notify *mn)
{
	unsigned long start_pfn, end_pfn, pages, bitmap_size, prev_base;
	unsigned long *remap;
	struct zone *zone;
	int zone_idx;

	start_pfn = mn->start_pfn;
	end_pfn = start_pfn + mn->nr_pages;
	zone = page_zone(pfn_to_page(start_pfn));
	zone_idx = zone_idx(zone);
	prev_base = free_area[zone_idx].base_pfn;

	/* Considering memory will always be removed from low to high? */
	if (free_area[zone_idx].base_pfn <= start_pfn) {
		if (free_area[zone_idx].end_pfn > end_pfn)
			/* why not end_pfn + 1? Why the last PFN is not included in every hotunplug
			 * request? if for a hotunplug the last pfn was pfn then for the next
			 * request start should be pfn+1 and not pfn.*/
			free_area[zone_idx].base_pfn = end_pfn;
		else {
			/* When the entire zone is removed.
			 * TODO: Is this possible?.
			 * TODO: Do we have to cehck for wrong cases here?
			 * or those will be taken care by the hotplug code.
			 */
			free_area[zone_idx].base_pfn = 0;
			free_area[zone_idx].end_pfn = 0;
		}

	}
	/* TODO:
	 * If the hotremove creates a hole in the existing zone.
	 * Then, do we have to do anything??
	 */


	pages = free_area[zone_idx].end_pfn - free_area[zone_idx].base_pfn;
	bitmap_size = (pages >> PAGE_HINTING_MIN_ORDER) + 1;
	if (!bitmap_size)
		return;
	remap = bitmap_zalloc(bitmap_size, GFP_KERNEL);
	if (!remap) {
		/*TODO: Print an error message*/
		/*TODO: Guest is probably running low on memory, shouldn't we
		 * disable page hinting all together.
		 */
		return;
	}
//	copy_bitmap(remap, zone_idx, bitmap_size, prev_base);
	bitmap_free(free_area[zone_idx].bitmap);
	free_area[zone_idx].nbits = bitmap_size;
	free_area[zone_idx].bitmap = remap;
}

static void revert_bitmap(struct memory_notify *mn)
{
	unsigned long start_pfn, end_pfn, pages, bitmap_size, prev_base;
	struct zone *zone, *revert_zone;
	int revert_zone_idx, zone_idx;
	unsigned long *remap;

	start_pfn = mn->start_pfn;
	end_pfn = start_pfn + mn->nr_pages;
	revert_zone = page_zone(pfn_to_page(start_pfn));
	revert_zone_idx = zone_idx(revert_zone);
	prev_base = free_area[revert_zone_idx].base_pfn;
	free_area[revert_zone_idx].base_pfn = 0;
	free_area[revert_zone_idx].end_pfn = 0;
	/* TODO: Better way to do this?*/
	for_each_populated_zone(zone) {
		zone_idx = zone_idx(zone);
		if (zone_idx == revert_zone_idx) {
			spin_lock(&zone->lock);
			if (free_area[zone_idx].base_pfn) {
				free_area[zone_idx].base_pfn =
					min(free_area[zone_idx].base_pfn,
					    zone->zone_start_pfn);
				free_area[zone_idx].end_pfn =
					max(free_area[zone_idx].end_pfn,
					    zone->zone_start_pfn +
					    zone->spanned_pages);
			} else {
				free_area[zone_idx].base_pfn =
					zone->zone_start_pfn;
				free_area[zone_idx].end_pfn =
					zone->zone_start_pfn +
					zone->spanned_pages;
			}
			spin_unlock(&zone->lock);
		}
	}
	pages = free_area[revert_zone_idx].end_pfn -
			free_area[revert_zone_idx].base_pfn;
	bitmap_size = (pages >> PAGE_HINTING_MIN_ORDER) + 1;
	if (!bitmap_size)
		return;
	remap = bitmap_zalloc(bitmap_size, GFP_KERNEL);
	if (!remap) {
		/*TODO: Print an error message*/
		/*TODO: Guest is probably running low on memory, shouldn't we
		 * disable page hinting all together.
		 */
		return;
	}
	copy_bitmap(remap, revert_zone_idx, bitmap_size, prev_base);
	bitmap_free(free_area[revert_zone_idx].bitmap);
	free_area[revert_zone_idx].nbits = bitmap_size;
	free_area[revert_zone_idx].bitmap = remap;
}

static int __meminit page_hinting_memory_callback(struct notifier_block *self,
                                        unsigned long action, void *arg)
{
	struct memory_notify *mn = arg;

        switch (action) {
        case MEM_GOING_OFFLINE:
		/* mem_change_active prevents any free memory tracking/
		 * reporting until memory hotremove is going.
		 */
		atomic_set(&mem_change_active, 1);
		/* We have to ensure that if any pages are isolated or reported
		 * to the host then they must be returned back to the buddy
		 * before hot remove.
		 */
		while(atomic_read(&hinting_work_enqueued))
		{}
		shrink_bitmap(mn);
                break;
        case MEM_CANCEL_OFFLINE:
        case MEM_CANCEL_ONLINE:
        	revert_bitmap(mn);
		/* resume page hinting.*/
		atomic_set(&mem_change_active, 0);
		break;
	case MEM_GOING_ONLINE:
		/* mem_change_active prevents any free memory tracking/hinting
		 * until memory hotplug is going.
		 */
		atomic_set(&mem_change_active, 1);
		/* We have to ensure that if any pages are isolated or reported
		 * to the host then they must be returned back to the buddy
		 * before hot remove.
		 */
		while(atomic_read(&hinting_work_enqueued))
		{}
		expand_bitmap(mn);
                break;
        case MEM_OFFLINE:
        case MEM_ONLINE:
		/* resume page hinting.*/
		atomic_set(&mem_change_active, 0);
		break;
	}
        return NOTIFY_OK;
}
#endif

int __meminit page_hinting_enable(const struct page_hinting_config *conf)
{
	int ret = -EBUSY;

	mutex_lock(&page_hinting_init);
	if (!page_hinting_conf) {
		ret = zone_free_area_init();
		if (ret)
			return ret;
		page_hinting_conf = conf;
		INIT_WORK(&hinting_work, init_hinting_wq);
#if defined(CONFIG_MEMORY_HOTPLUG) || defined(CONFIG_MEMORY_HOTREMOVE)
		hotplug_memory_notifier(page_hinting_memory_callback, 100);
#endif
		ret = 0;
	}
	mutex_unlock(&page_hinting_init);
	return ret;
}
EXPORT_SYMBOL_GPL(page_hinting_enable);

void page_hinting_disable(void)
{
	cancel_work_sync(&hinting_work);
	page_hinting_conf = NULL;
	free_area_cleanup(MAX_NR_ZONES);
}
EXPORT_SYMBOL_GPL(page_hinting_disable);

static void release_buddy_pages(struct list_head *pages)
{
	unsigned long bitnr, base_pfn;
	int mt = 0, zone_idx, order;
	struct page *page, *next;
	struct zone *zone;

	list_for_each_entry_safe(page, next, pages, lru) {
		zone_idx = page_zonenum(page);
		base_pfn = free_area[zone_idx].base_pfn;
		zone = page_zone(page);
		bitnr = pfn_to_bit(page, zone_idx, base_pfn);
		spin_lock(&zone->lock);
		list_del(&page->lru);
		order = page_private(page);
		set_page_private(page, 0);
		mt = get_pageblock_migratetype(page);
		__free_one_page(page, page_to_pfn(page), zone,
				order, mt, false);
		spin_unlock(&zone->lock);
	}
}

static void bm_set_pfn(struct page *page)
{
	struct zone *zone = page_zone(page);
	int zone_idx = page_zonenum(page);
	unsigned long bitnr = 0, base_pfn;

	base_pfn = free_area[zone_idx].base_pfn;
	lockdep_assert_held(&zone->lock);
	bitnr = pfn_to_bit(page, zone_idx, base_pfn);
	/*
	 * TODO: fix possible underflows.
	 */
	if (free_area[zone_idx].bitmap &&
	    bitnr < free_area[zone_idx].nbits &&
	    !test_and_set_bit(bitnr, free_area[zone_idx].bitmap))
		atomic_inc(&free_area[zone_idx].free_pages);
}

static void scan_zone_free_area(int zone_idx, int free_pages)
{
	int ret = 0, order, isolated_cnt = 0;
	unsigned long setbit, nbits;
	LIST_HEAD(isolated_pages);
	struct page *page;
	struct zone *zone;

	nbits = free_area[zone_idx].nbits;
	for_each_set_bit(setbit, free_area[zone_idx].bitmap, nbits) {
		ret = 0;
		page = pfn_to_online_page((setbit << PAGE_HINTING_MIN_ORDER) +
				free_area[zone_idx].base_pfn);
		if (!page)
			continue;
		zone = page_zone(page);
		spin_lock(&zone->lock);

		if (PageBuddy(page) && page_private(page) >=
		    PAGE_HINTING_MIN_ORDER) {
			order = page_private(page);
			ret = __isolate_free_page(page, order);
		}
		clear_bit(setbit, free_area[zone_idx].bitmap);
		atomic_dec(&free_area[zone_idx].free_pages);
		spin_unlock(&zone->lock);
		if (ret) {
			/*
			 * restoring page order to use it while releasing
			 * the pages back to the buddy.
			 */
			set_page_private(page, order);
			list_add_tail(&page->lru, &isolated_pages);
			isolated_cnt++;
			if (isolated_cnt == page_hinting_conf->max_pages) {
				page_hinting_conf->hint_pages(&isolated_pages);
				release_buddy_pages(&isolated_pages);
				isolated_cnt = 0;
			}
		}
	}
	if (isolated_cnt) {
		page_hinting_conf->hint_pages(&isolated_pages);
		release_buddy_pages(&isolated_pages);
	}
}

static void init_hinting_wq(struct work_struct *work)
{
	int zone_idx, free_pages;

	atomic_set(&hinting_work_enqueued, 1);
	for (zone_idx = 0; zone_idx < MAX_NR_ZONES; zone_idx++) {
		free_pages = atomic_read(&free_area[zone_idx].free_pages);
		if (free_pages >= page_hinting_conf->max_pages)
			scan_zone_free_area(zone_idx, free_pages);
	}
	atomic_set(&hinting_work_enqueued, 0);
}

void page_hinting_enqueue(struct page *page, int order)
{
	int zone_idx;

	if (!page_hinting_conf || order < PAGE_HINTING_MIN_ORDER ||
	    atomic_read(&mem_change_active))
		return;

	bm_set_pfn(page);
	if (atomic_read(&hinting_work_enqueued))
		return;
	zone_idx = zone_idx(page_zone(page));
	if (atomic_read(&free_area[zone_idx].free_pages) >=
			page_hinting_conf->max_pages)
		queue_work_on(smp_processor_id(), system_wq, &hinting_work);
}
