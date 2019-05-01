#include <linux/mm.h>
#include <linux/page_hinting.h>
#include <linux/page_ref.h>
#include <linux/kvm_host.h>
#include <linux/kernel.h>
#include <linux/sort.h>
#include <trace/events/kmem.h>

/*
 * struct guest_isolated_pages- holds the buddy isolated pages which are
 * supposed to be freed by the host.
 * @pfn: page frame number for the isolated page.
 * @len: isolated memory address length.
 */
struct guest_isolated_pages {
	unsigned long pfn;
	unsigned long len;
};

struct hinting_bitmap bm_zone[3];
int (*request_hypercall)(void *balloon_ptr, void *hinting_req, int entries);
EXPORT_SYMBOL_GPL(request_hypercall);
void *balloon_ptr;
EXPORT_SYMBOL_GPL(balloon_ptr);

struct work_struct hinting_work;
EXPORT_SYMBOL(hinting_work);

struct static_key_false guest_free_page_hinting_key  = STATIC_KEY_FALSE_INIT;
EXPORT_SYMBOL_GPL(guest_free_page_hinting_key);
static DEFINE_MUTEX(hinting_mutex);
int guest_free_page_hinting_flag;
EXPORT_SYMBOL_GPL(guest_free_page_hinting_flag);

unsigned long total_freed, captured, scanned, total_isolated, failed_isolation, guest_returned, reported;
int sys_init_cnt;

#ifdef CONFIG_SYSFS
#define HINTING_ATTR_RO(_name) \
                static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

static ssize_t hinting_memory_stats_show(struct kobject *kobj,
                                         struct kobj_attribute *attr,
                                         char *buf)
{
        return sprintf(buf, "total_freed_memory:%lu KB\ncaptued_memory:%lu KB\n"
                       "scanned_memory:%lu KB\ntotal_isolated_memory:%lu KB\n"
                       "failed_isolation_memory:%lu KB \nreported_memory:%lu KB\n"
                       "guest_returned_memory:%lu KB\n", total_freed, captured,
                       scanned, total_isolated,
                       failed_isolation, reported,
                       guest_returned);
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

void release_buddy_pages(void *hinting_req, int entries)
{
	int i = 0;
	int mt = 0;
	struct guest_isolated_pages *isolated_pages_obj = hinting_req;
	unsigned long flags = 0;

	while (i < entries) {
		struct page *page = pfn_to_page(isolated_pages_obj[i].pfn);
		int zonenum = page_zonenum(page);
		struct zone *zone = page_zone(page);
		unsigned long bitmap_no = (page_to_pfn(page) - zone->zone_start_pfn) >> FREE_PAGE_HINTING_MIN_ORDER;

		spin_lock_irqsave(&zone->lock, flags);
		mt = get_pageblock_migratetype(page);
		__free_one_page(page, page_to_pfn(page), zone,
				FREE_PAGE_HINTING_MIN_ORDER, mt);
		guest_returned += (1 << FREE_PAGE_HINTING_MIN_ORDER) * 4;
		bitmap_clear(bm_zone[zonenum].bitmap, bitmap_no, 1);
		spin_unlock_irqrestore(&zone->lock, flags);
		i++;
	}
}
EXPORT_SYMBOL_GPL(release_buddy_pages);

void guest_free_page_report(struct guest_isolated_pages *isolated_pages_obj,
			    int entries)
{
	int err = 0;

	reported += (1 << FREE_PAGE_HINTING_MIN_ORDER) * entries * 4;
	if (balloon_ptr)
		err = request_hypercall(balloon_ptr, isolated_pages_obj,
					entries);
	else
		release_buddy_pages(isolated_pages_obj, entries);
}

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

void set_bitmap(struct page *page, int order, int zonenum)
{
	int bitmap_no = 0;
	struct zone *zone = page_zone(page);

	bitmap_no = (page_to_pfn(page) - zone->zone_start_pfn) >> FREE_PAGE_HINTING_MIN_ORDER;
	bm_zone[zonenum].zone = zone;
	bitmap_set(bm_zone[zonenum].bitmap, bitmap_no, 1);
	atomic_inc(&bm_zone[zonenum].free_mem_cnt);
	captured += (((1 << order) * 4));
}

static void guest_free_page_hinting(int zonenum)
{
	unsigned long flags = 0;
	unsigned long set_bit, start = 0;
	struct page *page;
	struct guest_isolated_pages *isolated_pages_obj;
	int hyp_idx = 0;
	int ret = 0;

	isolated_pages_obj = kmalloc(HINTING_THRESHOLD *
			sizeof(struct guest_isolated_pages), GFP_KERNEL);
	if (!isolated_pages_obj) {
		return;
		/* return some logical error here*/
	}

        for (;;) {
		set_bit = find_next_bit(bm_zone[zonenum].bitmap, bm_zone[zonenum].bm_size, start);
		if (set_bit >= bm_zone[zonenum].bm_size ){
			//|| hyp_idx == HINTING_THRESHOLD) {
			break;
		}
		scanned += (1 << FREE_PAGE_HINTING_MIN_ORDER) * 4; 

		spin_lock_irqsave(&bm_zone[zonenum].zone->lock, flags);
		page = pfn_to_page((set_bit << FREE_PAGE_HINTING_MIN_ORDER) + bm_zone[zonenum].zone->zone_start_pfn);
		if (PageBuddy(page) && page_private(page) >= FREE_PAGE_HINTING_MIN_ORDER) {
			int buddy_order = page_private(page);
			unsigned long pfn = page_to_pfn(page);

			ret = __isolate_free_page(page, buddy_order);
			if (ret) {
				trace_guest_isolated_page(pfn, buddy_order);
				isolated_pages_obj[hyp_idx].pfn = pfn;
				isolated_pages_obj[hyp_idx].len = (1 << buddy_order) * 4;
				total_isolated += (((1 << buddy_order) * 4));
				hyp_idx += 1;
			} else {
				failed_isolation += ((1 << buddy_order) * 4);
			}

		}
		spin_unlock_irqrestore(&bm_zone[zonenum].zone->lock, flags);
		if (hyp_idx >= HINTING_THRESHOLD) {
			guest_free_page_report(isolated_pages_obj, hyp_idx);
			hyp_idx = 0;
		}
		start = set_bit + 1;
	}
	if (hyp_idx > 0) {
		int i = 0;
		int mt = 0;

		spin_lock_irqsave(&bm_zone[zonenum].zone->lock, flags);
		while (i < hyp_idx) {
			struct page *page = pfn_to_page(isolated_pages_obj[i].pfn);

			mt = get_pageblock_migratetype(page);
			__free_one_page(page, page_to_pfn(page), page_zone(page),
			FREE_PAGE_HINTING_MIN_ORDER, mt);
			i++;
		}
		spin_unlock_irqrestore(&bm_zone[zonenum].zone->lock, flags);
	}
	kfree(isolated_pages_obj);
}

void guest_free_page_enqueue(struct page *page, int order)
{
	unsigned long flags;

	if (!static_branch_unlikely(&guest_free_page_hinting_key))
		return;
	/*
	 * use of global variables may trigger a race condition between irq and
	 * process context causing unwanted overwrites. This will be replaced
	 * with a better solution to prevent such race conditions.
	 */
	local_irq_save(flags);
	trace_guest_free_page(page_to_pfn(page), order);
	total_freed += (((1 << order) * 4));
	if (PageBuddy(page) && page_private(page) >=
	    FREE_PAGE_HINTING_MIN_ORDER) {
		set_bitmap(page, order, page_zonenum(page));
	} else {
		struct page *buddy_page = get_buddy_page(page);

		if (buddy_page && page_private(buddy_page) >=
		    FREE_PAGE_HINTING_MIN_ORDER) {
			unsigned int buddy_order =
				page_private(buddy_page);

			set_bitmap(buddy_page, buddy_order, page_zonenum(page));
		}
	}
	local_irq_restore(flags);
}

void init_hinting_wq(struct work_struct *work)
{
	int idx = 0;
	while (idx < 3) {
		if (atomic_read(&bm_zone[idx].free_mem_cnt) >= HINTING_THRESHOLD) {
			atomic_sub(HINTING_THRESHOLD, &bm_zone[idx].free_mem_cnt);
			guest_free_page_hinting(idx);
		}
		idx++;
	}
}

void guest_free_page_try_hinting(void)
{
	int err;

	if (!static_branch_unlikely(&guest_free_page_hinting_key))
		return;
        if (sys_init_cnt == 0) {
                err = sysfs_create_group(mm_kobj, &hinting_attr_group);
                if (err)
                        pr_err("hinting: register sysfs failed\n");
                sys_init_cnt = 1;
        }
	if (atomic_read(&bm_zone[0].free_mem_cnt) >= HINTING_THRESHOLD ||
	    atomic_read(&bm_zone[1].free_mem_cnt) >= HINTING_THRESHOLD ||
	    atomic_read(&bm_zone[2].free_mem_cnt) >= HINTING_THRESHOLD ) {
//		int cpu_id = smp_processor_id();

		/*
		 * Scheduling work on a different CPU (not the CPU which generated the
		 * request) could degrade performance???????
		 */
		queue_work(system_wq, &hinting_work);
	}
}
