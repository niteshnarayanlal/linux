#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/kvm_host.h>
#include <linux/kernel.h>

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
 * @hypervisor_pagelist: array object for the structure hypervisor_pages.
 * @hyp_idx: index for hypervisor_pages object.
 */
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
static DEFINE_PER_CPU(struct task_struct *, hinting_task);

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

static void hinting_fn(unsigned int cpu)
{
	struct page_hinting *page_hinting_obj = this_cpu_ptr(&hinting_obj);

	page_hinting_obj->kvm_pt_idx = 0;
	put_cpu_var(hinting_obj);
}

void scan_array(void)
{
	struct page_hinting *page_hinting_obj = this_cpu_ptr(&hinting_obj);

	if (page_hinting_obj->kvm_pt_idx == MAX_FGPT_ENTRIES)
		wake_up_process(__this_cpu_read(hinting_task));
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

void guest_free_page(struct page *page, int order)
{
	unsigned long flags;
	struct page_hinting *page_hinting_obj = this_cpu_ptr(&hinting_obj);
	/*
	 * use of global variables may trigger a race condition between irq and
	 * process context causing unwanted overwrites. This will be replaced
	 * with a better solution to prevent such race conditions.
	 */

	local_irq_save(flags);
	if (page_hinting_obj->kvm_pt_idx != MAX_FGPT_ENTRIES) {
		disable_page_poisoning();
		page_hinting_obj->kvm_pt[page_hinting_obj->kvm_pt_idx].pfn =
							page_to_pfn(page);
		page_hinting_obj->kvm_pt[page_hinting_obj->kvm_pt_idx].zonenum =
							page_zonenum(page);
		page_hinting_obj->kvm_pt[page_hinting_obj->kvm_pt_idx].order =
							order;
		page_hinting_obj->kvm_pt_idx += 1;
		if (page_hinting_obj->kvm_pt_idx == MAX_FGPT_ENTRIES)
			scan_array();
	}
	local_irq_restore(flags);
}
