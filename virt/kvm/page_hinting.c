#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/page_ref.h>
#include <linux/kvm_host.h>
#include <linux/sort.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <trace/events/kmem.h>

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
unsigned long total_isolated_memory, failed_isolation_memory, tail_isolated_memory;
unsigned long captured_freed_memory, reallocated_memory, free_non_buddy_memory, buddy_unisolated_memory, total_freed_memory;
EXPORT_SYMBOL(captured_freed_memory);
EXPORT_SYMBOL(total_freed_memory);
EXPORT_SYMBOL(reallocated_memory);
EXPORT_SYMBOL(free_non_buddy_memory);
EXPORT_SYMBOL(buddy_unisolated_memory);
EXPORT_SYMBOL(total_isolated_memory);
EXPORT_SYMBOL(tail_isolated_memory);
EXPORT_SYMBOL(failed_isolation_memory);
static DEFINE_PER_CPU(struct task_struct *, hinting_task);


int count_total_freed_memory(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp,
			 loff_t *ppos)
{
	int ret;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	return ret;
}

int count_captured_freed_memory(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp,
			 loff_t *ppos)
{
	int ret;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	return ret;
}

int count_reallocated_memory(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp,
			 loff_t *ppos)
{
	int ret;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	return ret;
}

int count_free_non_buddy_memory(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp,
			 loff_t *ppos)
{
	int ret;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	return ret;
}

int count_buddy_unisolated_memory(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp,
			 loff_t *ppos)
{
	int ret;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	return ret;
}

int count_tail_isolated_memory(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp,
			 loff_t *ppos)
{
	int ret;

	trace_guest_str_dump("count_isolated_pages");
	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	return ret;
}
int count_total_isolated_memory(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp,
			 loff_t *ppos)
{
	int ret;

	trace_guest_str_dump("count_isolated_pages");
	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	return ret;
}

int count_failed_isolations_memory(struct ctl_table *table, int write,
			    void __user *buffer, size_t *lenp,
			    loff_t *ppos)
{
	int ret;

	trace_guest_str_dump("count_failed_isolations");
	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	return ret;
}

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

	trace_guest_str_dump("hyperlist_ready:Hypercall to host...");
	request_hypercall(balloon_ptr, (u64)&guest_isolated_pages[0], entries);
	while (i < entries) {
		struct page *p = pfn_to_page(guest_isolated_pages[i].pfn);

		init_page_count(p);
		__free_pages(p, page_private(p));
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
	unsigned int mem = 0;

	trace_guest_str_dump("hinting_fn:Scan per cpu, isolate and report");
	while (idx < MAX_FGPT_ENTRIES) {
		unsigned long pfn = free_page_obj[idx].pfn;
		struct page *p = pfn_to_page(pfn);

		zone_cur = page_zone(p);
		spin_lock_irqsave(&zone_cur->lock, flags);
		trace_guest_free_page_slowpath(pfn, free_page_obj[idx].order);
		if (PageBuddy(p)) {
			ret = __isolate_free_page(p, page_private(p));
			if (!ret) {
				mem = ((1 << page_private(p)) * 4);
				failed_isolation_memory += mem; 
			} else {
				mem = ((1 << page_private(p)) * 4);
				total_isolated_memory += mem; 
				guest_isolated_pages[hyp_idx].pfn =
						pfn;
				guest_isolated_pages[hyp_idx].pages =
						1 << page_private(p);
				trace_guest_isolated_pages(pfn, page_private(p));
				hyp_idx += 1;
			}
#if 0		
			} else {
				mem = ((1 << free_page_obj[idx].order) * 4);
				buddy_unisolated_memory += mem; 
			}
#endif
		}
		else if(page_private(pfn_to_page(pfn)) == 0) {
			struct page *buddy_page = get_buddy_page(pfn_to_page(pfn));
				if (buddy_page != NULL) {
					ret = __isolate_free_page(buddy_page, page_private(buddy_page));
					if (!ret) {
						mem = ((1 << page_private(buddy_page)) * 4);
						failed_isolation_memory += mem;
					} else {
						mem = ((1 << page_private(buddy_page)) * 4);

						total_isolated_memory += mem; 
						tail_isolated_memory += mem; 
						guest_isolated_pages[hyp_idx].pfn =
							page_to_pfn(buddy_page);
						guest_isolated_pages[hyp_idx].pages =
							1 << page_private(buddy_page);
						trace_guest_isolated_pages(page_to_pfn(buddy_page), page_private(buddy_page));
						hyp_idx += 1;
					}
				}
		}
	       	else {
			unsigned long pfn_end = pfn + (1 << free_page_obj[idx].order) - 1;
			while (pfn <= pfn_end) {
				struct page *p1 = pfn_to_page(pfn);
				mem = 0;

				if (PageCompound(p1)) {
					struct page *head_page = compound_head(p1);
					unsigned long head_pfn = page_to_pfn(head_page);
					unsigned int alloc_pages =
						1 << compound_order(head_page);

					mem = (alloc_pages * 4);
					reallocated_memory += mem; 
					pfn = head_pfn + alloc_pages;
					trace_guest_pfn_dump("Compound",
							     head_pfn, alloc_pages);
					continue;
				}
				if (page_ref_count(p1)) {
					reallocated_memory += 4;
					pfn++;
					trace_guest_pfn_dump("Single", pfn, 1);
					continue;
				}
				free_non_buddy_memory += 4;
				pfn++;
			}
		}
		spin_unlock_irqrestore(&zone_cur->lock, flags);
		free_page_obj[idx].pfn = 0;
		free_page_obj[idx].order = -1;
		free_page_obj[idx].zonenum = -1;
		idx++;
	}
	if (hyp_idx > 0)
		hyperlist_ready(guest_isolated_pages, hyp_idx);

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

void guest_free_page(struct page *page, int order)
{
	unsigned long flags;
	int *free_page_idx;
	struct kvm_free_pages *free_page_obj;
	unsigned int mem = 0;
	/*
	 * use of global variables may trigger a race condition between irq and
	 * process context causing unwanted overwrites. This will be replaced
	 * with a better solution to prevent such race conditions.
	 */

	local_irq_save(flags);
	free_page_idx = this_cpu_ptr(&kvm_pt_idx);
	free_page_obj = this_cpu_ptr(kvm_pt);
	
	mem = ((1 << order) * 4);
	total_freed_memory += mem; 
	mem = 0;

	if (*free_page_idx != MAX_FGPT_ENTRIES) {
		disable_page_poisoning();
		trace_guest_free_page(page, order);
		free_page_obj[*free_page_idx].pfn = page_to_pfn(page);
		free_page_obj[*free_page_idx].zonenum = page_zonenum(page);
		free_page_obj[*free_page_idx].order = order;
		*free_page_idx += 1;
		mem = ((1 << order) * 4);
		captured_freed_memory += mem; 
		if (*free_page_idx == MAX_FGPT_ENTRIES)
			wake_up_process(__this_cpu_read(hinting_task));
	}
	local_irq_restore(flags);
}
