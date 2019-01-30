/*
 * Size of the array which is used to store the freed pages is defined by
 * MAX_FGPT_ENTRIES. If possible, we have to find a better way using which
 * we can get rid of the hardcoded array size.
 */
#define MAX_FGPT_ENTRIES	1000
/*
 * hypervisor_pages - It is a dummy structure passed with the hypercall.
 * @pfn: page frame number for the page which needs to be sent to the host.
 * @order: order of the page needs to be reported to the host.
 */
struct hypervisor_pages {
	unsigned long pfn;
	unsigned int order;
};

extern int guest_page_hinting_flag;
extern struct static_key_false guest_page_hinting_key;

int guest_page_hinting_sysctl(struct ctl_table *table, int write,
			      void __user *buffer, size_t *lenp, loff_t *ppos);
void guest_free_page(struct page *page, int order);
