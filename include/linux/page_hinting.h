#include <linux/gfp.h>
/*
 * Size of the array which is used to store the freed pages is defined by
 * MAX_FGPT_ENTRIES.
 */
#define MAX_FGPT_ENTRIES	256
/*
 * Threshold value after which hinting needs to be initiated on the captured
 * free pages.
 */
#define HINTING_THRESHOLD	128
#define FREE_PAGE_HINTING_MIN_ORDER	(MAX_ORDER - 1)

extern void *balloon_ptr;

void guest_free_page_enqueue(struct page *page, int order);
void guest_free_page_try_hinting(void);
extern int __isolate_free_page(struct page *page, unsigned int order);
extern void __free_one_page(struct page *page, unsigned long pfn,
			    struct zone *zone, unsigned int order,
			    int migratetype);
void release_buddy_pages(void *obj_to_free, int entries);
extern void (*request_hypercall)(void *balloon_ptr,
				 void *guest_req, int entries);
