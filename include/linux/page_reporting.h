/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_REPORTING_H
#define _LINUX_PAGE_REPORTING_H

//#define PAGE_REPORTING_MIN_ORDER		(MAX_ORDER - 1)
#define PAGE_REPORTING_MIN_ORDER		pageblock_order
#define PAGE_REPORTING_MAX_PAGES		32

#ifdef CONFIG_PAGE_REPORTING
struct page_reporting_config {
	/* function to hint batch of isolated pages */
	void (*report)(struct page_reporting_config *phconf,
		       unsigned int num_pages);

	/* scatterlist to hold the isolated pages to be hinted */
	struct scatterlist *sg;

	/*
	 * Maxmimum pages that are going to be hinted to the hypervisor at a
	 * time of granularity >= PAGE_REPORTING_MIN_ORDER.
	 */
	int max_pages;

	/* work object to process page reporting rqeuests */
	struct work_struct reporting_work;

	/* tracks the number of reporting request processed at a time */
	atomic_t refcnt;
};

void __page_reporting_enqueue(struct page *page);
void __return_isolated_page(struct zone *zone, struct page *page);
void set_pageblock_migratetype(struct page *page, int migratetype);
void __page_reporting_dequeue(struct page *page);
struct page *get_buddy_page(struct page *page);

/**
 * page_reporting_enqueue - checks the eligibility of the freed page based on
 * its order for further page reporting processing.
 * @page: page which has been freed.
 * @order: order for the the free page.
 */
static inline void page_reporting_enqueue(struct page *page, int order)
{
	if (order < PAGE_REPORTING_MIN_ORDER)
		return;
	__page_reporting_enqueue(page);
}

static inline void page_reporting_dequeue(struct page *page)
{
	struct page *buddy_page = get_buddy_page(page);
	
	if (page_private(buddy_page) < PAGE_REPORTING_MIN_ORDER)
		return;
	__page_reporting_dequeue(page);
}

int page_reporting_enable(struct page_reporting_config *phconf);
void page_reporting_disable(struct page_reporting_config *phconf);
#else
static inline void page_reporting_dequeue(struct page *page, int order)
{
}

static inline void page_reporting_enqueue(struct page *page, int order)
{
}

static inline int page_reporting_enable(struct page_reporting_config *phconf)
{
	return -EOPNOTSUPP;
}

static inline void page_reporting_disable(struct page_reporting_config *phconf)
{
}
#endif /* CONFIG_PAGE_REPORTING */
#endif /* _LINUX_PAGE_REPORTING_H */
