#include <linux/gfp.h>
#include <linux/mm.h>
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

void guest_free_page(struct page *page, int order)
{
}
