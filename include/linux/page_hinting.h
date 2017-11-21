#include <linux/poison.h>
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
extern void (*request_hypercall)(void *, int);
extern void *balloon_ptr;
extern bool want_page_poisoning;

extern struct static_key_false guest_page_hinting_key;
int guest_page_hinting_sysctl(struct ctl_table *table, int write,
			      void __user *buffer, size_t *lenp, loff_t *ppos);
extern int guest_page_hinting_flag;
void guest_alloc_page(struct page *page, int order);
void guest_free_page(struct page *page, int order);

static inline void disable_page_poisoning(void)
{
#ifdef CONFIG_PAGE_POISONING
	want_page_poisoning = 0;
#endif
}
