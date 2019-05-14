// SPDX-License-Identifier: GPL-2.0
/*
 * Free page hinting support to enable a VM to report the freed pages back
 * to the host.
 *
 * Copyright Red Hat, Inc. 2019
 *
 * Author(s): Nitesh Narayan Lal <nitesh@redhat.com>
 */

#include <linux/page_hinting.h>

void page_hinting_enqueue(struct page *page, int order)
{
}
