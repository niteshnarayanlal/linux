#include <linux/gfp.h>
#include <linux/mmzone.h>
#include <linux/smpboot.h>
/*
 * Size of the array which is used to store the freed pages is defined by
 * MAX_FGPT_ENTRIES.
 */
#define MAX_FGPT_ENTRIES	32
/*
 * Threshold value after which hinting needs to be initiated on the captured
 * free pages.
 */
#define HINTING_THRESHOLD	16
#define FREE_PAGE_HINTING_MIN_ORDER	(MAX_ORDER - 1)

extern void *balloon_ptr;
extern int guest_free_page_hinting_flag;
extern struct static_key_false guest_free_page_hinting_key;
extern struct smp_hotplug_thread hinting_threads;

void guest_free_page_enqueue(struct page *page, int order);
void guest_free_page_try_hinting(void);
extern int __isolate_free_page(struct page *page, unsigned int order);
extern void __free_one_page(struct page *page, unsigned long pfn,
			    struct zone *zone, unsigned int order,
			    int migratetype);
void release_buddy_pages(void *obj_to_free, int entries);
extern int (*request_hypercall)(void *balloon_ptr,
				void *hinting_req, int entries);
int guest_free_page_hinting_sysctl(struct ctl_table *table, int write,
				   void __user *buffer, size_t *lenp,
				   loff_t *ppos);
