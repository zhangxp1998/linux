// SPDX-License-Identifier: GPL-2.0

#include <linux/highmem.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>

#include "../mmap_action_map_kernel_pages_ppps.h"

#define USER_PAGE_SIZE	4096UL
#define TEST_SIZE	(4 * USER_PAGE_SIZE)
#define MAX_TEST_PAGES	(TEST_SIZE / USER_PAGE_SIZE)
#define FIRST_MARKER	0x81

static struct page *test_pages[MAX_TEST_PAGES];
static unsigned int nr_test_pages;

static int mmap_action_map_kernel_pages_ppps_mmap(struct vm_area_desc *desc)
{
	if (vma_desc_size(desc) != TEST_SIZE || desc->pgoff)
		return -EINVAL;
	mmap_action_map_kernel_pages_full(desc, test_pages);
	return 0;
}

static const struct file_operations mmap_action_map_kernel_pages_ppps_fops = {
	.owner = THIS_MODULE,
	.mmap_prepare = mmap_action_map_kernel_pages_ppps_mmap,
};

static struct miscdevice mmap_action_map_kernel_pages_ppps_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = MMAP_ACTION_MAP_KERNEL_PAGES_PPPS_DEVICE_NAME,
	.fops = &mmap_action_map_kernel_pages_ppps_fops,
};

static void free_test_pages(void)
{
	unsigned int i;

	for (i = 0; i < nr_test_pages; i++)
		__free_page(test_pages[i]);
}

static int allocate_test_pages(void)
{
	unsigned int logical_page = 0;
	unsigned int i;

	nr_test_pages = DIV_ROUND_UP(TEST_SIZE, PAGE_SIZE);
	for (i = 0; i < nr_test_pages; i++) {
		unsigned char *address;
		unsigned int offset;

		test_pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!test_pages[i])
			return -ENOMEM;
		address = kmap_local_page(test_pages[i]);
		for (offset = 0; offset < PAGE_SIZE; offset += USER_PAGE_SIZE)
			memset(address + offset, FIRST_MARKER + logical_page++,
			       USER_PAGE_SIZE);
		kunmap_local(address);
	}
	return 0;
}

static int __init mmap_action_map_kernel_pages_ppps_init(void)
{
	int error;

	error = allocate_test_pages();
	if (error)
		goto free_pages;
	error = misc_register(&mmap_action_map_kernel_pages_ppps_device);
	if (!error)
		return 0;
free_pages:
	free_test_pages();
	return error;
}

static void __exit mmap_action_map_kernel_pages_ppps_exit(void)
{
	misc_deregister(&mmap_action_map_kernel_pages_ppps_device);
	free_test_pages();
}

module_init(mmap_action_map_kernel_pages_ppps_init);
module_exit(mmap_action_map_kernel_pages_ppps_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PPPS mmap action map-kernel-pages regression helper");
