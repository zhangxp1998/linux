// SPDX-License-Identifier: GPL-2.0
/*
 * Generic PCI resource mmap helper
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/pci.h>

#include "pci.h"

#ifdef ARCH_GENERIC_PCI_MMAP_RESOURCE

static const struct vm_operations_struct pci_phys_vm_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys,
#endif
};

int pci_mmap_resource_range(struct pci_dev *pdev, int bar,
			    struct vm_area_struct *vma,
			    enum pci_mmap_state mmap_state, int write_combine)
{
	resource_size_t offset = vma_file_offset(vma);
	resource_size_t size = pci_resource_len(pdev, bar);
	resource_size_t map_end = ALIGN(size, MM_PAGE_SIZE(vma->vm_mm));
	unsigned long map_size = vma->vm_end - vma->vm_start;
	int ret;

	/* Like the page-count check this replaces, allow a partial last page. */
	if (offset > map_end || map_size > map_end - offset)
		return -EINVAL;

	if (write_combine)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	else
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);

	vma->vm_ops = &pci_phys_vm_ops;

	if (mmap_state == pci_mmap_io) {
		ret = pci_iobar_pfn(pdev, bar, vma);
		if (ret)
			return ret;
		return io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
					  map_size, vma->vm_page_prot);
	}

	return vm_iomap_memory(vma, pci_resource_start(pdev, bar), size);
}

#endif

#if (defined(CONFIG_SYSFS) || defined(CONFIG_PROC_FS)) && \
    (defined(HAVE_PCI_MMAP) || defined(ARCH_GENERIC_PCI_MMAP_RESOURCE))

int pci_mmap_fits(struct pci_dev *pdev, int resno, struct vm_area_struct *vma,
		  enum pci_mmap_api mmap_api)
{
	resource_size_t pci_start = 0, pci_end;
	resource_size_t start, size;
	unsigned long nr;

	if (pci_resource_len(pdev, resno) == 0)
		return 0;
	nr = vma->vm_end - vma->vm_start;
	start = vma_file_offset(vma);
	size = ALIGN(pci_resource_len(pdev, resno), MM_PAGE_SIZE(vma->vm_mm));
	if (mmap_api == PCI_MMAP_PROCFS) {
		pci_resource_to_user(pdev, resno, &pdev->resource[resno],
				     &pci_start, &pci_end);
		pci_start &= MM_PAGE_MASK(vma->vm_mm);
	}
	if (start < pci_start || start - pci_start > size)
		return 0;
	return nr <= size - (start - pci_start);
}

#endif
