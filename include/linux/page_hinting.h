/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_HINTING_H
#define _LINUX_PAGE_HINTING_H

/*
 * Minimum page order required for a page to be hinted to the host.
 */
#define PAGE_HINTING_MIN_ORDER		(MAX_ORDER - 2)

/*
 * struct page_hinting_config: holds the information supplied by the balloon
 * device to page hinting.
 * @hint_pages:		Callback which reports the isolated pages
 *			synchornously to the host.
 * @max_pages:		Maxmimum pages that are going to be hinted to the host
 *			at a time of granularity >= PAGE_HINTING_MIN_ORDER.
 */
struct page_hinting_config {
	void (*hint_pages)(struct list_head *list);
	int max_pages;
};

extern int __isolate_free_page(struct page *page, unsigned int order);
extern void __free_one_page(struct page *page, unsigned long pfn,
			    struct zone *zone, unsigned int order,
			    int migratetype, bool hint);
#ifdef CONFIG_PAGE_HINTING
void page_hinting_enqueue(struct page *page, int order);
int page_hinting_enable(const struct page_hinting_config *conf);
void page_hinting_disable(void);
#else
static inline void page_hinting_enqueue(struct page *page, int order)
{
}

static inline int page_hinting_enable(const struct page_hinting_config *conf)
{
	return -EOPNOTSUPP;
}

static inline void page_hinting_disable(void)
{
}
#endif
#endif /* _LINUX_PAGE_HINTING_H */
