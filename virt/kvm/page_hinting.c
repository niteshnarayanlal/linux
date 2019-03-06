#include <linux/mm.h>
#include <linux/page_hinting.h>

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

	/*
	 * use of global variables may trigger a race condition between irq and
	 * process context causing unwanted overwrites. This will be replaced
	 * with a better solution to prevent such race conditions.
	 */
	local_irq_save(flags);
	hinting_obj = this_cpu_ptr(&free_pages_obj);
	l_idx = hinting_obj->free_pages_idx;
	if (l_idx != MAX_FGPT_ENTRIES) {
		if (PageBuddy(page) && page_private(page) >=
		    FREE_PAGE_HINTING_MIN_ORDER) {
			hinting_obj->free_page_arr[l_idx] = page_to_pfn(page);
			hinting_obj->free_pages_idx += 1;
		} else {
			struct page *buddy_page = get_buddy_page(page);

			if (buddy_page && page_private(buddy_page) >=
			    FREE_PAGE_HINTING_MIN_ORDER &&
			    !if_exist(buddy_page)) {
				unsigned long buddy_pfn =
					page_to_pfn(buddy_page);

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

	hinting_obj = this_cpu_ptr(&free_pages_obj);
	if (hinting_obj->free_pages_idx >= HINTING_THRESHOLD)
		guest_free_page_hinting();
}
