/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_HINTING_H
#define _LINUX_PAGE_HINTING_H

#define PAGE_HINTING_MIN_ORDER		(MAX_ORDER - 2)
#define PAGE_HINTING_MAX_PAGES		16

#ifdef CONFIG_PAGE_HINTING
struct page_hinting_config {
	/* function to hint batch of isolated pages */
	void (*hint_pages)(struct page_hinting_config *phconf,
			   unsigned int num_hints);

	/* scatterlist to hold the isolated pages to be hinted */
	struct scatterlist *sg;

	/*
	 * Maxmimum pages that are going to be hinted to the hypervisor at a
	 * time of granularity >= PAGE_HINTING_MIN_ORDER.
	 */
	int max_pages;

	/* work object to process page hinting rqeuests */
	struct work_struct hinting_work;

	/* tracks the number of hinting request processed at a time */
	atomic_t refcnt;
};

void __page_hinting_enqueue(struct page *page);
void __return_isolated_page(struct zone *zone, struct page *page);
void set_pageblock_migratetype(struct page *page, int migratetype);

/**
 * page_hinting_enqueue - checks the eligibility of the freed page based on
 * its order for further page hinting processing.
 * @page: page which has been freed.
 * @order: order for the the free page.
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
