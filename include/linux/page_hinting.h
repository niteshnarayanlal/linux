/*
 * Threshold value after which hinting needs to be initiated on the captured
 * free pages.
 */
#define HINTING_THRESHOLD		16
#define MAX_ZONES			3
#define FREE_PAGE_HINTING_MIN_ORDER	(MAX_ORDER - 1)

extern int guest_free_page_hinting_flag;

void guest_free_page_enqueue(struct page *page, int order);
void guest_free_page_try_hinting(void);
void guest_free_page_hinting_enable(void);
void guest_free_page_hinting_disable(void);
int guest_free_page_hinting_sysctl(struct ctl_table *table, int write,
				   void __user *buffer, size_t *lenp,
				   loff_t *ppos);
