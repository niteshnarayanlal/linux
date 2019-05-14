// SPDX-License-Identifier: GPL-2.0
/*
 * Free page hinting support to enable a VM to report the freed pages back
 * to the host.
 *
 * Copyright Red Hat, Inc. 2019
 *
 * Author(s): Nitesh Narayan Lal <nitesh@redhat.com>
 */

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/page_hinting.h>
#include <linux/kvm_host.h>

/*
 * struct hinting_bitmap: holds the bitmap pointer which tracks the freed PFNs
 * and other required variables which could help in retrieving the original PFN
 * value using the bitmap.
 * @bitmap:		Pointer to the bitmap of free PFN.
 * @base_pfn:		Starting PFN value for the zone whose bitmap is stored.
 * @free_mem_cnt:	Tracks the total amount of memory freed corresponding
 *			to the zone on a granularity of PAGE_HINTING_MIN_ORDER.
 * @bm_size:		Indicates the total size of the bitmap allocated at the
 *			time of initialization.
 */

struct hinting_bitmap {
	unsigned long *bitmap;
	unsigned long base_pfn;
	atomic_t free_mem_cnt;
	unsigned long bm_size;
} bm_zone[MAX_NR_ZONES];

/*
 * struct guest_isolated_pages- holds the buddy isolated pages which are
 * supposed to be freed by the host.
 * @pfn: page frame number for the isolated page.
 * @order: order of the isolated page.
 */
struct guest_isolated_pages {
	unsigned long pfn;
	u64 len;
};

struct work_struct hinting_work;
void init_hinting_wq(struct work_struct *work);
extern bool page_hinting_flag;
void *vb_obj;
void (*request_hypercall)(void *vb_obj, void *hinting_req, int entries);

void page_hinting_enable(void *vb, void (*vb_callback)(void *, void *, int))
{
	struct zone *zone;
	int idx = 0;
	unsigned long bitmap_size = 0;

	for_each_zone(zone) {
		bitmap_size = (zone->spanned_pages << PAGE_SHIFT) >>
				PAGE_HINTING_MIN_ORDER;
		bm_zone[idx].bitmap = kmalloc(bitmap_size, GFP_KERNEL);
		memset(bm_zone[idx].bitmap, 0, bitmap_size);
		bm_zone[idx].bm_size = bitmap_size;
		bm_zone[idx].base_pfn = zone->zone_start_pfn;
		idx++;
	}
	vb_obj = vb;
	request_hypercall = vb_callback;
	page_hinting_flag = true;
	INIT_WORK(&hinting_work, init_hinting_wq);
}
EXPORT_SYMBOL_GPL(page_hinting_enable);

void page_hinting_disable(void)
{
	struct zone *zone;
	int idx = 0;

	cancel_work_sync(&hinting_work);
	page_hinting_flag = false;
	for_each_zone(zone)
		kfree(bm_zone[idx++].bitmap);
	vb_obj = NULL;
}
EXPORT_SYMBOL_GPL(page_hinting_disable);

struct page *get_buddy_page(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned int order;

	for (order = 0; order < MAX_ORDER; order++) {
		struct page *page_head = page - (pfn & ((1 << order) - 1));

		if (PageBuddy(page_head) && page_private(page_head) >= order)
			return page_head;
	}
	return NULL;
}

void release_buddy_pages(void *hinting_req, int entries)
{
	int i = 0, mt = 0, zonenum;
	struct page *page;
	struct zone *zone;
	struct guest_isolated_pages *isolated_pages_obj = hinting_req;
	unsigned long flags = 0, bitmap_no;
	u32 order;

	while (i < entries) {
		page = pfn_to_page(isolated_pages_obj[i].pfn);
		zonenum = page_zonenum(page);
		zone = page_zone(page);
		bitmap_no = (isolated_pages_obj[i].pfn - zone->zone_start_pfn)
				>> PAGE_HINTING_MIN_ORDER;
		order = isolated_pages_obj[i].len >> 32;

		spin_lock_irqsave(&zone->lock, flags);
		mt = get_pageblock_migratetype(page);
		__free_one_page(page, page_to_pfn(page), zone, order, mt);
		bitmap_clear(bm_zone[zonenum].bitmap, bitmap_no, 1);
		spin_unlock_irqrestore(&zone->lock, flags);
		i++;
	}
}

void page_hinting_report(struct guest_isolated_pages *isolated_pages_obj,
			 int entries)
{
	if (vb_obj)
		request_hypercall(vb_obj, isolated_pages_obj,
				  entries);
	release_buddy_pages(isolated_pages_obj, entries);
}

void bm_set_pfn(struct page *page)
{
	int bitmap_no = 0;
	int zonenum = page_zonenum(page);

	bitmap_no = (page_to_pfn(page) - bm_zone[zonenum].base_pfn) >>
			PAGE_HINTING_MIN_ORDER;
	if (bitmap_no <= bm_zone[zonenum].bm_size) {
		bitmap_set(bm_zone[zonenum].bitmap, bitmap_no, 1);
		atomic_inc(&bm_zone[zonenum].free_mem_cnt);
	}
}

static void scan_hinting_bitmap(int zonenum)
{
	unsigned long flags = 0;
	unsigned long set_bit, start = 0;
	struct page *page;
	struct zone *zone;
	struct guest_isolated_pages *isolated_pages_obj;
	int isolated_idx = 0, ret = 0;
	u32 len;

	isolated_pages_obj = kmalloc(HINTING_MEM_THRESHOLD *
			sizeof(struct guest_isolated_pages), GFP_KERNEL);
	if (!isolated_pages_obj)
		return;
	for (;;) {
		set_bit = find_next_bit(bm_zone[zonenum].bitmap,
					bm_zone[zonenum].bm_size, start);
		if (set_bit >= bm_zone[zonenum].bm_size)
			break;
		page = pfn_to_page((set_bit << PAGE_HINTING_MIN_ORDER) +
				bm_zone[zonenum].base_pfn);
		zone = page_zone(page);
		atomic_dec(&bm_zone[zonenum].free_mem_cnt);
		spin_lock_irqsave(&zone->lock, flags);

		if (PageBuddy(page) && page_private(page) >=
		    PAGE_HINTING_MIN_ORDER) {
			int buddy_order = page_private(page);
			unsigned long pfn = page_to_pfn(page);

			ret = __isolate_free_page(page, buddy_order);
			if (ret) {
				isolated_pages_obj[isolated_idx].pfn = pfn;
				len = (1 << buddy_order) * 4;
				isolated_pages_obj[isolated_idx].len =
					((uint64_t)buddy_order << 32) | len;
				isolated_idx += 1;
			}
		}
		spin_unlock_irqrestore(&zone->lock, flags);

		if (isolated_idx >= HINTING_MEM_THRESHOLD) {
			page_hinting_report(isolated_pages_obj, isolated_idx);
			isolated_idx = 0;
		}
		start = set_bit + 1;
	}
	if (isolated_idx > 0) {
		int i = 0;
		int mt = 0;

		spin_lock_irqsave(&zone->lock, flags);
		while (i < isolated_idx) {
			unsigned long pfn = isolated_pages_obj[i].pfn;
			struct page *page = pfn_to_page(pfn);
			u32 order = isolated_pages_obj[i].len >> 32;

			mt = get_pageblock_migratetype(page);
			__free_one_page(page, pfn, zone, order, mt);
			atomic_inc(&bm_zone[zonenum].free_mem_cnt);
			i++;
		}
		spin_unlock_irqrestore(&zone->lock, flags);
	}
	kfree(isolated_pages_obj);
}

bool check_hinting_threshold(void)
{
	int zonenum = 0;

	while (zonenum < MAX_NR_ZONES) {
		if (atomic_read(&bm_zone[zonenum].free_mem_cnt) >=
				HINTING_MEM_THRESHOLD)
			return true;
		zonenum++;
	}
	return false;
}

void init_hinting_wq(struct work_struct *work)
{
	int zonenum = 0;

	while (zonenum < MAX_NR_ZONES) {
		if (atomic_read(&bm_zone[zonenum].free_mem_cnt) >=
		    HINTING_MEM_THRESHOLD) {
			atomic_sub(HINTING_MEM_THRESHOLD,
				   &bm_zone[zonenum].free_mem_cnt);
			scan_hinting_bitmap(zonenum);
		}
		zonenum++;
	}
}

void page_hinting_enqueue(struct page *page, int order)
{
	if (!page_hinting_flag)
		return;
	if (PageBuddy(page) && order >= PAGE_HINTING_MIN_ORDER) {
		bm_set_pfn(page);
	} else {
		struct page *buddy_page = get_buddy_page(page);

		if (buddy_page && page_private(buddy_page) >=
		    PAGE_HINTING_MIN_ORDER)
			bm_set_pfn(buddy_page);
	}
	if (check_hinting_threshold())
		queue_work(system_wq, &hinting_work);
}
