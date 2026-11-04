// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include <linux/module.h>

#include "../p3s_misc_module.h"
#include "../vm_iomap_memory_p3s.h"

#define USER_PAGE_SIZE	4096UL
#define TEST_BUFFER_SIZE	(4 * USER_PAGE_SIZE)

static void *test_buffer;

static int vm_iomap_memory_p3s_mmap(struct file *file,
				    struct vm_area_struct *vma)
{
	phys_addr_t start = virt_to_phys(test_buffer);

	if (file->private_data)
		return vm_iomap_memory(vma, start + USER_PAGE_SIZE,
				       USER_PAGE_SIZE);
	return vm_iomap_memory(vma, start, TEST_BUFFER_SIZE);
}

static long vm_iomap_memory_p3s_ioctl(struct file *file, unsigned int cmd,
				      unsigned long arg)
{
	if (cmd != VM_IOMAP_MEMORY_P3S_SET_UNALIGNED)
		return -ENOTTY;
	file->private_data = (void *)1UL;
	return 0;
}

static int vm_iomap_memory_p3s_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static const struct file_operations vm_iomap_memory_p3s_fops = {
	.owner = THIS_MODULE,
	.open = vm_iomap_memory_p3s_open,
	.mmap = vm_iomap_memory_p3s_mmap,
	.unlocked_ioctl = vm_iomap_memory_p3s_ioctl,
	.compat_ioctl = vm_iomap_memory_p3s_ioctl,
};

static int vm_iomap_memory_p3s_setup(void)
{
	unsigned int offset;

	test_buffer = alloc_pages_exact(TEST_BUFFER_SIZE, GFP_KERNEL | __GFP_ZERO);
	if (!test_buffer)
		return -ENOMEM;
	for (offset = 0; offset < TEST_BUFFER_SIZE; offset += USER_PAGE_SIZE)
		memset(test_buffer + offset, 0x71 + offset / USER_PAGE_SIZE,
		       USER_PAGE_SIZE);
	return 0;
}

static void vm_iomap_memory_p3s_teardown(void)
{
	free_pages_exact(test_buffer, TEST_BUFFER_SIZE);
}

P3S_MISC_MODULE("vm_iomap_memory_p3s", &vm_iomap_memory_p3s_fops, 0,
		 vm_iomap_memory_p3s_setup, vm_iomap_memory_p3s_teardown,
		 "P3S vm_iomap_memory regression test helper");
