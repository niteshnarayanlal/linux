#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/page_hinting.h>
#include <linux/kvm_host.h>

/*
 * struct hinitng_bitmap: holds the bitmap pointer which tracks the freed PFNs
 * and other required variables which could help in retriving the original PFN
 * value using the bitmap.
 * @bitmap:		Pointer to the bitmap of free PFN.
 * @base_pfn:		Starting PFN value for the zone whose bitmap is stored.
 * @free_mem_cnt:	Tracks the total amount of memory freed corresponding
 *			to the zone.
 * @bm_size:		Indicates the total size of the bitmap allocated at the
 *			time of initialization.
 */

struct hinting_bitmap {
	unsigned long *bitmap;
	unsigned long base_pfn;
	atomic_t free_mem_cnt;
	unsigned long bm_size;
} bm_zone[MAX_ZONES];

/*
 * struct guest_isolated_pages- holds the buddy isolated pages which are
 * supposed to be freed by the host.
 * @pfn: page frame number for the isolated page.
 * @order: order of the isolated page.
 */
struct guest_isolated_pages {
	unsigned long pfn;
	unsigned long len;
	unsigned int order;
};

struct work_struct hinting_work;
void init_hinting_wq(struct work_struct *work);

int guest_free_page_hinting_flag;
struct static_key_false guest_free_page_hinting_key;
static DEFINE_MUTEX(hinting_mutex);

void *vb_obj;
void (*request_hypercall)(void *vb_obj, void *hinting_req, int entries);

int guest_free_page_hinting_sysctl(struct ctl_table *table, int write,
				   void __user *buffer, size_t *lenp,
				   loff_t *ppos)
{
	int ret;

	mutex_lock(&hinting_mutex);
	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (guest_free_page_hinting_flag)
		static_key_enable(&guest_free_page_hinting_key.key);
	else
		static_key_disable(&guest_free_page_hinting_key.key);
	mutex_unlock(&hinting_mutex);
	return ret;
}

void guest_free_page_hinting_enable(void *vb,
				    void (*vb_callback)(void *, void *, int))
{
	struct zone *zone;
	int idx = 0;
	unsigned long bitmap_size = 0;

	for_each_zone(zone) {
		bitmap_size = zone->spanned_pages;
		bm_zone[idx].bitmap = kmalloc(bitmap_size, GFP_KERNEL);
		memset(bm_zone[idx].bitmap, 0, bitmap_size);
		bm_zone[idx].bm_size = bitmap_size;
		bm_zone[idx].base_pfn = zone->zone_start_pfn;
		idx++;
	}
	vb_obj = vb;
	request_hypercall = vb_callback;
	guest_free_page_hinting_flag = 1;
	static_branch_enable(&guest_free_page_hinting_key);
	INIT_WORK(&hinting_work, init_hinting_wq);
}
EXPORT_SYMBOL_GPL(guest_free_page_hinting_enable);

void guest_free_page_hinting_disable(void)
{
	struct zone *zone;
	int idx = 0;

	cancel_work_sync(&hinting_work);
	vb_obj = NULL;
	for_each_zone(zone)
		kfree(bm_zone[idx++].bitmap);
	guest_free_page_hinting_flag = 0;
	static_branch_enable(&guest_free_page_hinting_key);
}
EXPORT_SYMBOL_GPL(guest_free_page_hinting_disable);

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

	while (i < entries) {
		page = pfn_to_page(isolated_pages_obj[i].pfn);
		zonenum = page_zonenum(page);
		zone = page_zone(page);
		bitmap_no = (isolated_pages_obj[i].pfn - zone->zone_start_pfn)
				>> FREE_PAGE_HINTING_MIN_ORDER;

		spin_lock_irqsave(&zone->lock, flags);
		mt = get_pageblock_migratetype(page);
		__free_one_page(page, page_to_pfn(page), zone,
				isolated_pages_obj[i].order, mt);
		bitmap_clear(bm_zone[zonenum].bitmap, bitmap_no, 1);
		spin_unlock_irqrestore(&zone->lock, flags);
		i++;
	}
}

void guest_free_page_report(struct guest_isolated_pages *isolated_pages_obj,
			    int entries)
{
	if (vb_obj)
		request_hypercall(vb_obj, isolated_pages_obj,
				  entries);
	release_buddy_pages(isolated_pages_obj, entries);
}

void set_bitmap(struct page *page, int zonenum)
{
	int bitmap_no = 0;

	bitmap_no = (page_to_pfn(page) - bm_zone[zonenum].base_pfn) >> FREE_PAGE_HINTING_MIN_ORDER;
	if (bitmap_no <= bm_zone[zonenum].bm_size) {
		bitmap_set(bm_zone[zonenum].bitmap, bitmap_no, 1);
		atomic_inc(&bm_zone[zonenum].free_mem_cnt);
	}
}

static void guest_free_page_hinting(int zonenum)
{
	unsigned long flags = 0;
	unsigned long set_bit, start = 0;
	struct page *page;
	struct zone *zone;
	struct guest_isolated_pages *isolated_pages_obj;
	int isolated_idx = 0;
	int ret = 0;

	isolated_pages_obj = kmalloc(HINTING_THRESHOLD *
			sizeof(struct guest_isolated_pages), GFP_KERNEL);
	if (!isolated_pages_obj)
		return;
	for (;;) {
		set_bit = find_next_bit(bm_zone[zonenum].bitmap,
					bm_zone[zonenum].bm_size, start);
		if (set_bit >= bm_zone[zonenum].bm_size)
			break;
		page = pfn_to_page((set_bit << FREE_PAGE_HINTING_MIN_ORDER) +
				bm_zone[zonenum].base_pfn);
		zone = page_zone(page);
		spin_lock_irqsave(&zone->lock, flags);
		if (PageBuddy(page) && page_private(page) >=
		    FREE_PAGE_HINTING_MIN_ORDER) {
			int buddy_order = page_private(page);
			unsigned long pfn = page_to_pfn(page);

			ret = __isolate_free_page(page, buddy_order);
			if (ret) {
				isolated_pages_obj[isolated_idx].pfn = pfn;
				isolated_pages_obj[isolated_idx].len =
					(1 << buddy_order) * 4;
				isolated_pages_obj[isolated_idx].order =
					buddy_order;
				isolated_idx += 1;
			}
		}
		spin_unlock_irqrestore(&zone->lock, flags);
		if (isolated_idx >= HINTING_THRESHOLD) {
			guest_free_page_report(isolated_pages_obj,
					       isolated_idx);
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

			mt = get_pageblock_migratetype(page);
			__free_one_page(page, page_to_pfn(page), zone,
					isolated_pages_obj[i].order, mt);
			i++;
		}
		spin_unlock_irqrestore(&zone->lock, flags);
	}
	kfree(isolated_pages_obj);
}

void guest_free_page_enqueue(struct page *page, int order)
{
	if (!static_branch_unlikely(&guest_free_page_hinting_key))
		return;
	if (PageBuddy(page) && order >= FREE_PAGE_HINTING_MIN_ORDER) {
		set_bitmap(page, page_zonenum(page));
	} else {
		struct page *buddy_page = get_buddy_page(page);

		if (buddy_page && page_private(buddy_page) >=
		    FREE_PAGE_HINTING_MIN_ORDER)
			set_bitmap(buddy_page, page_zonenum(page));
	}
}

void init_hinting_wq(struct work_struct *work)
{
	int zonenum = 0;

	/* Don't like checking the meomry threshold twice.*/
	while (zonenum < 3) {
		if (atomic_read(&bm_zone[zonenum].free_mem_cnt) >=
		    HINTING_THRESHOLD) {
			atomic_sub(HINTING_THRESHOLD,
				   &bm_zone[zonenum].free_mem_cnt);
			guest_free_page_hinting(zonenum);
		}
		zonenum++;
	}
}

void guest_free_page_try_hinting(void)
{
	if (!static_branch_unlikely(&guest_free_page_hinting_key))
		return;
	if (atomic_read(&bm_zone[0].free_mem_cnt) >= HINTING_THRESHOLD ||
	    atomic_read(&bm_zone[1].free_mem_cnt) >= HINTING_THRESHOLD ||
	    atomic_read(&bm_zone[2].free_mem_cnt) >= HINTING_THRESHOLD) {
		/*
		 * Scheduling work on a different CPU (not the CPU which
		 * generated the request) could degrade performance?
		 * int cpu_id = smp_processor_id();
		 * queue_work_on(cpu_id, system_wq, &hinting_work);
		 */
		queue_work(system_wq, &hinting_work);
	}
}
