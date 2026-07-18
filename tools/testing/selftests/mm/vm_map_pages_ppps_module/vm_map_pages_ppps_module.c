// SPDX-License-Identifier: GPL-2.0

#include <linux/highmem.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>

#include "../vm_map_pages_ppps.h"

#define USER_PAGE_SIZE	4096UL
#define TEST_PAGE_COUNT	8

static struct page *test_pages[TEST_PAGE_COUNT];

static int vm_map_pages_ppps_mmap(struct file *file,
				  struct vm_area_struct *vma)
{
	return vm_map_pages(vma, test_pages, ARRAY_SIZE(test_pages));
}

static const struct file_operations vm_map_pages_ppps_fops = {
	.owner = THIS_MODULE,
	.mmap = vm_map_pages_ppps_mmap,
};

static struct miscdevice vm_map_pages_ppps_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = VM_MAP_PAGES_PPPS_DEVICE_NAME,
	.fops = &vm_map_pages_ppps_fops,
};

static void free_test_pages(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(test_pages); i++) {
		if (test_pages[i])
			__free_page(test_pages[i]);
	}
}

static int allocate_test_pages(void)
{
	unsigned int logical_page = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(test_pages); i++) {
		unsigned char *address;
		unsigned int offset;

		test_pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!test_pages[i])
			return -ENOMEM;
		address = kmap_local_page(test_pages[i]);
		for (offset = 0; offset < PAGE_SIZE; offset += USER_PAGE_SIZE)
			memset(address + offset, 0x40 + logical_page++,
			       USER_PAGE_SIZE);
		kunmap_local(address);
	}
	return 0;
}

static int __init vm_map_pages_ppps_init(void)
{
	int error;

	error = allocate_test_pages();
	if (error)
		goto free_pages;
	error = misc_register(&vm_map_pages_ppps_device);
	if (!error)
		return 0;
free_pages:
	free_test_pages();
	return error;
}

static void __exit vm_map_pages_ppps_exit(void)
{
	misc_deregister(&vm_map_pages_ppps_device);
	free_test_pages();
}

module_init(vm_map_pages_ppps_init);
module_exit(vm_map_pages_ppps_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PPPS vm_map_pages regression test helper");
