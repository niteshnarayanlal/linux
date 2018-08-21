#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/kernel.h>

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

void guest_free_page(struct page *page, int order)
{
}
