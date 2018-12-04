#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/page_ref.h>
#include <linux/kvm_host.h>
#include <linux/sort.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <trace/events/kmem.h>
#include <asm/current.h>
#include <linux/kobject.h>

/*
 * struct kvm_free_pages - Tracks the pages which are freed by the guest.
 * @pfn	- page frame number for the page which is to be freed
 * @pages - number of pages which are supposed to be freed.
 * A global array object is used to hold the list of pfn and number of pages
 * which are freed by the guest. This list may also have fragmentated pages so
 * defragmentation is a must prior to the hypercall.
 */
struct kvm_free_pages {
	unsigned long pfn;
	unsigned int order;
	int zonenum;
};

struct page_hinting {
	struct kvm_free_pages kvm_pt[MAX_FGPT_ENTRIES];
	int kvm_pt_idx;
	struct hypervisor_pages hypervisor_pagelist[MAX_FGPT_ENTRIES];
	int hyp_idx;
};
DEFINE_PER_CPU(struct page_hinting, hinting_obj);

struct static_key_false guest_page_hinting_key  = STATIC_KEY_FALSE_INIT;
EXPORT_SYMBOL(guest_page_hinting_key);
static DEFINE_MUTEX(hinting_mutex);
int guest_page_hinting_flag;
EXPORT_SYMBOL(guest_page_hinting_flag);

unsigned long total_freed, captured, scanned, total_isolated, tail_isolated; 
unsigned long failed_isolation, reallocated, free_non_buddy, guest_returned;

static DEFINE_PER_CPU(struct task_struct *, hinting_task);

void (*request_hypercall)(void *, u64, int);
EXPORT_SYMBOL(request_hypercall);
void *balloon_ptr;
EXPORT_SYMBOL(balloon_ptr);

#ifdef CONFIG_SYSFS

#define HINTING_ATTR_RO(_name) \
        static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

static ssize_t hinting_memory_stats_show(struct kobject *kobj,
                                    struct kobj_attribute *attr, char *buf)
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

	trace_guest_str_dump("guest_page_hinting_sysctl:enable/disable");
	ret = proc_dointvec(table, write, buffer, lenp, ppos);

	if (guest_page_hinting_flag)
		static_key_enable(&guest_page_hinting_key.key);
	else
		static_key_disable(&guest_page_hinting_key.key);
	mutex_unlock(&hinting_mutex);
	return ret;
}

void hyperlist_ready(struct hypervisor_pages *guest_isolated_pages, int entries)
{
	int i = 0;
	unsigned long mem = 0;
	int mt = 0;

	request_hypercall(balloon_ptr, (u64)&guest_isolated_pages[0], entries);
	while (i < entries) {
		struct page *page = pfn_to_page(guest_isolated_pages[i].pfn);
		
		mem = (1 << guest_isolated_pages[i].order) * 4;
		guest_returned += mem;	
		mt = get_pageblock_migratetype(page);
		free_one_page(page_zone(page), page, page_to_pfn(page), guest_isolated_pages[i].order, mt);
		i++;
	}
}

struct page* get_buddy_page(struct page *page)
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

static void hinting_fn(unsigned int cpu)
{
	int idx = 0, ret = 0;
	struct page_hinting *page_hinting_obj = &get_cpu_var(hinting_obj);
	struct zone *zone_cur;
	unsigned long flags = 0;
   
	while (idx < MAX_FGPT_ENTRIES) {
		unsigned long pfn = page_hinting_obj->kvm_pt[idx].pfn;
		unsigned long pfn_end = page_hinting_obj->kvm_pt[idx].pfn + (1 << page_hinting_obj->kvm_pt[idx].order) - 1;

		while (pfn <= pfn_end) {
			struct page *page = pfn_to_page(pfn);
			struct page *buddy_page = NULL;
	
			zone_cur = page_zone(page);
			spin_lock_irqsave(&zone_cur->lock, flags);
			trace_guest_free_page_slowpath(pfn, page_hinting_obj->kvm_pt[idx].order);
	
			if (PageCompound(page)) {
				struct page *head_page = compound_head(page);
				unsigned long head_pfn = page_to_pfn(head_page);
				unsigned int alloc_pages =
					1 << compound_order(head_page);

				reallocated += (alloc_pages * 4); 
				scanned += (alloc_pages * 4); 
				pfn = head_pfn + alloc_pages;
				spin_unlock_irqrestore(&zone_cur->lock, flags);
				continue;
			}
		
			if (page_ref_count(page)) {
				reallocated += (4);
				scanned += (4);
				pfn++;
				spin_unlock_irqrestore(&zone_cur->lock, flags);
				continue;
			}
		
			if (PageBuddy(page)) {
				int or = page_private(page);
				ret = __isolate_free_page(page, page_private(page));
				if (!ret) {
					failed_isolation += ((1 << or) * 4); 
				} else {
					page_hinting_obj->hypervisor_pagelist[page_hinting_obj->hyp_idx].pfn =
							pfn;
					page_hinting_obj->hypervisor_pagelist[page_hinting_obj->hyp_idx].pages =
							1 << or;
					page_hinting_obj->hypervisor_pagelist[page_hinting_obj->hyp_idx].order = or;
					page_hinting_obj->hyp_idx += 1;
					total_isolated += (((1 << or) * 4) ); 
				}
				pfn = pfn + (1 << or);
				scanned += ((1 << or) * 4);
				spin_unlock_irqrestore(&zone_cur->lock, flags);
				continue;
			}
			
			buddy_page = get_buddy_page(page);
			if (buddy_page != NULL) {
				int or = page_private(buddy_page);
				ret = __isolate_free_page(buddy_page, page_private(buddy_page));
				if (!ret) {
					failed_isolation += (((1 << or) * 4) );
					} else {
					page_hinting_obj->hypervisor_pagelist[page_hinting_obj->hyp_idx].pfn =
						page_to_pfn(buddy_page);
					page_hinting_obj->hypervisor_pagelist[page_hinting_obj->hyp_idx].pages =
						1 << or;
					page_hinting_obj->hypervisor_pagelist[page_hinting_obj->hyp_idx].order = or;
					trace_guest_isolated_pages(page_to_pfn(buddy_page), or);
					page_hinting_obj->hyp_idx += 1;
					total_isolated += (((1 << or) * 4) );
					tail_isolated += (((1 << or) * 4) );
				}
				pfn = page_to_pfn(buddy_page) + (1 << or);
				scanned += ((1 << or) * 4);
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
	if (page_hinting_obj->hyp_idx > 0) {
		hyperlist_ready(page_hinting_obj->hypervisor_pagelist, page_hinting_obj->hyp_idx);
		page_hinting_obj->hyp_idx = 0;
	}

	page_hinting_obj->kvm_pt_idx = 0;
	put_cpu_var(hinting_obj);
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


void pack_array(void)
{
	int i = 0, j = 0;
	struct page_hinting *page_hinting_obj = this_cpu_ptr(&hinting_obj);
	
	while (i < MAX_FGPT_ENTRIES) {
		if (page_hinting_obj->kvm_pt[i].pfn != 0) {
			if (i != j) {
				page_hinting_obj->kvm_pt[j].pfn =
						page_hinting_obj->kvm_pt[i].pfn;
				page_hinting_obj->kvm_pt[j].order =
						page_hinting_obj->kvm_pt[i].order;
				page_hinting_obj->kvm_pt[j].zonenum =
						page_hinting_obj->kvm_pt[i].zonenum;
			}
			j++;
		}
		i++;
	}
	i = j;
	page_hinting_obj->kvm_pt_idx = j;
	while (j < MAX_FGPT_ENTRIES) {
		page_hinting_obj->kvm_pt[j].pfn = 0;
		page_hinting_obj->kvm_pt[j].order = -1;
		page_hinting_obj->kvm_pt[j].zonenum = -1;
		j++;
	}
}

void scan_array(void)
{
	int i = 0;
	struct page_hinting *page_hinting_obj = this_cpu_ptr(&hinting_obj);
	while (i < MAX_FGPT_ENTRIES) {
		struct page *page = pfn_to_page(page_hinting_obj->kvm_pt[i].pfn);
		struct page *buddy_page = get_buddy_page(page);
		if(!PageBuddy(page) && buddy_page != NULL) {
			if (if_exist(buddy_page)) {
				page_hinting_obj->kvm_pt[i].pfn = 0;
				page_hinting_obj->kvm_pt[i].order = -1;
				page_hinting_obj->kvm_pt[i].zonenum = -1;
			}
			else {	
				page_hinting_obj->kvm_pt[i].pfn = page_to_pfn(buddy_page);
				page_hinting_obj->kvm_pt[i].order = page_private(buddy_page);
			}
		}

		i++;
	}
	pack_array();
	if (page_hinting_obj->kvm_pt_idx == MAX_FGPT_ENTRIES)
		wake_up_process(__this_cpu_read(hinting_task));
	return;
}

static int hinting_should_run(unsigned int cpu)
{
	struct page_hinting *page_hinting_obj = this_cpu_ptr(&hinting_obj);
	int free_page_idx = page_hinting_obj->kvm_pt_idx;

	if (free_page_idx == MAX_FGPT_ENTRIES)
		return 1;
	else
		return 0;
}

struct smp_hotplug_thread hinting_threads = {
	.store			= &hinting_task,
	.thread_should_run	= hinting_should_run,
	.thread_fn		= hinting_fn,
	.thread_comm		= "hinting/%u",
	.selfparking		= false,
};
EXPORT_SYMBOL(hinting_threads);

int sys_init_cnt;
void guest_free_page(struct page *page, int order)
{
	unsigned long flags;
	int err = 0;
	struct page_hinting *page_hinting_obj = this_cpu_ptr(&hinting_obj);
	/*
	 * use of global variables may trigger a race condition between irq and
	 * process context causing unwanted overwrites. This will be replaced
	 * with a better solution to prevent such race conditions.
	 */

	local_irq_save(flags);

	if(sys_init_cnt == 0) {
	        err = sysfs_create_group(mm_kobj, &hinting_attr_group);
        	if (err) {
                	pr_err("hinting: register sysfs failed\n");
        	}
		sys_init_cnt = 1;
	}
	total_freed += (((1 << order) * 4) ); 
	
	if (page_hinting_obj->kvm_pt_idx != MAX_FGPT_ENTRIES) {
		disable_page_poisoning();
		trace_guest_free_page(page, order);
		page_hinting_obj->kvm_pt[page_hinting_obj->kvm_pt_idx].pfn = page_to_pfn(page);
		page_hinting_obj->kvm_pt[page_hinting_obj->kvm_pt_idx].zonenum = page_zonenum(page);
		page_hinting_obj->kvm_pt[page_hinting_obj->kvm_pt_idx].order = order;
		page_hinting_obj->kvm_pt_idx += 1;
		captured += (((1 << order) * 4) ); 

		if (page_hinting_obj->kvm_pt_idx == MAX_FGPT_ENTRIES) {
			drain_local_pages(NULL);
			scan_array();
			//wake_up_process(__this_cpu_read(hinting_task));
		}
	}
	local_irq_restore(flags);
}
