#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/kvm_host.h>

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
struct static_key_false guest_page_hinting_key  = STATIC_KEY_FALSE_INIT;
EXPORT_SYMBOL(guest_page_hinting_key);
static DEFINE_MUTEX(hinting_mutex);
int guest_page_hinting_flag;
EXPORT_SYMBOL(guest_page_hinting_flag);

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

void guest_free_page(struct page *page, int order)
{
}
