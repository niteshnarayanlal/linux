/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_HINTING_H
#define _LINUX_PAGE_HINTING_H

/* Minimum page order required for a page to be hinted to the hypervisor */
#define PAGE_HINTING_MIN_ORDER		(MAX_ORDER - 2)
/* Number of isolated pages to be reported to the hypervisor at a time */
#define PAGE_HINTING_MAX_PAGES		16

/*
 * struct page_hinting_config - holds the information supplied by the backend
 * driver to page hinting.
 * @hint_pages:		Callback which reports batch of isolated pages
 *			synchornously to the host.
 * @sg:			scatterlist to hold the isolated pages to be hinted.
 * @max_pages:		Maxmimum pages that are going to be hinted to the host
 *			at a time of granularity >= PAGE_HINTING_MIN_ORDER.
 * @hinting_work:	work object to process page hinting rqeuests.
 * @refcnt:		Track if page hinting is under processing.
 */
struct page_hinting_config {
	void (*hint_pages)(struct page_hinting_config *phconf,
			   unsigned int num_hints);
	struct scatterlist *sg;
	int max_pages;
	struct work_struct hinting_work;
	atomic_t refcnt;
};

void set_pageblock_migratetype(struct page *page, int migratetype);
void __page_hinting_enqueue(struct page *page);
void __release_hinted_page(struct zone *zone, struct page *page);

#ifdef CONFIG_PAGE_HINTING
/*
 * page_hinting_enqueue - Checks the eligibility of the freed page based on
 * its order for further page hinting processing.
 */
static inline void page_hinting_enqueue(struct page *page, int order)
{
	if (order < PAGE_HINTING_MIN_ORDER)
		return;
	__page_hinting_enqueue(page);
}

int page_hinting_enable(struct page_hinting_config *conf);
void page_hinting_disable(struct page_hinting_config *conf);
#else
static inline void page_hinting_enqueue(struct page *page, int order)
{
}

static inline int page_hinting_enable(struct page_hinting_config *conf)
{
	return -EOPNOTSUPP;
}

static inline void page_hinting_disable(struct page_hinting_config *conf)
{
}
#endif
#endif /* _LINUX_PAGE_HINTING_H */
