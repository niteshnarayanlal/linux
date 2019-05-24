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
 * @nbits:		Indicates the total size of the bitmap in bits allocated
 *			at the time of initialization.
 */
struct hinting_bitmap {
	unsigned long *bitmap;
	unsigned long base_pfn;
	atomic_t free_mem_cnt;
	unsigned long nbits;
} bm_zone[MAX_NR_ZONES];

static void init_hinting_wq(struct work_struct *work);
extern int __isolate_free_page(struct page *page, unsigned int order);
extern void __free_one_page(struct page *page, unsigned long pfn,
			    struct zone *zone, unsigned int order,
			    int migratetype, bool hint);
struct hinting_cb *hcb;
struct work_struct hinting_work;

unsigned long freed, captured, scanned, isolated, returned, reported;
int sys_init_cnt;

#ifdef CONFIG_SYSFS
#define HINTING_ATTR_RO(_name) \
		static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

static ssize_t hinting_memory_stats_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sprintf(buf, "Freed memory:%lu KB\nCaptured memory:%lu KB\n"
		       "Scanned memory:%lu KB\nIsolated memory:%lu KB\n"
		       "Reported memory:%lu KB\nReturned memory:%lu KB\n",
		       freed, captured, scanned, isolated,
		       reported, returned);
}

HINTING_ATTR_RO(hinting_memory_stats);

static struct attribute *hinting_attrs[] = {
	&hinting_memory_stats_attr.attr,
	NULL,
};

static const struct attribute_group hinting_attr_group = {
	.attrs = hinting_attrs,
	.name = "hinting",
};
#endif

static unsigned long find_bitmap_size(struct zone *zone)
{
	unsigned long nbits = ALIGN(zone->spanned_pages,
			    PAGE_HINTING_MIN_ORDER);

	nbits = nbits >> PAGE_HINTING_MIN_ORDER;
	return nbits;
}

void page_hinting_enable(struct hinting_cb *callback)
{
	struct zone *zone;
	int idx = 0;
	unsigned long bitmap_size = 0;

	for_each_populated_zone(zone) {
		spin_lock(&zone->lock);
		bitmap_size = find_bitmap_size(zone);
		bm_zone[idx].bitmap = bitmap_zalloc(bitmap_size, GFP_KERNEL);
		if (!bm_zone[idx].bitmap)
			return;
		bm_zone[idx].nbits = bitmap_size;
		bm_zone[idx].base_pfn = zone->zone_start_pfn;
		spin_unlock(&zone->lock);
		idx++;
	}
	hcb = callback;
	INIT_WORK(&hinting_work, init_hinting_wq);
}
EXPORT_SYMBOL_GPL(page_hinting_enable);

void page_hinting_disable(void)
{
	struct zone *zone;
	int idx = 0;

	cancel_work_sync(&hinting_work);
	hcb = NULL;
	for_each_populated_zone(zone) {
		spin_lock(&zone->lock);
		bitmap_free(bm_zone[idx].bitmap);
		bm_zone[idx].base_pfn = 0;
		bm_zone[idx].nbits = 0;
		atomic_set(&bm_zone[idx].free_mem_cnt, 0);
		spin_unlock(&zone->lock);
		idx++;
	}
}
EXPORT_SYMBOL_GPL(page_hinting_disable);

static unsigned long pfn_to_bit(struct page *page, int zonenum)
{
	unsigned long bitnr;

	bitnr = (page_to_pfn(page) - bm_zone[zonenum].base_pfn)
			 >> PAGE_HINTING_MIN_ORDER;
	return bitnr;
}

void release_buddy_pages(u64 phys_addr, u32 len)
{
	int mt = 0, zonenum;
	struct page *page;
	struct zone *zone;
	unsigned long bitnr, pfn;
	u32 order;

	pfn = phys_addr >> PAGE_SHIFT;
	page = pfn_to_online_page(pfn);
	if (!page)
		return;
	zonenum = page_zonenum(page);
	zone = page_zone(page);
	bitnr = pfn_to_bit(page, zonenum);
	order = ilog2(len / PAGE_SIZE);

	spin_lock(&zone->lock);
	mt = get_pageblock_migratetype(page);
	__free_one_page(page, page_to_pfn(page), zone, order, mt,
			false);
	spin_unlock(&zone->lock);
}
EXPORT_SYMBOL_GPL(release_buddy_pages);

static void bm_set_pfn(struct page *page)
{
	unsigned long bitnr = 0;
	int zonenum = page_zonenum(page);
	struct zone *zone = page_zone(page);

	captured += (1 << page_private(page)) * 4;
	lockdep_assert_held(&zone->lock);
	bitnr = pfn_to_bit(page, zonenum);
	if (bm_zone[zonenum].bitmap &&
	    bitnr < bm_zone[zonenum].nbits &&
	    !test_and_set_bit(bitnr, bm_zone[zonenum].bitmap))
		atomic_inc(&bm_zone[zonenum].free_mem_cnt);
}

static void scan_hinting_bitmap(int zonenum, int free_mem_cnt)
{
	unsigned long set_bit, start = 0;
	struct page *page;
	struct zone *zone;
	int scan_cnt = 0, ret = 0, order;
	u32 len;
	u64 phys_addr;

	ret = hcb->prepare();
	if (!ret)
		return;
	for (;;) {
		ret = 0;
		set_bit = find_next_bit(bm_zone[zonenum].bitmap,
					bm_zone[zonenum].nbits, start);
		if (set_bit >= bm_zone[zonenum].nbits)
			break;
		page = pfn_to_online_page((set_bit << PAGE_HINTING_MIN_ORDER) +
				bm_zone[zonenum].base_pfn);
		if (!page)
			continue;
		zone = page_zone(page);
		scanned += (1 << page_private(page)) * 4;
		spin_lock(&zone->lock);

		if (PageBuddy(page) && page_private(page) >=
		    PAGE_HINTING_MIN_ORDER) {
			order = page_private(page);
			ret = __isolate_free_page(page, order);
		}
		clear_bit(set_bit, bm_zone[zonenum].bitmap);
		spin_unlock(&zone->lock);
		if (ret) {
			isolated += (1 << order) * 4;
			phys_addr = page_to_pfn(page) << PAGE_SHIFT;
			len = (1 << order) * PAGE_SIZE;
			hcb->hint_page(phys_addr, len);
		}
		start = set_bit + 1;
		scan_cnt++;
	}
	hcb->cleanup();
	if (scan_cnt > free_mem_cnt)
		atomic_sub((scan_cnt - free_mem_cnt),
			   &bm_zone[zonenum].free_mem_cnt);
}

static bool check_hinting_threshold(void)
{
	int zonenum = 0;

	for (; zonenum < MAX_NR_ZONES; zonenum++) {
		if (atomic_read(&bm_zone[zonenum].free_mem_cnt) >=
				HINTING_MEM_THRESHOLD)
			return true;
	}
	return false;
}

static void init_hinting_wq(struct work_struct *work)
{
	int zonenum = 0, free_mem_cnt = 0;

	for (; zonenum < MAX_NR_ZONES; zonenum++) {
		free_mem_cnt = atomic_read(&bm_zone[zonenum].free_mem_cnt);
		if (free_mem_cnt >= HINTING_MEM_THRESHOLD) {
			if (sys_init_cnt == 0) {
				int err = sysfs_create_group(mm_kobj,
						&hinting_attr_group);

				if (err)
					pr_err("hinting: register sysfs failed\n");
				sys_init_cnt = 1;
			}

			/* Find a better way to synchronize per zone
			 * free_mem_cnt.
			 */
			atomic_sub(free_mem_cnt,
				   &bm_zone[zonenum].free_mem_cnt);
			scan_hinting_bitmap(zonenum, free_mem_cnt);
		}
	}
}

void page_hinting_enqueue(struct page *page, int order)
{
	freed += (1 << order) * 4;
	if (hcb && order >= PAGE_HINTING_MIN_ORDER)
		bm_set_pfn(page);
	else
		return;

	if (check_hinting_threshold()) {
		int cpu = smp_processor_id();

		queue_work_on(cpu, system_wq, &hinting_work);
	}
}
