#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/page_ref.h>
#include <linux/kvm_host.h>
#include <linux/sort.h>
#include <linux/kernel.h>
#include <trace/events/kmem.h>

/*
 * struct kvm_free_pages - Tracks the pages which are freed by the guest.
 * @pfn: page frame number for the page which is freed.
 * @order: order corresponding to the page freed.
 * @zonenum: zone number to which the freed page belongs.
 */
struct kvm_free_pages {
	unsigned long pfn;
	unsigned int order;
	int zonenum;
};

/*
 * struct page_hinting - holds array objects for the structures used to track
 * guest free pages, along with an index variable for each of them.
 * @kvm_pt: array object for the structure kvm_free_pages.
 * @kvm_pt_idx: index for kvm_free_pages object.
 * @hyp_idx: index for hypervisor_pages object.
 */
struct page_hinting {
	struct kvm_free_pages kvm_pt[MAX_FGPT_ENTRIES];
	int kvm_pt_idx;
};
DEFINE_PER_CPU(struct page_hinting, hinting_obj);

struct static_key_false guest_page_hinting_key  = STATIC_KEY_FALSE_INIT;
EXPORT_SYMBOL(guest_page_hinting_key);
static DEFINE_MUTEX(hinting_mutex);
int guest_page_hinting_flag;
EXPORT_SYMBOL(guest_page_hinting_flag);

void (*request_hypercall)(void *, struct guest_request *);
EXPORT_SYMBOL(request_hypercall);
void *balloon_ptr;
EXPORT_SYMBOL(balloon_ptr);

unsigned long total_freed, captured, scanned, total_isolated, tail_isolated;
unsigned long failed_isolation, reallocated, free_non_buddy, guest_returned;
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
		       "tail_isolated_memory:%lu KB\nfailed_isolation_memory:%lu KB\n"
		       "reallocated_memory:%lu KB\nfree_non_buddy_memory:%lu KB\n"
		       "guest_returned_memory:%lu KB\n", total_freed, captured,
		       scanned, total_isolated, tail_isolated,
		       failed_isolation, reallocated,
		       free_non_buddy, guest_returned);
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

int guest_page_hinting_sysctl(struct ctl_table *table, int write,
			      void __user *buffer, size_t *lenp,
			      loff_t *ppos)
{
	int ret;

	mutex_lock(&hinting_mutex);
	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (guest_page_hinting_flag)
		static_key_enable(&guest_page_hinting_key.key);
	else
		static_key_disable(&guest_page_hinting_key.key);
	mutex_unlock(&hinting_mutex);
	return ret;
}

void release_buddy_pages(struct guest_request *guest_req)
{
	int i = 0;
	int mt = 0;
	unsigned long mem = 0;
	void *temp_addr = phys_to_virt(guest_req->addr);
	struct hypervisor_pages *guest_isolated_pages = temp_addr;
	int entries = guest_req->entries;

	while (i < entries) {
		struct page *page = pfn_to_page(guest_isolated_pages[i].pfn);

		mem = (1 << guest_isolated_pages[i].order) * 4;
		guest_returned += mem;
		mt = get_pageblock_migratetype(page);
		__free_one_page(page, page_to_pfn(page), page_zone(page),
			      guest_isolated_pages[i].order, mt);
		i++;
	}
	kfree(guest_isolated_pages);
	kfree(guest_req);
}

void hyperlist_ready(struct hypervisor_pages *guest_isolated_pages, int entries)
{

	struct guest_request *guest_req;

	guest_req = kmalloc(sizeof(struct guest_request), GFP_KERNEL);

	guest_req->addr = virt_to_phys((void *)&guest_isolated_pages[0]);
	guest_req->entries = entries;

	if (balloon_ptr)
		request_hypercall(balloon_ptr, guest_req);
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

static void arch_free_page_slowpath(void)
{
	struct page_hinting *page_hinting_obj = this_cpu_ptr(&hinting_obj);
	struct hypervisor_pages *free_pagelist;
	int idx = 0, ret = 0;
	struct zone *zone_cur;
	unsigned long flags = 0;
	int hyp_idx = 0;

	free_pagelist = kmalloc(MAX_FGPT_ENTRIES * sizeof(struct hypervisor_pages), GFP_KERNEL);
	if (!free_pagelist) {
		page_hinting_obj->kvm_pt_idx = 0;
		put_cpu_var(hinting_obj);
		return;
		/* return some logical error here*/
	}

	while (idx < MAX_FGPT_ENTRIES) {
		unsigned long pfn = page_hinting_obj->kvm_pt[idx].pfn;
		unsigned long pfn_end = page_hinting_obj->kvm_pt[idx].pfn +
			(1 << page_hinting_obj->kvm_pt[idx].order) - 1;

		while (pfn <= pfn_end) {
			struct page *page = pfn_to_page(pfn);
			struct page *buddy_page = NULL;

			zone_cur = page_zone(page);
			spin_lock_irqsave(&zone_cur->lock, flags);

			if (PageCompound(page)) {
				struct page *head_page = compound_head(page);
				unsigned long head_pfn = page_to_pfn(head_page);
				unsigned int alloc_pages =
					1 << compound_order(head_page);

				pfn = head_pfn + alloc_pages;
				reallocated += (alloc_pages * 4);
				scanned += (alloc_pages * 4);
				spin_unlock_irqrestore(&zone_cur->lock, flags);
				continue;
			}

			if (page_ref_count(page)) {
				pfn++;
				reallocated += (4);
				scanned += (4);
				spin_unlock_irqrestore(&zone_cur->lock, flags);
				continue;
			}

			if (PageBuddy(page) && page_private(page) >= (MAX_ORDER - 1)) {
				int buddy_order = page_private(page);

				ret = __isolate_free_page(page, buddy_order);
				if (!ret) {
					failed_isolation +=
						((1 << buddy_order) * 4);
				} else {
					trace_guest_isolated_page(pfn,
								 buddy_order);
					free_pagelist[hyp_idx].pfn = pfn;
					free_pagelist[hyp_idx].order = buddy_order;
					total_isolated +=
						(((1 << buddy_order) * 4));
					tail_isolated +=
						(((1 << buddy_order) * 4));
					hyp_idx += 1;
				}
				pfn = pfn + (1 << buddy_order);
				scanned += ((1 << buddy_order) * 4);
				spin_unlock_irqrestore(&zone_cur->lock, flags);
				continue;
			}

			buddy_page = get_buddy_page(page);
			if (buddy_page && page_private(buddy_page) >= (MAX_ORDER - 1)) {
				int buddy_order = page_private(buddy_page);

				ret = __isolate_free_page(buddy_page,
							  buddy_order);
				if (!ret) {
					failed_isolation +=
						((1 << buddy_order) * 4);
				} else {
					unsigned long buddy_pfn =
						page_to_pfn(buddy_page);
	
					trace_guest_isolated_page(pfn,
								 buddy_order);
					free_pagelist[hyp_idx].pfn = buddy_pfn;
					free_pagelist[hyp_idx].order = buddy_order;
					total_isolated +=
						(((1 << buddy_order) * 4));
					tail_isolated +=
						(((1 << buddy_order) * 4));
					hyp_idx += 1;
				}
				pfn = page_to_pfn(buddy_page) +
					(1 << buddy_order);
				scanned += ((1 << buddy_order) * 4);
				spin_unlock_irqrestore(&zone_cur->lock, flags);
				continue;
			}
			scanned += (4);
			free_non_buddy += (4);
			spin_unlock_irqrestore(&zone_cur->lock, flags);
			pfn++;
		}
		page_hinting_obj->kvm_pt[idx].pfn = 0;
		page_hinting_obj->kvm_pt[idx].order = -1;
		page_hinting_obj->kvm_pt[idx].zonenum = -1;
		idx++;
	}

	page_hinting_obj->kvm_pt_idx = 0;
	put_cpu_var(hinting_obj);

	if (hyp_idx > 0)
		hyperlist_ready(free_pagelist, hyp_idx);
	else 
		kfree(free_pagelist);
		/* return some logical error here*/
}

int if_exist(struct page *page)
{
	int i = 0;
	struct page_hinting *page_hinting_obj = this_cpu_ptr(&hinting_obj);

	while (i < MAX_FGPT_ENTRIES) {
		if (page_to_pfn(page) == page_hinting_obj->kvm_pt[i].pfn)
			return 1;
		i++;
	}
	return 0;
}

void guest_hinting_fn(struct page *page, int order)
{
	unsigned long flags;
	struct page_hinting *page_hinting_obj = this_cpu_ptr(&hinting_obj);
	int err = 0;
	struct page *buddy_page = get_buddy_page(page);
	/*
	 * use of global variables may trigger a race condition between irq and
	 * process context causing unwanted overwrites. This will be replaced
	 * with a better solution to prevent such race conditions.
	 */

	local_irq_save(flags);
	if (sys_init_cnt == 0) {
		err = sysfs_create_group(mm_kobj, &hinting_attr_group);
		if (err)
			pr_err("hinting: register sysfs failed\n");
		sys_init_cnt = 1;
	}

	trace_guest_free_page(page_to_pfn(page), order);
	total_freed += (((1 << order) * 4));
	if (page_hinting_obj->kvm_pt_idx != MAX_FGPT_ENTRIES) {
		disable_page_poisoning();
		if (PageBuddy(page) && page_private(page) >= (MAX_ORDER - 1)) {
			page_hinting_obj->kvm_pt[page_hinting_obj->kvm_pt_idx].pfn =
								page_to_pfn(page);
			page_hinting_obj->kvm_pt[page_hinting_obj->kvm_pt_idx].zonenum =
								page_zonenum(page);
			page_hinting_obj->kvm_pt[page_hinting_obj->kvm_pt_idx].order =
								order;
			trace_guest_captured_page(page_to_pfn(page), order, page_hinting_obj->kvm_pt_idx);
			page_hinting_obj->kvm_pt_idx += 1;
			captured += (((1 << order) * 4));
		}
		if (buddy_page && page_private(buddy_page) >= (MAX_ORDER - 1) && !if_exist(buddy_page)) {
			page_hinting_obj->kvm_pt[page_hinting_obj->kvm_pt_idx].pfn =
								page_to_pfn(buddy_page);
			page_hinting_obj->kvm_pt[page_hinting_obj->kvm_pt_idx].zonenum =
								page_zonenum(buddy_page);
			page_hinting_obj->kvm_pt[page_hinting_obj->kvm_pt_idx].order =
								page_private(buddy_page);
			trace_guest_captured_page(page_to_pfn(buddy_page), page_private(buddy_page), page_hinting_obj->kvm_pt_idx);
			page_hinting_obj->kvm_pt_idx += 1;
			captured += (((1 << order) * 4));
		}
		if (page_hinting_obj->kvm_pt_idx == MAX_FGPT_ENTRIES) {
			struct zone *zone = page_zone(page);
			int was_unlocked = 0;

			if (spin_is_locked(&zone->lock)) {
				spin_unlock(&zone->lock);
				was_unlocked = 1;
			}
			arch_free_page_slowpath();
			if (was_unlocked)
				spin_lock(&zone->lock);
		}
	}
	local_irq_restore(flags);
}
