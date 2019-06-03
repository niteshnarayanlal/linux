/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_HINTING_H
#define _LINUX_PAGE_HINTING_H

/*
 * Minimum page order required for a page to be hinted to the host.
 */
#define PAGE_HINTING_MIN_ORDER		(MAX_ORDER - 2)

/*
 * struct page_hinting_cb: holds the callbacks to store, report and cleanup
 * isolated pages.
 * @prepare:		Callback responsible for allocating an array to hold
 *			the isolated pages.
 * @hint_pages:		Callback which reports the isolated pages synchornously
 *			to the host.
 * @cleanup:		Callback to free the the array used for reporting the
 *			isolated pages.
 * @max_pages:		Maxmimum pages that are going to be hinted to the host
 *			at a time of granularity >= PAGE_HINTING_MIN_ORDER.
 */
struct page_hinting_cb {
	int (*prepare)(void);
	void (*hint_pages)(struct list_head *list);
	void (*cleanup)(void);
	int max_pages;
};

#ifdef CONFIG_PAGE_HINTING
void page_hinting_enqueue(struct page *page, int order);
void page_hinting_enable(const struct page_hinting_cb *cb);
void page_hinting_disable(void);
#else
static inline void page_hinting_enqueue(struct page *page, int order)
{
}

static inline void page_hinting_enable(struct page_hinting_cb *cb)
{
}

static inline void page_hinting_disable(void)
{
}
#endif
#endif /* _LINUX_PAGE_HINTING_H */
