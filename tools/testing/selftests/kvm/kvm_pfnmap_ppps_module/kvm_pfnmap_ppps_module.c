// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/numa.h>
#include <linux/sizes.h>

#define DEVICE_NAME "kvm-pfnmap-ppps"
#define USER_OFFSET SZ_4K
#define PFNMAP_SIZE SZ_32M
#define TEST_MAGIC 0x4b564d50464e4d50ULL

static struct page *allocated_pages;
static struct page *magic_page;
static unsigned long allocated_nr_pages;

static vm_fault_t kvm_pfnmap_fault(struct vm_fault *vmf)
{
	unsigned long offset = vmf->address - vmf->vma->vm_start;
	struct page *page;

	if (offset >= SZ_16K && offset < SZ_16K + PAGE_SIZE)
		page = magic_page;
	else
		page = allocated_pages + (offset >> PAGE_SHIFT);

	return vmf_insert_pfn(vmf->vma, vmf->address, page_to_pfn(page));
}

static const struct vm_operations_struct kvm_pfnmap_vm_ops = {
	.fault = kvm_pfnmap_fault,
};

static int kvm_pfnmap_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (vma_file_offset(vma) != USER_OFFSET ||
	    vma->vm_end - vma->vm_start != PFNMAP_SIZE)
		return -EINVAL;

	vma->vm_ops = &kvm_pfnmap_vm_ops;
	vm_flags_set(vma, VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	return 0;
}

static const struct file_operations kvm_pfnmap_fops = {
	.owner = THIS_MODULE,
	.mmap = kvm_pfnmap_mmap,
};

static struct miscdevice kvm_pfnmap_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME,
	.fops = &kvm_pfnmap_fops,
	.mode = 0600,
};

static void kvm_pfnmap_free_pages(void)
{
	unsigned long i;

	if (magic_page) {
		ClearPageReserved(magic_page);
		__free_page(magic_page);
		magic_page = NULL;
	}
	if (allocated_pages) {
		for (i = 0; i < allocated_nr_pages; i++)
			ClearPageReserved(allocated_pages + i);
		free_contig_range(page_to_pfn(allocated_pages),
				  allocated_nr_pages);
		allocated_pages = NULL;
	}
}

static int __init kvm_pfnmap_init(void)
{
	u64 *data;
	unsigned long i, j;
	int ret;

	allocated_nr_pages = PFNMAP_SIZE / PAGE_SIZE;
	allocated_pages = alloc_contig_pages(allocated_nr_pages, GFP_KERNEL,
					     numa_node_id(),
					     &node_states[N_MEMORY]);
	if (!allocated_pages)
		return -ENOMEM;

	for (i = 0; i < allocated_nr_pages; i++) {
		SetPageReserved(allocated_pages + i);
		data = kmap_local_page(allocated_pages + i);
		for (j = 0; j < PAGE_SIZE / sizeof(*data); j++)
			data[j] = 0;
		kunmap_local(data);
	}

	magic_page = alloc_page(GFP_KERNEL);
	if (!magic_page) {
		kvm_pfnmap_free_pages();
		return -ENOMEM;
	}
	SetPageReserved(magic_page);
	data = kmap_local_page(magic_page);
	for (j = 0; j < PAGE_SIZE / sizeof(*data); j++)
		data[j] = TEST_MAGIC;
	kunmap_local(data);

	ret = misc_register(&kvm_pfnmap_device);
	if (ret)
		kvm_pfnmap_free_pages();
	return ret;
}

static void __exit kvm_pfnmap_exit(void)
{
	misc_deregister(&kvm_pfnmap_device);
	kvm_pfnmap_free_pages();
}

module_init(kvm_pfnmap_init);
module_exit(kvm_pfnmap_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PPPS KVM PFNMAP regression fixture");
