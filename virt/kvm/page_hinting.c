#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/page_ref.h>
#include <linux/kvm_host.h>
#include <linux/sort.h>
#include <linux/kernel.h>
#include <trace/events/kmem.h>

#define HYPERLIST_THRESHOLD	500
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
	unsigned int pages;
};

static __cacheline_aligned_in_smp DEFINE_SEQLOCK(guest_page_lock);
DEFINE_PER_CPU(struct kvm_free_pages [MAX_FGPT_ENTRIES], kvm_pt);
DEFINE_PER_CPU(int, kvm_pt_idx);
struct hypervisor_pages hypervisor_pagelist[MAX_FGPT_ENTRIES];
EXPORT_SYMBOL(hypervisor_pagelist);
void (*request_hypercall)(void *, int);
EXPORT_SYMBOL(request_hypercall);
void *balloon_ptr;
EXPORT_SYMBOL(balloon_ptr);
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

static void empty_hyperlist(void)
{
	int i = 0;

	while (i < MAX_FGPT_ENTRIES) {
		hypervisor_pagelist[i].pfn = 0;
		hypervisor_pagelist[i].pages = 0;
		i++;
	}
}

void hyperlist_ready(int entries)
{
	trace_guest_str_dump("Hypercall to host...:");
	request_hypercall(balloon_ptr, entries);
	empty_hyperlist();
}

static int sort_pfn(const void *a1, const void *b1)
{
	const struct hypervisor_pages *a = a1;
	const struct hypervisor_pages *b = b1;

	if (a->pfn > b->pfn)
		return 1;

	if (a->pfn < b->pfn)
		return -1;

	return 0;
}

int pack_hyperlist(void)
{
	int i = 0, j = 0;

	while (i < MAX_FGPT_ENTRIES) {
		if (hypervisor_pagelist[i].pfn != 0) {
			if (i != j) {
				trace_guest_pfn_dump("Packing Hyperlist",
						     hypervisor_pagelist[i].pfn,
						hypervisor_pagelist[i].pages);
				hypervisor_pagelist[j].pfn =
						hypervisor_pagelist[i].pfn;
				hypervisor_pagelist[j].pages =
						hypervisor_pagelist[i].pages;
			}
			j++;
		}
		i++;
	}
	i = j;
	while (j < MAX_FGPT_ENTRIES) {
		hypervisor_pagelist[j].pfn = 0;
		hypervisor_pagelist[j].pages = 0;
		j++;
	}
	return i;
}

int compress_hyperlist(void)
{
	int i = 0, j = 1, merge_counter = 0, ret = 0;

	sort(hypervisor_pagelist, MAX_FGPT_ENTRIES,
	     sizeof(struct hypervisor_pages), sort_pfn, NULL);
	while (i < MAX_FGPT_ENTRIES && j < MAX_FGPT_ENTRIES) {
		unsigned long pfni = hypervisor_pagelist[i].pfn;
		unsigned int pagesi = hypervisor_pagelist[i].pages;
		unsigned long pfnj = hypervisor_pagelist[j].pfn;
		unsigned int pagesj = hypervisor_pagelist[j].pages;

		if (pfnj <= pfni) {
			if (((pfnj + pagesj - 1) <= (pfni + pagesi - 1)) &&
			    ((pfnj + pagesj - 1) >= (pfni - 1))) {
				hypervisor_pagelist[i].pfn = pfnj;
				hypervisor_pagelist[i].pages += pfni - pfnj;
				hypervisor_pagelist[j].pfn = 0;
				hypervisor_pagelist[j].pages = 0;
				j++;
				merge_counter++;
				continue;
			} else if ((pfnj + pagesj - 1) > (pfni + pagesi - 1)) {
				hypervisor_pagelist[i].pfn = pfnj;
				hypervisor_pagelist[i].pages = pagesj;
				hypervisor_pagelist[j].pfn = 0;
				hypervisor_pagelist[j].pages = 0;
				j++;
				merge_counter++;
				continue;
			}
		} else if (pfnj > pfni) {
			if ((pfnj + pagesj - 1) > (pfni + pagesi - 1) &&
			    (pfnj <= pfni + pagesi)) {
				hypervisor_pagelist[i].pages +=
						(pfnj + pagesj - 1) -
						(pfni + pagesi - 1);
				hypervisor_pagelist[j].pfn = 0;
				hypervisor_pagelist[j].pages = 0;
				j++;
				merge_counter++;
				continue;
			} else if ((pfnj + pagesj - 1) <= (pfni + pagesi - 1)) {
				hypervisor_pagelist[j].pfn = 0;
				hypervisor_pagelist[j].pages = 0;
				j++;
				merge_counter++;
				continue;
			}
		}
		i = j;
		j++;
	}
	if (merge_counter != 0)
		ret = pack_hyperlist() - 1;
	else
		ret = MAX_FGPT_ENTRIES;
	return ret;
}

void copy_hyperlist(int hyper_idx)
{
	int *idx = &get_cpu_var(kvm_pt_idx);
	struct kvm_free_pages *free_page_obj;
	int i = 0;

	free_page_obj = &get_cpu_var(kvm_pt)[0];
	while (i < hyper_idx) {
		trace_guest_pfn_dump("HyperList entry copied",
				     hypervisor_pagelist[i].pfn,
				     hypervisor_pagelist[i].pages);
		free_page_obj[*idx].pfn = hypervisor_pagelist[i].pfn;
		free_page_obj[*idx].pages = hypervisor_pagelist[i].pages;
		*idx += 1;
		i++;
	}
	empty_hyperlist();
	put_cpu_var(kvm_pt);
	put_cpu_var(kvm_pt_idx);
}

/*
 * arch_free_page_slowpath() - This function adds the guest free page entries
 * to hypervisor_pages list and also ensures defragmentation prior to addition
 * if it is present with any entry of the kvm_free_pages list.
 */
void arch_free_page_slowpath(void)
{
	int idx = 0;
	int hyper_idx = -1;
	int *kvm_idx = &get_cpu_var(kvm_pt_idx);
	struct kvm_free_pages *free_page_obj = &get_cpu_var(kvm_pt)[0];

	write_seqlock(&guest_page_lock);
	while (idx < MAX_FGPT_ENTRIES) {
		unsigned long pfn = free_page_obj[idx].pfn;
		unsigned long pfn_end = free_page_obj[idx].pfn +
					free_page_obj[idx].pages - 1;
		bool prev_free = false;

		while (pfn <= pfn_end) {
			struct page *p = pfn_to_page(pfn);

			if (PageCompound(p)) {
				struct page *head_page = compound_head(p);
				unsigned long head_pfn = page_to_pfn(head_page);
				unsigned int alloc_pages =
					1 << compound_order(head_page);

				pfn = head_pfn + alloc_pages;
				prev_free = false;
				trace_guest_pfn_dump("Compound",
						     head_pfn, alloc_pages);
				continue;
			}
			if (page_ref_count(p)) {
				pfn++;
				prev_free = false;
				trace_guest_pfn_dump("Single", pfn, 1);
				continue;
			}
			/*
			 * The page is free so add it to the list and free the
			 * hypervisor_pagelist if required.
			 */
			if (!prev_free) {
				hyper_idx++;
				trace_guest_free_page_slowpath(
				hypervisor_pagelist[hyper_idx].pfn,
				hypervisor_pagelist[hyper_idx].pages);
				hypervisor_pagelist[hyper_idx].pfn = pfn;
				hypervisor_pagelist[hyper_idx].pages = 1;
				if (hyper_idx == MAX_FGPT_ENTRIES - 1) {
					hyper_idx =  compress_hyperlist();
					if (hyper_idx >=
					    HYPERLIST_THRESHOLD) {
						hyperlist_ready(hyper_idx);
						hyper_idx = 0;
					}
				}
				/*
				 * If the next contiguous page is free, it can
				 * be added to this same entry.
				 */
				prev_free = true;
			} else {
				/*
				 * Multiple adjacent free pages
				 */
				hypervisor_pagelist[hyper_idx].pages++;
			}
			pfn++;
		}
		free_page_obj[idx].pfn = 0;
		free_page_obj[idx].pages = 0;
		idx++;
	}
	*kvm_idx = 0;
	put_cpu_var(kvm_pt);
	put_cpu_var(kvm_pt_idx);
	write_sequnlock(&guest_page_lock);
}

void guest_alloc_page(struct page *page, int order)
{
	unsigned int seq;

	/*
	 * arch_free_page will acquire the lock once the list carrying guest
	 * free pages is full and a hypercall will be made. Until complete free
	 * page list is traversed no further allocaiton will be allowed.
	 */

	do {
		seq = read_seqbegin(&guest_page_lock);
	} while (read_seqretry(&guest_page_lock, seq));
	trace_guest_alloc_page(page, order);
}

void guest_free_page(struct page *page, int order)
{
	int *free_page_idx = &get_cpu_var(kvm_pt_idx);
	struct kvm_free_pages *free_page_obj;
	unsigned long flags;
	/*
	 * use of global variables may trigger a race condition between irq and
	 * process context causing unwanted overwrites. This will be replaced
	 * with a better solution to prevent such race conditions.
	 */
	local_irq_save(flags);
	free_page_obj = &get_cpu_var(kvm_pt)[0];
	trace_guest_free_page(page, order);
	free_page_obj[*free_page_idx].pfn = page_to_pfn(page);
	free_page_obj[*free_page_idx].pages = 1 << order;
	*free_page_idx += 1;
	if (*free_page_idx == MAX_FGPT_ENTRIES)
		arch_free_page_slowpath();
	put_cpu_var(kvm_pt);
	put_cpu_var(kvm_pt_idx);
	local_irq_restore(flags);
}
