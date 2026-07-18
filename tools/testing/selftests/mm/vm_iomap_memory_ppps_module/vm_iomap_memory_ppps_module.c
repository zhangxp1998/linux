// SPDX-License-Identifier: GPL-2.0

#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>

#include "../vm_iomap_memory_ppps.h"

#define USER_PAGE_SIZE	4096UL
#define TEST_BUFFER_SIZE	(4 * USER_PAGE_SIZE)

static void *test_buffer;

static int vm_iomap_memory_ppps_mmap(struct file *file,
				     struct vm_area_struct *vma)
{
	return vm_iomap_memory(vma, virt_to_phys(test_buffer), TEST_BUFFER_SIZE);
}

static const struct file_operations vm_iomap_memory_ppps_fops = {
	.owner = THIS_MODULE,
	.mmap = vm_iomap_memory_ppps_mmap,
};

static struct miscdevice vm_iomap_memory_ppps_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = VM_IOMAP_MEMORY_PPPS_DEVICE_NAME,
	.fops = &vm_iomap_memory_ppps_fops,
};

static int __init vm_iomap_memory_ppps_init(void)
{
	unsigned int offset;
	int error;

	test_buffer = alloc_pages_exact(TEST_BUFFER_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!test_buffer)
		return -ENOMEM;
	for (offset = 0; offset < TEST_BUFFER_SIZE; offset += USER_PAGE_SIZE)
		memset(test_buffer + offset, 0x71 + offset / USER_PAGE_SIZE,
		       USER_PAGE_SIZE);

	error = misc_register(&vm_iomap_memory_ppps_device);
	if (!error)
		return 0;
	free_pages_exact(test_buffer, TEST_BUFFER_SIZE);
	return error;
}

static void __exit vm_iomap_memory_ppps_exit(void)
{
	misc_deregister(&vm_iomap_memory_ppps_device);
	free_pages_exact(test_buffer, TEST_BUFFER_SIZE);
}

module_init(vm_iomap_memory_ppps_init);
module_exit(vm_iomap_memory_ppps_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PPPS vm_iomap_memory regression test helper");
