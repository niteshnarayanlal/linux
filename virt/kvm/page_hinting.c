#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/page_ref.h>
#include <linux/kvm_host.h>
#include <linux/sort.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <trace/events/kmem.h>
#include <linux/internal.h>

#define HYPERLIST_THRESHOLD	1	/* FIXME: find a good threshold */
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
	int zonenum;
};

DEFINE_PER_CPU(struct kvm_free_pages [MAX_FGPT_ENTRIES], kvm_pt);
DEFINE_PER_CPU(int, kvm_pt_idx);
DEFINE_PER_CPU(int, counter);
struct hypervisor_pages hypervisor_pagelist[MAX_FGPT_ENTRIES];
EXPORT_SYMBOL(hypervisor_pagelist);
void (*request_hypercall)(void *, int);
EXPORT_SYMBOL(request_hypercall);
void *balloon_ptr;
EXPORT_SYMBOL(balloon_ptr);
struct static_key_false guest_page_hinting_key  = STATIC_KEY_FALSE_INIT;
EXPORT_SYMBOL(guest_page_hinting_key);
static DEFINE_MUTEX(hinting_mutex);
static DEFINE_MUTEX(irq_mutex);
int guest_page_hinting_flag;
EXPORT_SYMBOL(guest_page_hinting_flag);
struct work_struct hinting_work;
EXPORT_SYMBOL(hinting_work);

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

	while (i < MAX_FGPT_ENTRIES - 1) {
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

static int sort_zonenum(const void *a1, const void *b1)
{
	const struct kvm_free_pages *a = a1;
	const struct kvm_free_pages *b = b1;

        if (a->zonenum > b->zonenum)
                return 1;

        if (a->zonenum < b->zonenum)
                return -1;

        return 0;
}

/*
 * arch_free_page_slowpath() - This function adds the guest free page entries
 * to hypervisor_pages list and also ensures defragmentation prior to addition
 * if it is present with any entry of the kvm_free_pages list.
 */
void arch_free_page_slowpath(void)
{
	int idx = 0;
	int *kvm_idx = &get_cpu_var(kvm_pt_idx);
	int *cnt = &get_cpu_var(counter);
	int zonenum_prev = -1, zonenum_cur = -1;
	struct kvm_free_pages *free_page_obj = &get_cpu_var(kvm_pt)[0];
	int hyper_idx = -1;
	int ret = 0;
	int i = 0;

//	printk("\nBefore operation list\n");
	while (i < MAX_FGPT_ENTRIES) {
		if (free_page_obj[i].pfn == 0) {
			printk("\n!!!!!!!!!!!!!!!!!!!!!!!!!!WARNING!!!!!!!!!!!!!!\n");
			printk("\n%d:%s counter:%d idx:%d pfn:%lu pages:%d cpu:%d\n", __LINE__, __func__, *cnt, i, free_page_obj[i].pfn, free_page_obj[idx].pages, smp_processor_id());
		}
		i++;
	}
	sort(free_page_obj, MAX_FGPT_ENTRIES,
	     sizeof(struct kvm_free_pages), sort_zonenum, NULL);
#if 0
	i = 0;
	printk("\nAfter sort operation list\n");
	while (i < MAX_FGPT_ENTRIES) {
		if (free_page_obj[i].pfn == 0) {
			printk("\n!!!!!!!!!!!!!!!!!!!!!!!!!!WARNING!!!!!!!!!!!!!!\n");
			printk("\nidx:%d pfn:%lu pages:%d\n", i, free_page_obj[i].pfn, free_page_obj[idx].pages);
		}
		i++;
	}
#endif
	while (idx < MAX_FGPT_ENTRIES) {
		unsigned long pfn = free_page_obj[idx].pfn;
		unsigned long pfn_end = free_page_obj[idx].pfn +
					free_page_obj[idx].pages - 1;
		bool prev_free = false;
		struct zone *zone_cur, *zone_prev;

		printk("\n%d:%s counter:%d idx:%d pfn:%lu pfn_end:%lu cpu:%d \n", __LINE__, __func__, *cnt, idx, pfn, pfn_end, smp_processor_id());
		if (zonenum_prev == -1) {
			zonenum_prev = free_page_obj[idx].zonenum;
		   	zone_prev = page_zone(pfn_to_page(pfn));
//			printk("\nAcquiring lock for the first entry which belongs to zone:%d\n", zonenum_prev);
			spin_lock(&zone_prev->lock);
		}
		zonenum_cur = free_page_obj[idx].zonenum;
		zone_cur = page_zone(pfn_to_page(pfn));
		if (zonenum_prev != zonenum_cur) {
			//unlock previous zone
//			printk("\nReleasing lock for :%d zone\n", zonenum_prev);
			spin_unlock(&zone_prev->lock);
			//change zone_next and zone value
			zonenum_prev = zonenum_cur;
			//lock current zone
//			printk("\nAcquiring lock for :%d zone\n", zonenum_cur);
			spin_lock(&zone_cur->lock);
		}
		while (pfn <= pfn_end && pfn!= 0) {
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
			if (PageBuddy(p)) {
					printk("pfn:%lu pfn from page:%lu", pfn, page_to_pfn(p));	
					ret = __isolate_free_page(p, page_order(p));
					if (!ret) {
						printk("\nidx:%d Isolation failure for pfn:%lu\n", idx, pfn);
					} else {
//						printk("\nidx:%d Isolation successful for pfn:%lu\n", idx, pfn);
						hyper_idx++;
						hypervisor_pagelist[hyper_idx].pfn = pfn;
						hypervisor_pagelist[hyper_idx].pages = page_order(p);
						hyperlist_ready(1);
					}
			//		trace_guest_free_page_slowpath(head_pfn,
					pfn = pfn + page_order(p);
					continue;
			}
			pfn++;
		}
		free_page_obj[idx].pfn = 0;
		free_page_obj[idx].pages = 0;
		idx++;
		if (idx == MAX_FGPT_ENTRIES) {
			//Release the last entries zone lock befoe leaving
//			printk("\nReleasing lock for the last entry\n");
			spin_unlock(&zone_cur->lock);
		}
	}

	*kvm_idx = 0;
	put_cpu_var(kvm_pt);
	put_cpu_var(kvm_pt_idx);
}

void guest_alloc_page(struct page *page, int order)
{
}

void init_hinting_wq(struct work_struct *work)
{
	int *free_page_idx = &get_cpu_var(kvm_pt_idx);
	int *cnt = &get_cpu_var(counter);
	
	printk("\n%d:%s counter:%d kvm_idx:%d cpu_id:%d\n", __LINE__, __func__, *cnt, *free_page_idx, smp_processor_id());
	arch_free_page_slowpath();
}

void guest_free_page(struct page *page, int order)
{
	unsigned long flags;
	/*
	 * use of global variables may trigger a race condition between irq and
	 * process context causing unwanted overwrites. This will be replaced
	 * with a better solution to prevent such race conditions.
	 */

	int *free_page_idx = &get_cpu_var(kvm_pt_idx);
	int *cnt = &get_cpu_var(counter);
	struct kvm_free_pages *free_page_obj = &get_cpu_var(kvm_pt)[0];
	int cpu_id = 0;

	if (*free_page_idx != MAX_FGPT_ENTRIES) {
		disable_page_poisoning();
		local_irq_save(flags);
		free_page_obj = &get_cpu_var(kvm_pt)[0];
		trace_guest_free_page(page, order);
		free_page_obj[*free_page_idx].pfn = page_to_pfn(page);
		free_page_obj[*free_page_idx].zonenum = page_zonenum(page);
		free_page_obj[*free_page_idx].pages = 1 << order;
		*free_page_idx += 1;
		if (*free_page_idx == MAX_FGPT_ENTRIES) {
			*cnt += 1;
			cpu_id = smp_processor_id();
			printk("\n%d:%s counter:%d cpu_id:%d", __LINE__, __func__, *cnt, cpu_id);
			queue_work_on(cpu_id, system_wq, &hinting_work);
			put_cpu_var(counter);
		}
		put_cpu_var(kvm_pt);
		put_cpu_var(kvm_pt_idx);
		local_irq_restore(flags);
	}
}
