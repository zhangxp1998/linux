// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/uio.h>

#include "../iov_iter_ppps.h"

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

static long iov_iter_ppps_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	struct iov_iter_ppps_args request;
	bool expect_packed;

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
