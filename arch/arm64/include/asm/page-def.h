/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/include/asm/page.h
 *
 * Copyright (C) 1995-2003 Russell King
 * Copyright (C) 2017 ARM Ltd.
 */
#ifndef __ASM_PAGE_DEF_H
#define __ASM_PAGE_DEF_H

#include <linux/const.h>

/* PAGE_SHIFT determines the page size */
/* CONT_SHIFT determines the number of pages which can be tracked together  */
#define PAGE_SHIFT		CONFIG_ARM64_PAGE_SHIFT
#define CONT_SHIFT		CONFIG_ARM64_CONT_SHIFT
#define PAGE_SIZE		(_AC(1, UL) << PAGE_SHIFT)
#define PAGE_MASK		(~(PAGE_SIZE-1))

#define CONT_SIZE		(_AC(1, UL) << (CONT_SHIFT + PAGE_SHIFT))
#define CONT_MASK		(~(CONT_SIZE-1))

/*
 * Hugetlb definitions.
 */
#define HUGE_MAX_HSTATE		4
#define HPAGE_SHIFT		PMD_SHIFT
#define HPAGE_SIZE		(_AC(1, UL) << HPAGE_SHIFT)
#define HPAGE_MASK		(~(HPAGE_SIZE - 1))
#define HUGETLB_PAGE_ORDER	(HPAGE_SHIFT - PAGE_SHIFT)

#endif /* __ASM_PAGE_DEF_H */
