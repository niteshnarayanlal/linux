#define MAX_FGPT_ENTRIES	1000
/*
 * hypervisor_pages - It is a dummy structure passed with the hypercall.
 * @pfn - page frame number for the page which is to be freed.
 * @pages - number of pages which are supposed to be freed.
 * A global array object is used to to hold the list of pfn and pages and is
 * passed as part of the hypercall.
 */
struct hypervisor_pages {
	unsigned long pfn;
	unsigned int pages;
};

extern struct hypervisor_pages hypervisor_pagelist[MAX_FGPT_ENTRIES];
void guest_free_page(struct page *page, int order);
