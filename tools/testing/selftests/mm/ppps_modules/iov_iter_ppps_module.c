// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/uio.h>

#include "../iov_iter_ppps.h"
#include "../page_range_ppps.h"

#include "../../ppps/ppps_misc_module.h"

#define PROCESS_PAGE_SIZE 4096UL

static const u8 expected[] = { 0x31, 0x72, 0x93, 0xb4 };

static int check_page_value(struct page *page, size_t offset, u8 value)
{
	void *base;
	int ret;

	if (offset >= PAGE_SIZE)
		return -ERANGE;
	base = kmap_local_page(page);
	ret = *((u8 *)base + offset) == value ? 0 : -ENODATA;
	kunmap_local(base);
	return ret;
}

static int check_page(struct page *page, size_t offset, u8 value,
		      bool expect_packed, struct page **first_page)
{
	if (expect_packed) {
		if (!folio_test_ppps_compat_anon(page_folio(page)))
			return -ENODATA;
		if (*first_page && page != *first_page)
			return -ENXIO;
		*first_page = page;
	}
	return check_page_value(page, offset, value);
}

static int run_get_pages(unsigned long address, size_t length,
			 bool expect_packed)
{
	struct page *first_page = NULL;
	struct iov_iter iter;
	unsigned int nr_slices = length / PROCESS_PAGE_SIZE;
	unsigned int i;

	iov_iter_ubuf(&iter, ITER_SOURCE, (void __user *)address, length);
	for (i = 0; i < nr_slices; i++) {
		struct page *page = NULL;
		size_t offset = 0;
		ssize_t ret;
		int err;

		ret = iov_iter_get_pages2(&iter, &page, PROCESS_PAGE_SIZE, 1,
					  &offset);
		if (ret != PROCESS_PAGE_SIZE)
			return ret < 0 ? ret : -EMSGSIZE;
		err = check_page(page, offset, expected[i], expect_packed,
				 &first_page);
		put_page(page);
		if (err)
			return err;
	}
	return iov_iter_count(&iter) ? -EMSGSIZE : 0;
}

static int run_extract_pages(unsigned long address, size_t length,
			     bool expect_packed)
{
	struct page *first_page = NULL;
	struct iov_iter iter;
	unsigned int nr_slices = length / PROCESS_PAGE_SIZE;
	unsigned int i;

	iov_iter_ubuf(&iter, ITER_SOURCE, (void __user *)address, length);
	for (i = 0; i < nr_slices; i++) {
		struct page *storage[1];
		struct page **pages = storage;
		size_t offset = 0;
		ssize_t ret;
		int err;

		ret = iov_iter_extract_pages(&iter, &pages,
					     PROCESS_PAGE_SIZE, 1, 0, &offset);
		if (ret != PROCESS_PAGE_SIZE)
			return ret < 0 ? ret : -EMSGSIZE;
		err = check_page(pages[0], offset, expected[i], expect_packed,
				 &first_page);
		unpin_user_page(pages[0]);
		if (err)
			return err;
	}
	return iov_iter_count(&iter) ? -EMSGSIZE : 0;
}

static ssize_t run_bulk_extract(unsigned long address, size_t length)
{
	struct page *storage[2];
	struct page **pages = storage;
	struct iov_iter iter;
	size_t offset = 0;
	ssize_t ret;
	int i;

	iov_iter_ubuf(&iter, ITER_SOURCE, (void __user *)address, length);
	ret = iov_iter_extract_pages(&iter, &pages, length,
				     ARRAY_SIZE(storage), 0, &offset);
	if (ret > 0) {
		int nr_pages = DIV_ROUND_UP(ret, PROCESS_PAGE_SIZE);

		for (i = 0; i < nr_pages; i++)
			unpin_user_page(pages[i]);
	}
	return ret;
}

/* Pure offset/unit contracts, exercised in both native and compat processes. */
static bool check_public_helpers(void)
{
	static const struct vm_operations_struct ops;
	static struct file file;
	struct mm_struct *mm = current->mm;
	struct vm_area_struct vma = {
		.vm_mm = mm,
		.vm_ops = &ops,
		.vm_file = &file,
		.vm_start = 0x100000,
		.vm_end = 0x100000 + 2 * PAGE_SIZE,
	};
	struct vma_offset saved, next;
	unsigned long size = MM_PAGE_SIZE(mm);

	if (mm_user_range_pages(mm, 17, 0) ||
	    mm_user_range_pages(mm, 17, size) != 2 ||
	    mm_counter_to_bytes(mm, MM_ANONPAGES, 1) != PAGE_SIZE ||
	    mm_counter_to_bytes(mm, MM_SWAPENTS, 1) != size ||
	    mm_counter_to_bytes(mm, MM_FILEPAGES, 1) != size ||
	    mm_counter_to_bytes(mm, MM_SHMEMPAGES, 1) != size)
		return false;
	if (vma_set_file_offset(&vma, PAGE_SIZE - size))
		return false;
	saved = vma_get_offset(&vma);
	if (vma_set_file_offset(&vma, 1) != -EINVAL ||
	    vma_file_offset(&vma) != PAGE_SIZE - size)
		return false;
	next = vma_offset_advance(mm, saved, ppps_vma_has_slices(&vma), size);
	vma_set_offset(&vma, next);
	if (vma_file_offset(&vma) != PAGE_SIZE)
		return false;
	vma_set_offset(&vma, saved);
	if (vma_addr_file_offset(&vma, vma.vm_start + size) != PAGE_SIZE)
		return false;
	next = vma_offset_at(&vma, vma.vm_start + size);
	if (next.pgoff != 1 || next.slice != 0)
		return false;
	/* Lossless driver-selector save/restore, even beyond byte-offset width. */
	saved.pgoff = ULONG_MAX;
	vma_set_offset(&vma, saved);
	return vma_get_offset(&vma).pgoff == ULONG_MAX;
}

static long page_range_ioctl(unsigned long arg)
{
	struct page_range_ppps_args request;
	struct page_span spans[PAGE_RANGE_PPPS_MAX];
	struct page *pages[PAGE_RANGE_PPPS_MAX];
	long nr;
	int i;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (request.capacity > ARRAY_SIZE(pages) || request.pin > 1 ||
	    request.address > ULONG_MAX || request.length > SIZE_MAX)
		return -EINVAL;
	memset(request.spans, 0, sizeof(request.spans));
	request.helpers_ok = check_public_helpers();
	nr = request.pin ?
		     pin_user_pages_range(current->mm, request.address,
					  request.length, request.capacity,
					  FOLL_WRITE, pages, spans) :
		     get_user_pages_range(current->mm, request.address,
					  request.length, request.capacity,
					  FOLL_WRITE, pages, spans);
	request.result = nr;
	for (i = 0; i < nr; i++) {
		u8 *base;

		if (!spans[i].length || spans[i].offset >= PAGE_SIZE ||
		    spans[i].length > PAGE_SIZE - spans[i].offset) {
			if (request.pin)
				unpin_user_pages(pages + i, nr - i);
			else
				release_pages(pages + i, nr - i);
			return -ERANGE;
		}
		base = kmap_local_page(pages[i]);

		request.spans[i].offset = spans[i].offset;
		request.spans[i].length = spans[i].length;
		request.spans[i].first = base[spans[i].offset];
		request.spans[i].last =
			base[spans[i].offset + spans[i].length - 1];
		kunmap_local(base);
		if (request.pin)
			unpin_user_page(pages[i]);
		else
			put_page(pages[i]);
	}
	return copy_to_user((void __user *)arg, &request, sizeof(request)) ?
		       -EFAULT :
		       0;
}

static long iov_iter_ppps_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct iov_iter_ppps_args request;
	bool expect_packed;

	if (cmd == PAGE_RANGE_PPPS_IOCTL)
		return page_range_ioctl(arg);
	if (cmd != IOV_ITER_PPPS_IOCTL)
		return -EINVAL;
	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (!request.length || request.length % PROCESS_PAGE_SIZE ||
	    request.length > ARRAY_SIZE(expected) * PROCESS_PAGE_SIZE ||
	    request.flags & ~IOV_ITER_PPPS_F_EXPECT_PACKED)
		return -EINVAL;
	expect_packed = request.flags & IOV_ITER_PPPS_F_EXPECT_PACKED;

	request.native_page_size = PAGE_SIZE;
	request.get_pages_result = run_get_pages(request.address,
						 request.length, expect_packed);
	request.extract_pages_result = run_extract_pages(request.address,
							 request.length,
							 expect_packed);
	request.bulk_first_len = run_bulk_extract(request.address,
						  request.length);
	if (copy_to_user((void __user *)arg, &request, sizeof(request)))
		return -EFAULT;
	return 0;
}

static const struct file_operations iov_iter_ppps_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = iov_iter_ppps_ioctl,
	.compat_ioctl = iov_iter_ppps_ioctl,
};

PPPS_MISC_MODULE(IOV_ITER_PPPS_DEVICE_NAME, &iov_iter_ppps_fops, 0,
		 ppps_misc_no_setup, ppps_misc_no_teardown,
		 "PPPS user-backed iov_iter page extraction regression helper");
