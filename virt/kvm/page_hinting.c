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
DEFINE_PER_CPU(int, hyper_idx);
void (*request_hypercall)(void *, u64, int);
EXPORT_SYMBOL(request_hypercall);
void *balloon_ptr;
EXPORT_SYMBOL(balloon_ptr);
struct static_key_false guest_page_hinting_key  = STATIC_KEY_FALSE_INIT;
EXPORT_SYMBOL(guest_page_hinting_key);
static DEFINE_MUTEX(hinting_mutex);
int guest_page_hinting_flag;
EXPORT_SYMBOL(guest_page_hinting_flag);
unsigned int isolated_page_counter, failed_isolation_counter;
EXPORT_SYMBOL(isolated_page_counter);
EXPORT_SYMBOL(failed_isolation_counter);
static DEFINE_PER_CPU(struct task_struct *, hinting_task);

int count_isolated_pages(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp,
			 loff_t *ppos)
{
	int ret;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	return ret;
}

int count_failed_isolations(struct ctl_table *table, int write,
			    void __user *buffer, size_t *lenp,
			    loff_t *ppos)
{
	int ret;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	return ret;
}

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

void hyperlist_ready(struct hypervisor_pages *guest_isolated_pages, int entries)
{
	int i = 0;

	trace_guest_str_dump("Hypercall to host...:");
	while (i < entries) {
		struct page *p = pfn_to_page(guest_isolated_pages[i].pfn);

		init_page_count(p);
		__free_pages(p, page_private(p));
		i++;
	}
}

static void hinting_fn(unsigned int cpu)
{
	int idx = 0, ret = 0;
	int *kvm_idx = &get_cpu_var(kvm_pt_idx);
	struct kvm_free_pages *free_page_obj = &get_cpu_var(kvm_pt)[0];
	struct hypervisor_pages *guest_isolated_pages =
					&get_cpu_var(hypervisor_pagelist)[0];
	int *hyp_idx = &get_cpu_var(hyper_idx);
	struct zone *zone_cur;
	unsigned long flags = 0;
	int unlocked = 0;

	while (idx < MAX_FGPT_ENTRIES) {
		unsigned long pfn = free_page_obj[idx].pfn;
		struct page *p = pfn_to_page(pfn);

		zone_cur = page_zone(p);
		spin_lock_irqsave(&zone_cur->lock, flags);
		unlocked = 0;

		if (PageBuddy(p)) {
			if (page_private(p) == free_page_obj[idx].order) {
				ret = __isolate_free_page(p, page_private(p));
				if (!ret) {
					failed_isolation_counter++;
				} else {
					isolated_page_counter++;
					guest_isolated_pages[*hyp_idx].pfn =
							pfn;
					guest_isolated_pages[*hyp_idx].pages =
							1 << page_private(p);
					*hyp_idx += 1;
					if (*hyp_idx == MAX_FGPT_ENTRIES ||
					    page_private(p) > MAX_ORDER) {
						spin_unlock_irqrestore
							(&zone_cur->lock,
							 flags);
						unlocked = 1;
						hyperlist_ready
							(guest_isolated_pages,
							 *hyp_idx);
						*hyp_idx = 0;
					}
				}
			}
		}
		if (!unlocked)
			spin_unlock_irqrestore(&zone_cur->lock, flags);
		free_page_obj[idx].pfn = 0;
		free_page_obj[idx].order = -1;
		free_page_obj[idx].zonenum = -1;
		idx++;
	}

	*kvm_idx = 0;
	put_cpu_var(hypervisor_pagelist);
	put_cpu_var(kvm_pt);
	put_cpu_var(hyper_idx);
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
	/*
	 * use of global variables may trigger a race condition between irq and
	 * process context causing unwanted overwrites. This will be replaced
	 * with a better solution to prevent such race conditions.
	 */

	local_irq_save(flags);
	free_page_idx = this_cpu_ptr(&kvm_pt_idx);
	free_page_obj = this_cpu_ptr(kvm_pt);

	if (*free_page_idx != MAX_FGPT_ENTRIES) {
		disable_page_poisoning();
		trace_guest_free_page(page, order);
		free_page_obj[*free_page_idx].pfn = page_to_pfn(page);
		free_page_obj[*free_page_idx].zonenum = page_zonenum(page);
		free_page_obj[*free_page_idx].order = order;
		*free_page_idx += 1;
		if (*free_page_idx == MAX_FGPT_ENTRIES)
			wake_up_process(__this_cpu_read(hinting_task));
	}
	local_irq_restore(flags);
}
