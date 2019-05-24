/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PAGE_HINTING_H
#define _LINUX_PAGE_HINTING_H

/*
 * Threshold value after which hinting needs to be initiated on the captured
 * free pages.
 */
#define HINTING_MEM_THRESHOLD		16
#define PAGE_HINTING_MIN_ORDER		(MAX_ORDER - 2)

/*
 * struct hinting_cb: holds the callbacks to store, report and cleanup
 * isolated pages. It also holds setup flag the work structure object required
 * to initiate hinting.
 * @prepare:		Callback responsible for allocating an array to hold
 *			the isolated pages.
 * @hint_isolated_page:	Callback which adds an isolated page to the array and
 *			reports them to the host if the threshold is met.
 * @cleanup:		Callback which reports if there are any remaining
 *			isolated pages remaining and frees the isolated array.
 */
struct hinting_cb {
	int (*prepare)(void);
	void (*hint_page)(u64 phys_addr, u32 len);
	void (*cleanup)(void);
};

#ifdef CONFIG_PAGE_HINTING
void page_hinting_enqueue(struct page *page, int order);
#else
void page_hinting_enqueue(struct page *page, int order)
{
}
#endif
void page_hinting_enqueue(struct page *page, int order);
void page_hinting_enable(struct hinting_cb *cb);
void page_hinting_disable(void);
#endif /* _LINUX_PAGE_HINTING_H */
