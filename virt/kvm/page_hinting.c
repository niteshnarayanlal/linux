#include <linux/mm.h>
#include <linux/page_hinting.h>
#include <linux/page_ref.h>
#include <linux/kvm_host.h>
#include <linux/kernel.h>
#include <linux/sort.h>
#include <trace/events/kmem.h>

/*
 * struct guest_free_pages- holds array objects for the structures used to track
 * guest free pages, along with an index variable for each of them.
 * @free_pfn_arr: array to store the page frame number of all the pages which
 * are freed by the guest.
 * @guest_free_pages_idx: index to track the number entries stored in
 * free_pfn_arr.
 */
struct guest_free_pages {
	unsigned long free_page_arr[MAX_FGPT_ENTRIES];
	int free_pages_idx;
};

DEFINE_PER_CPU(struct guest_free_pages, free_pages_obj);

/*
 * struct guest_isolated_pages- holds the buddy isolated pages which are
 * supposed to be freed by the host.
 * @pfn: page frame number for the isolated page.
 * @len: isolated memory address length.
 */
struct guest_isolated_pages {
	unsigned long pfn;
	unsigned long len;
};

int (*request_hypercall)(void *balloon_ptr, void *hinting_req, int entries);
EXPORT_SYMBOL_GPL(request_hypercall);
void *balloon_ptr;
EXPORT_SYMBOL_GPL(balloon_ptr);

struct static_key_false guest_free_page_hinting_key  = STATIC_KEY_FALSE_INIT;
EXPORT_SYMBOL_GPL(guest_free_page_hinting_key);
static DEFINE_MUTEX(hinting_mutex);
int guest_free_page_hinting_flag;
EXPORT_SYMBOL_GPL(guest_free_page_hinting_flag);

int guest_free_page_hinting_sysctl(struct ctl_table *table, int write,
				   void __user *buffer, size_t *lenp,
				   loff_t *ppos)
{
	int ret;

	mutex_lock(&hinting_mutex);
	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (guest_free_page_hinting_flag)
		static_key_enable(&guest_free_page_hinting_key.key);
	else
		static_key_disable(&guest_free_page_hinting_key.key);
	mutex_unlock(&hinting_mutex);
	return ret;
}

void release_buddy_pages(void *hinting_req, int entries)
{
	int i = 0;
	int mt = 0;
	struct guest_isolated_pages *isolated_pages_obj = hinting_req;
	unsigned long flags = 0;

	while (i < entries) {
		struct page *page = pfn_to_page(isolated_pages_obj[i].pfn);
		struct zone *zone = page_zone(page);

		spin_lock_irqsave(&zone->lock, flags);
		mt = get_pageblock_migratetype(page);
		__free_one_page(page, page_to_pfn(page), zone,
				ilog2(isolated_pages_obj[i].len/4), mt);
		spin_unlock_irqrestore(&zone->lock, flags);
		i++;
	}
	kfree(isolated_pages_obj);
}
EXPORT_SYMBOL_GPL(release_buddy_pages);

void guest_free_page_report(struct guest_isolated_pages *isolated_pages_obj,
			    int entries)
{
	int err = 0;

	if (balloon_ptr)
		err = request_hypercall(balloon_ptr, isolated_pages_obj,
					entries);
}

static int sort_zonenum(const void *a1, const void *b1)
{
	const unsigned long *a = a1;
	const unsigned long *b = b1;

	if (page_zonenum(pfn_to_page(a[0])) > page_zonenum(pfn_to_page(b[0])))
		return 1;

	if (page_zonenum(pfn_to_page(a[0])) < page_zonenum(pfn_to_page(b[0])))
		return -1;

	return 0;
}

struct page *get_buddy_page(struct page *page)
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

static void guest_free_page_hinting(void)
{
	struct guest_free_pages *hinting_obj = &get_cpu_var(free_pages_obj);
	struct guest_isolated_pages *isolated_pages_obj;
	int idx = 0, ret = 0;
	struct zone *zone_cur, *zone_prev;
	unsigned long flags = 0;
	int hyp_idx = 0;
	int free_pages_idx = hinting_obj->free_pages_idx;

	isolated_pages_obj = kmalloc(MAX_FGPT_ENTRIES *
			sizeof(struct guest_isolated_pages), GFP_KERNEL);
	if (!isolated_pages_obj) {
		hinting_obj->free_pages_idx = 0;
		put_cpu_var(hinting_obj);
		return;
		/* return some logical error here*/
	}

	sort(hinting_obj->free_page_arr, free_pages_idx,
	     sizeof(unsigned long), sort_zonenum, NULL);

	while (idx < free_pages_idx) {
		unsigned long pfn = hinting_obj->free_page_arr[idx];
		unsigned long pfn_end = hinting_obj->free_page_arr[idx] +
			(1 << FREE_PAGE_HINTING_MIN_ORDER) - 1;

		zone_cur = page_zone(pfn_to_page(pfn));
		if (idx == 0) {
			zone_prev = zone_cur;
			spin_lock_irqsave(&zone_cur->lock, flags);
		} else if (zone_prev != zone_cur) {
			spin_unlock_irqrestore(&zone_prev->lock, flags);
			spin_lock_irqsave(&zone_cur->lock, flags);
			zone_prev = zone_cur;
		}

		while (pfn <= pfn_end) {
			struct page *page = pfn_to_page(pfn);

			if (PageCompound(page)) {
				struct page *head_page = compound_head(page);
				unsigned long head_pfn = page_to_pfn(head_page);
				unsigned int alloc_pages =
					1 << compound_order(head_page);

				pfn = head_pfn + alloc_pages;
				continue;
			}

			if (page_ref_count(page)) {
				pfn++;
				continue;
			}

			if (PageBuddy(page) && page_private(page) >=
			    FREE_PAGE_HINTING_MIN_ORDER) {
				int buddy_order = page_private(page);

				ret = __isolate_free_page(page, buddy_order);
				if (ret) {
					trace_guest_isolated_page(pfn,
								  buddy_order);
					isolated_pages_obj[hyp_idx].pfn = pfn;
					isolated_pages_obj[hyp_idx].len = (1 << buddy_order) * 4;
					hyp_idx += 1;
				}
				pfn = pfn + (1 << buddy_order);
				continue;
			}
			pfn++;
		}
		hinting_obj->free_page_arr[idx] = 0;
		idx++;
		if (idx == free_pages_idx)
			spin_unlock_irqrestore(&zone_cur->lock, flags);
	}

	if (hyp_idx > 0)
		guest_free_page_report(isolated_pages_obj, hyp_idx);
	else
		kfree(isolated_pages_obj);
		/* return some logical error here*/
	hinting_obj->free_pages_idx = 0;
	put_cpu_var(hinting_obj);
}

int if_exist(struct page *page)
{
	int i = 0;
	struct guest_free_pages *hinting_obj = this_cpu_ptr(&free_pages_obj);

	while (i < MAX_FGPT_ENTRIES) {
		if (page_to_pfn(page) == hinting_obj->free_page_arr[i])
			return 1;
		i++;
	}
	return 0;
}

void guest_free_page_enqueue(struct page *page, int order)
{
	unsigned long flags;
	struct guest_free_pages *hinting_obj;
	int l_idx;

	if (!static_branch_unlikely(&guest_free_page_hinting_key))
		return;
	/*
	 * use of global variables may trigger a race condition between irq and
	 * process context causing unwanted overwrites. This will be replaced
	 * with a better solution to prevent such race conditions.
	 */
	local_irq_save(flags);
	hinting_obj = this_cpu_ptr(&free_pages_obj);
	l_idx = hinting_obj->free_pages_idx;
	trace_guest_free_page(page_to_pfn(page), order);
	if (l_idx != MAX_FGPT_ENTRIES) {
		if (PageBuddy(page) && page_private(page) >=
		    FREE_PAGE_HINTING_MIN_ORDER) {
			trace_guest_captured_page(page_to_pfn(page), order,
						  l_idx);
			hinting_obj->free_page_arr[l_idx] = page_to_pfn(page);
			hinting_obj->free_pages_idx += 1;
		} else {
			struct page *buddy_page = get_buddy_page(page);

			if (buddy_page && page_private(buddy_page) >=
			    FREE_PAGE_HINTING_MIN_ORDER &&
			    !if_exist(buddy_page)) {
				unsigned long buddy_pfn =
					page_to_pfn(buddy_page);
				unsigned int buddy_order =
					page_private(buddy_page);

				trace_guest_captured_page(buddy_pfn,
							  buddy_order, l_idx);
				hinting_obj->free_page_arr[l_idx] =
							buddy_pfn;
				hinting_obj->free_pages_idx += 1;
			}
		}
	}
	local_irq_restore(flags);
}

void guest_free_page_try_hinting(void)
{
	struct guest_free_pages *hinting_obj;

	if (!static_branch_unlikely(&guest_free_page_hinting_key))
		return;
	hinting_obj = this_cpu_ptr(&free_pages_obj);
	if (hinting_obj->free_pages_idx >= HINTING_THRESHOLD)
		guest_free_page_hinting();
}
