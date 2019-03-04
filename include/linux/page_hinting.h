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

