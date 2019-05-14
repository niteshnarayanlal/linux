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
void page_hinting_enable(void);
void page_hinting_disable(void);
#endif /* _LINUX_PAGE_HINTING_H */
