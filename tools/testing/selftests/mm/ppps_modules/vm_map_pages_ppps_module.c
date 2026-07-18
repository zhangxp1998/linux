// SPDX-License-Identifier: GPL-2.0

#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>

#include "../../ppps/ppps_misc_module.h"

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

static int vm_map_pages_ppps_setup(void)
{
	int error = allocate_test_pages();

	if (error)
		free_test_pages();
	return error;
}

PPPS_MISC_MODULE("vm_map_pages_ppps", &vm_map_pages_ppps_fops, 0,
		 vm_map_pages_ppps_setup, free_test_pages,
		 "PPPS vm_map_pages regression test helper");
