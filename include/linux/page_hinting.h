/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_HINTING_H
#define _LINUX_PAGE_HINTING_H

/*
 * Threshold value after which hinting needs to be initiated on the captured
 * free pages.
 */
#define HINTING_MEM_THRESHOLD		16
#define PAGE_HINTING_MIN_ORDER		(MAX_ORDER - 1)

void page_hinting_enqueue(struct page *page, int order);
void page_hinting_enable(void *vb, void (*vb_callback)(void*, void*, int));
void page_hinting_disable(void);
extern int __isolate_free_page(struct page *page, unsigned int order);
extern void __free_one_page(struct page *page, unsigned long pfn,
			    struct zone *zone, unsigned int order,
			    int migratetype);
#endif /* _LINUX_PAGE_HINTING_H */
