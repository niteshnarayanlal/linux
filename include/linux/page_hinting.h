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
extern int __isolate_free_page(struct page *page, unsigned int order);
extern void __free_one_page(struct page *page, unsigned long pfn,
			    struct zone *zone, unsigned int order,
			    int migratetype);
void release_buddy_pages(void *obj_to_free, int entries);
