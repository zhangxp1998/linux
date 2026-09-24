// SPDX-License-Identifier: GPL-2.0-only
/* Byte-range consumers of PPPS GUP. */
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uio.h>

#include "ppps.h"

/* Explicit opt-in: PAGE_* below uses the scoped target MM. */
#include <linux/p3s_user_pages.h>

int ppps_process_vm_rw(struct mm_struct *mm, unsigned long addr,
		unsigned long len, struct iov_iter *iter, struct page **pages,
		unsigned long capacity, bool write)
{
	P3S_CONTEXT_REMOTE_MM(mm);
	struct page_span *spans;
	unsigned long max_pages = min(capacity, mm_user_range_pages(mm, addr, len));
	unsigned int flags = write ? FOLL_WRITE : 0;
	int ret = 0;

	if (!len)
		return 0;
	spans = kmalloc_array(max_pages, sizeof(*spans), GFP_KERNEL);
	if (!spans)
		return -ENOMEM;
	while (len && iov_iter_count(iter)) {
		size_t bytes = min_t(unsigned long, len,
			max_pages * PAGE_SIZE - offset_in_page(addr));
		long nr, i;

		nr = pin_user_pages_range(mm, addr, bytes, max_pages, flags,
					  pages, spans);
		if (nr <= 0) {
			ret = -EFAULT;
			break;
		}
		for (i = 0; i < nr && iov_iter_count(iter); i++) {
			size_t copied;

			if (write)
				copied = copy_page_from_iter(pages[i], spans[i].offset,
							    spans[i].length, iter);
			else
				copied = copy_page_to_iter(pages[i], spans[i].offset,
							  spans[i].length, iter);
			addr += copied;
			len -= copied;
			if (copied < spans[i].length) {
				if (iov_iter_count(iter))
					ret = -EFAULT;
				break;
			}
		}
		unpin_user_pages_dirty_lock(pages, nr, write);
		if (ret)
			break;
	}
	kfree(spans);
	return ret;
}
