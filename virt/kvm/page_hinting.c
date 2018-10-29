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

DEFINE_PER_CPU(struct kvm_free_pages [MAX_FGPT_ENTRIES], kvm_pt);
DEFINE_PER_CPU(int, kvm_pt_idx);
DEFINE_PER_CPU(struct hypervisor_pages [MAX_FGPT_ENTRIES], hypervisor_pagelist);
EXPORT_SYMBOL(hypervisor_pagelist);
void (*request_hypercall)(void *, u64, int);
EXPORT_SYMBOL(request_hypercall);
void *balloon_ptr;
EXPORT_SYMBOL(balloon_ptr);
struct static_key_false guest_page_hinting_key  = STATIC_KEY_FALSE_INIT;
EXPORT_SYMBOL(guest_page_hinting_key);
static DEFINE_MUTEX(hinting_mutex);
int guest_page_hinting_flag;
EXPORT_SYMBOL(guest_page_hinting_flag);
unsigned long total_freed, captured, scanned, total_isolated, tail_isolated, failed_isolation, reallocated, free_non_buddy, guest_returned;
static DEFINE_PER_CPU(struct task_struct *, hinting_task);
#define HINTING_ATTR_RO(_name) \
        static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

static ssize_t hinting_memory_stats_show(struct kobject *kobj,
                                    struct kobj_attribute *attr, char *buf)
{
        return sprintf(buf, "total_freed_memory:%lu\ncaptued_memory:%lu\n"
		       "scanned_memory:%lu\ntotal_isolated_memory:%lu\n"
		       "tail_isolated_memory:%lu\nfailed_isolation_memory:%lu\n"
		       "reallocated_memory:%lu\nfree_non_buddy_memory:%lu\n"
		       "guest_returned_memory:%lu\n", total_freed, captured,
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

	request_hypercall(balloon_ptr, (u64)&guest_isolated_pages[0], entries);
	while (i < entries) {
		struct page *p = pfn_to_page(guest_isolated_pages[i].pfn);
		
		mem = (1 << guest_isolated_pages[i].order) * 4;
		guest_returned += mem;	
		int mt = get_pageblock_migratetype(p);
        	__free_one_page(p, page_to_pfn(p), page_zone(p), guest_isolated_pages[i].order, mt);
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
	int *kvm_idx = &get_cpu_var(kvm_pt_idx);
	struct kvm_free_pages *free_page_obj = &get_cpu_var(kvm_pt)[0];
	struct hypervisor_pages *guest_isolated_pages =
					&get_cpu_var(hypervisor_pagelist)[0];
	int hyp_idx = 0;
	struct zone *zone_cur;
	unsigned long flags = 0;
   
	while (idx < MAX_FGPT_ENTRIES) {
		unsigned long pfn = free_page_obj[idx].pfn;
		unsigned long pfn_end = free_page_obj[idx].pfn + (1 << free_page_obj[idx].order) - 1;

		while (pfn <= pfn_end) {
			struct page *p = pfn_to_page(pfn);
	
			zone_cur = page_zone(p);
			spin_lock_irqsave(&zone_cur->lock, flags);
			trace_guest_free_page_slowpath(pfn, free_page_obj[idx].order);
	
			if (PageCompound(p)) {
				struct page *head_page = compound_head(p);
				unsigned long head_pfn = page_to_pfn(head_page);
				unsigned int alloc_pages =
					1 << compound_order(head_page);

				reallocated += alloc_pages * 4; 
				scanned += alloc_pages * 4; 
				pfn = head_pfn + alloc_pages;
				spin_unlock_irqrestore(&zone_cur->lock, flags);
				continue;
			}
		
			if (page_ref_count(p)) {
				reallocated += 4;
				scanned += 4;
				pfn++;
				spin_unlock_irqrestore(&zone_cur->lock, flags);
				continue;
			}
		
			if (PageBuddy(p)) {
				int or = page_private(p);
				ret = __isolate_free_page(p, page_private(p));
				if (!ret) {
					failed_isolation += ((1 << or) * 4); 
				} else {
					guest_isolated_pages[hyp_idx].pfn =
							pfn;
					guest_isolated_pages[hyp_idx].pages =
							1 << or;
					guest_isolated_pages[hyp_idx].order = or;
					hyp_idx += 1;
					total_isolated += ((1 << or) * 4); 
				}
				pfn = pfn + (1 << or);
				scanned += (1 << or) * 4;
				spin_unlock_irqrestore(&zone_cur->lock, flags);
				continue;
			}
			
			struct page *buddy_page = get_buddy_page(p);
			if (buddy_page != NULL) {
				int or = page_private(buddy_page);
				ret = __isolate_free_page(buddy_page, page_private(buddy_page));
				if (!ret) {
					failed_isolation += ((1 << or) * 4);
				} else {
					guest_isolated_pages[hyp_idx].pfn =
						page_to_pfn(buddy_page);
					guest_isolated_pages[hyp_idx].pages =
						1 << or;
					guest_isolated_pages[hyp_idx].order = or;
					trace_guest_isolated_pages(page_to_pfn(buddy_page), or);
					hyp_idx += 1;
					total_isolated += ((1 << or) * 4); 
					tail_isolated += ((1 << or) * 4); 
				}
				pfn = page_to_pfn(buddy_page) + (1 << or);
				scanned += (1 << or) * 4;
				spin_unlock_irqrestore(&zone_cur->lock, flags);
				continue;
			}
			scanned += 4;
			free_non_buddy += 4;
			spin_unlock_irqrestore(&zone_cur->lock, flags);
			pfn++;
		}
		free_page_obj[idx].pfn = 0;
		free_page_obj[idx].order = -1;
		free_page_obj[idx].zonenum = -1;
		idx++;
	}
	if (hyp_idx > 0) {
		hyperlist_ready(guest_isolated_pages, hyp_idx);
	}

	*kvm_idx = 0;
	put_cpu_var(hypervisor_pagelist);
	put_cpu_var(kvm_pt);
	put_cpu_var(kvm_pt_idx);
}

static int hinting_should_run(unsigned int cpu)
{
	int free_page_idx = per_cpu(kvm_pt_idx, cpu);

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
	int *free_page_idx;
	int err = 0;
	struct kvm_free_pages *free_page_obj;
	/*
	 * use of global variables may trigger a race condition between irq and
	 * process context causing unwanted overwrites. This will be replaced
	 * with a better solution to prevent such race conditions.
	 */

	local_irq_save(flags);
	free_page_idx = this_cpu_ptr(&kvm_pt_idx);
	free_page_obj = this_cpu_ptr(kvm_pt);

	if(sys_init_cnt == 0) {
	        err = sysfs_create_group(mm_kobj, &hinting_attr_group);
        	if (err) {
                	pr_err("hinting: register sysfs failed\n");
        	}
		sys_init_cnt = 1;
	}
	total_freed += ((1 << order) * 4); 
	
	if (*free_page_idx != MAX_FGPT_ENTRIES) {
		disable_page_poisoning();
		trace_guest_free_page(page, order);
		free_page_obj[*free_page_idx].pfn = page_to_pfn(page);
		free_page_obj[*free_page_idx].zonenum = page_zonenum(page);
		free_page_obj[*free_page_idx].order = order;
		*free_page_idx += 1;
		captured += ((1 << order) * 4); 

		if (*free_page_idx == MAX_FGPT_ENTRIES) {
			wake_up_process(__this_cpu_read(hinting_task));
		}
	}
	local_irq_restore(flags);
}
