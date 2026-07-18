// SPDX-License-Identifier: GPL-2.0

#include <linux/dma-mapping.h>
#include <linux/highmem.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>

#include "../dma_mmap_pages_ppps.h"

#define USER_PAGE_SIZE	4096UL
#define TEST_BUFFER_SIZE	(4 * USER_PAGE_SIZE)

static struct page *test_page;

static int dma_mmap_pages_ppps_mmap(struct file *file,
				    struct vm_area_struct *vma)
{
	return dma_mmap_pages(NULL, vma, TEST_BUFFER_SIZE, test_page);
}

static const struct file_operations dma_mmap_pages_ppps_fops = {
	.owner = THIS_MODULE,
	.mmap = dma_mmap_pages_ppps_mmap,
};

static struct miscdevice dma_mmap_pages_ppps_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DMA_MMAP_PAGES_PPPS_DEVICE_NAME,
	.fops = &dma_mmap_pages_ppps_fops,
};

static int __init dma_mmap_pages_ppps_init(void)
{
	unsigned int offset;
	int error;

	test_page = alloc_pages(GFP_KERNEL | __GFP_ZERO,
				get_order(TEST_BUFFER_SIZE));
	if (!test_page)
		return -ENOMEM;

	for (offset = 0; offset < TEST_BUFFER_SIZE; offset += PAGE_SIZE) {
		unsigned char *address;
		unsigned int page_offset;

		address = kmap_local_page(test_page + offset / PAGE_SIZE);
		for (page_offset = 0; page_offset < PAGE_SIZE;
		     page_offset += USER_PAGE_SIZE)
			memset(address + page_offset,
			       0x51 + (offset + page_offset) / USER_PAGE_SIZE,
			       USER_PAGE_SIZE);
		kunmap_local(address);
	}

	error = misc_register(&dma_mmap_pages_ppps_device);
	if (!error)
		return 0;
	__free_pages(test_page, get_order(TEST_BUFFER_SIZE));
	return error;
}

static void __exit dma_mmap_pages_ppps_exit(void)
{
	misc_deregister(&dma_mmap_pages_ppps_device);
	__free_pages(test_page, get_order(TEST_BUFFER_SIZE));
}

module_init(dma_mmap_pages_ppps_init);
module_exit(dma_mmap_pages_ppps_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PPPS dma_mmap_pages regression test helper");
