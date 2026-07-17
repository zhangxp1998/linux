/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Private helpers for Per-Process Page Size (PPPS) support inside mm/
 */
#ifndef MM_PPPS_H
#define MM_PPPS_H

#include "vma.h"

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
int ppps_vm_insert_pages(struct vm_area_struct *vma, unsigned long addr,
			 struct page **pages, unsigned long *num);
#else
static inline int ppps_vm_insert_pages(struct vm_area_struct *vma,
		unsigned long addr, struct page **pages, unsigned long *num)
{
	return -EINVAL;
}
#endif


/*
 * VMA merge geometry.  vm_pgoff advances by one native page per
 * PPPS_SLICES_PER_PAGE compat pages once the slice offset is folded in, so a
 * file VMA that ends mid-page is followed by a VMA with a non-zero slice.
 */
static inline pgoff_t vma_pgoff_span(const struct vm_area_struct *vma)
{
	unsigned long pages = vma_pages(vma);

	if (ppps_vma_has_slices(vma))
		pages = (pages + vma_slice_off(vma)) >> PPPS_SLICE_SHIFT;
	return pages;
}

static inline unsigned long vmg_pages(const struct vma_merge_struct *vmg)
{
	return (vmg->end - vmg->start) >> MM_PAGE_SHIFT(vmg->mm);
}

static inline pgoff_t vmg_pgoff_span(const struct vma_merge_struct *vmg)
{
	unsigned long pages = vmg_pages(vmg);

	if (ppps_mm_is_compat(vmg->mm) && vmg->file)
		pages = (pages + vmg->slice_off) >> PPPS_SLICE_SHIFT;
	return pages;
}

static inline bool vmg_can_merge_offsets(const struct vma_merge_struct *vmg,
					 bool merge_next)
{
	bool sliced = ppps_mm_is_compat(vmg->mm) && vmg->file;

	if (merge_next) {
		if (vmg->next->vm_pgoff != vmg->pgoff + vmg_pgoff_span(vmg))
			return false;
		return !sliced || vma_slice_off(vmg->next) ==
			((vmg->slice_off + vmg_pages(vmg)) & PPPS_SLICE_MASK);
	}

	if (vmg->prev->vm_pgoff + vma_pgoff_span(vmg->prev) != vmg->pgoff)
		return false;
	return !sliced || vmg->slice_off ==
		((vma_slice_off(vmg->prev) + vma_pages(vmg->prev)) &
		 PPPS_SLICE_MASK);
}

/*
 * Split a user mmap offset (in process pages) into the page-cache index and
 * the slice within that native page.  Private anonymous compat mappings keep
 * vm_pgoff in process pages and carry no slice.
 */
static inline pgoff_t ppps_split_mmap_pgoff(struct mm_struct *mm, pgoff_t pgoff,
					    vm_flags_t vm_flags,
					    struct file *file,
					    unsigned int *slice)
{
	*slice = 0;
	if (!ppps_mm_is_compat(mm) || !(file || (vm_flags & VM_SHARED)))
		return pgoff;
	*slice = pgoff & PPPS_SLICE_MASK;
	return pgoff >> PPPS_SLICE_SHIFT;
}

#endif /* MM_PPPS_H */
