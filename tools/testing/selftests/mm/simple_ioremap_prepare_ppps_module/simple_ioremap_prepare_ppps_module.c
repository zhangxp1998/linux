// SPDX-License-Identifier: GPL-2.0

#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>

#include "../vm_iomap_memory_ppps.h"

#define USER_PAGE_SIZE	4096UL
#define TEST_BUFFER_SIZE	(4 * USER_PAGE_SIZE)

static void *test_buffer;

static int simple_ioremap_prepare_ppps_mmap(struct vm_area_desc *desc)
{
	mmap_action_simple_ioremap(desc, virt_to_phys(test_buffer),
				   TEST_BUFFER_SIZE);
	return 0;
}

static const struct file_operations simple_ioremap_prepare_ppps_fops = {
	.owner = THIS_MODULE,
	.mmap_prepare = simple_ioremap_prepare_ppps_mmap,
};

static struct miscdevice simple_ioremap_prepare_ppps_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = VM_IOMAP_MEMORY_PPPS_DEVICE_NAME,
	.fops = &simple_ioremap_prepare_ppps_fops,
};

static int __init simple_ioremap_prepare_ppps_init(void)
{
	unsigned int offset;
	int error;

	test_buffer = alloc_pages_exact(TEST_BUFFER_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!test_buffer)
		return -ENOMEM;
	for (offset = 0; offset < TEST_BUFFER_SIZE; offset += USER_PAGE_SIZE)
		memset(test_buffer + offset, 0x71 + offset / USER_PAGE_SIZE,
		       USER_PAGE_SIZE);

	error = misc_register(&simple_ioremap_prepare_ppps_device);
	if (!error)
		return 0;
	free_pages_exact(test_buffer, TEST_BUFFER_SIZE);
	return error;
}

static void __exit simple_ioremap_prepare_ppps_exit(void)
{
	misc_deregister(&simple_ioremap_prepare_ppps_device);
	free_pages_exact(test_buffer, TEST_BUFFER_SIZE);
}

module_init(simple_ioremap_prepare_ppps_init);
module_exit(simple_ioremap_prepare_ppps_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PPPS simple ioremap prepare regression test helper");
