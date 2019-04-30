#include <linux/gfp.h>
#include <linux/mmzone.h>
/*
 * Threshold value after which hinting needs to be initiated on the captured
 * free pages.
 */
#define HINTING_THRESHOLD	16
#define FREE_PAGE_HINTING_MIN_ORDER	(MAX_ORDER - 1)
#define HINTING_BITMAP_SIZE	300000

struct hinting_bitmap {
	unsigned long *bitmap;
	struct zone *zone;
	atomic_t free_mem_cnt;
	struct mutex hbm_lock;
};

extern struct hinting_bitmap bm_zone[3];
extern struct work_struct hinting_work;
extern void *balloon_ptr;
extern int guest_free_page_hinting_flag;
extern struct static_key_false guest_free_page_hinting_key;
extern struct work_struct hinting_work;

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
void init_hinting_wq(struct work_struct *work);
