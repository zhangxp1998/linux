/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Private helpers for Per-Process Page Size (PPPS) support inside mm/
 */
#ifndef MM_PPPS_H
#define MM_PPPS_H

#include "vma.h"

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE

/*
 * A packed anonymous mapping uses one native folio as four consecutive
 * process-page PTEs.  The invariant is deliberately strict: a mapping
 * instance is either this complete tuple, or a singleton mapping of slice 0.
 * Keeping tuple recognition in one helper avoids subtly different tests in
 * fault, fork, COW and rmap paths.
 */
static inline bool ppps_anon_pte_is_packed(struct vm_area_struct *vma,
					   struct folio *folio,
					   unsigned long address, pte_t *ptep)
{
	unsigned long base;
	unsigned int i, slice;
	pte_t *base_ptep;

	if (!ppps_mm_is_compat(vma->vm_mm) || !vma_is_anonymous(vma) ||
	    !folio_test_anon(folio) || folio_test_large(folio) ||
	    !folio_test_ppps_packed_anon(folio))
		return false;

	base = ALIGN_DOWN(address, PAGE_SIZE);
	if (base < vma->vm_start || base + PAGE_SIZE > vma->vm_end)
		return false;

	slice = (address - base) >> PAGE_SHIFT_COMPAT;
	base_ptep = ptep - slice;
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		pte_t pte = ptep_get(base_ptep + i);

		if (!pte_present(pte) || pte_special(pte) ||
		    pte_page(pte) != &folio->page ||
		    pte_page_offset(pte) != i * PAGE_SIZE_COMPAT)
			return false;
	}

	return true;
}

/*
 * A zero tuple maps the four slices of the native global zero page.  Unlike
 * a packed anonymous folio, it owns no rmap or page-table references and can
 * be split freely.  Recognizing the complete shape lets a write fault replace
 * it atomically with one private packed folio.
 */
static inline bool ppps_anon_pte_is_zero_tuple(struct vm_area_struct *vma,
					       unsigned long address, pte_t *ptep)
{
	unsigned long base;
	unsigned int i, slice;
	pte_t *base_ptep;

	if (!ppps_mm_is_compat(vma->vm_mm) || !vma_is_anonymous(vma))
		return false;

	base = ALIGN_DOWN(address, PAGE_SIZE);
	if (base < vma->vm_start || base + PAGE_SIZE > vma->vm_end)
		return false;

	slice = (address - base) >> PAGE_SHIFT_COMPAT;
	base_ptep = ptep - slice;
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		pte_t pte = ptep_get(base_ptep + i);

		if (!pte_present(pte) || !pte_special(pte) || pte_write(pte) ||
		    !is_zero_pfn(pte_pfn(pte)) ||
		    pte_page_offset(pte) != i * PAGE_SIZE_COMPAT)
			return false;
	}

	return true;
}

/*
 * A packed swap tuple stores the same swap offset in all four process PTEs,
 * unlike the consecutive offsets handled by swap_pte_batch().  Batch the
 * identical entries so callers release every swap reference while accounting
 * the tuple only once at its native-page-aligned first PTE.
 */
static inline int ppps_swap_pte_batch(pte_t *ptep, int max_nr, pte_t first,
				      unsigned long address)
{
	swp_entry_t entry;
	unsigned int slice;
	int nr = 1;

	if (!pte_swp_ppps_packed(first))
		return 0;

	entry = pte_to_swp_entry(first);
	slice = (address & ~PAGE_MASK) >> PAGE_SHIFT_COMPAT;
	max_nr = min_t(int, max_nr, PPPS_SLICES_PER_PAGE - slice);
	while (nr < max_nr) {
		pte_t pte = ptep_get(ptep + nr);

		if (!is_swap_pte(pte) || !pte_swp_ppps_packed(pte) ||
		    pte_to_swp_entry(pte).val != entry.val)
			break;
		nr++;
	}

	return nr;
}

static inline pgoff_t vma_native_pages(const struct vm_area_struct *vma)
{
	bool is_compat = ppps_mm_is_compat(vma->vm_mm);
	bool is_anon = !vma->vm_ops;
	unsigned long nr_slices = vma_pages(vma);

	if (!is_compat || is_anon) {
		/*
		 * For native processes and compat anonymous mappings (which do not track
		 * subpage slices), the number of slices corresponds directly to the number
		 * of host pages.
		 */
		return nr_slices;
	}

	/*
	 * For file-backed VMAs in compat processes, calculate the total host pages
	 * needed to cover the range (including starting slice offset alignment).
	 */
	return (vma_slice_off(vma) + nr_slices) >> PPPS_SLICE_SHIFT;
}

static inline unsigned int vma_slice_offset(struct vm_area_struct *vma,
					   unsigned long addr)
{
	unsigned long total_slices;

	if (!ppps_mm_is_compat(vma->vm_mm))
		return 0;

	if (vma_is_anonymous(vma))
		return 0;

	total_slices = ((addr - vma->vm_start) >> PAGE_SHIFT_COMPAT) +
				vma_slice_off(vma);

	return total_slices & PPPS_SLICE_MASK;
}

static inline unsigned long vmg_pages(const struct vma_merge_struct *vmg)
{
	return (vmg->end - vmg->start) >> MM_PAGE_SHIFT(vmg->mm);
}

static inline pgoff_t vmg_native_pages(const struct vma_merge_struct *vmg)
{
	bool is_compat = ppps_mm_is_compat(vmg->mm);
	bool is_anon = !vmg->file;
	unsigned long nr_slices = vmg_pages(vmg);

	if (!is_compat || is_anon)
		return nr_slices;

	return (vmg->slice_off + nr_slices) >> PPPS_SLICE_SHIFT;
}

static inline bool vmg_can_merge_offsets(const struct vma_merge_struct *vmg,
					 bool merge_next)
{
	bool is_compat = ppps_mm_is_compat(vmg->mm);
	bool is_anon = !vmg->file;

	if (merge_next) {
		pgoff_t pglen = vmg_native_pages(vmg);

		/* Verify native page cache index alignment */
		if (vmg->next->vm_pgoff != vmg->pgoff + pglen)
			return false;

		/* Non-compat or anonymous mappings don't track subpage slices */
		if (!is_compat || is_anon)
			return true;

		/* Verify compat subpage slice alignment */
		return vma_slice_off(vmg->next) ==
		       ((vmg->slice_off + vmg_pages(vmg)) & PPPS_SLICE_MASK);
	} else {
		pgoff_t pglen = vma_native_pages(vmg->prev);

		/* Verify native page cache index alignment */
		if (vmg->prev->vm_pgoff + pglen != vmg->pgoff)
			return false;

		/* Non-compat or anonymous mappings don't track subpage slices */
		if (!is_compat || is_anon)
			return true;

		/* Verify compat subpage slice alignment */
		return vmg->slice_off ==
		       ((vma_slice_off(vmg->prev) + vma_pages(vmg->prev)) & PPPS_SLICE_MASK);
	}
}

static inline pgoff_t mmap_pgoff_offset(struct mm_struct *mm, pgoff_t pgoff,
					vm_flags_t vm_flags, struct file *file)
{
	/* Native processes do not use subpage slice offsets. */
	if (!ppps_mm_is_compat(mm))
		return pgoff;

	/*
	 * File-backed and shared anonymous mappings must align with host page cache
	 * boundaries. Scale the starting offset (in compat pages) to host pages.
	 * Private anonymous mappings remain in compat page units.
	 */
	if (file || (vm_flags & VM_SHARED))
		return pgoff >> PPPS_SLICE_SHIFT;

	return pgoff;
}

static inline unsigned int mmap_slice_offset(struct mm_struct *mm,
					     pgoff_t pgoff,
					     vm_flags_t vm_flags,
					     struct file *file)
{
	/* For native processes, slice offsets are not used (always 0). */
	if (!ppps_mm_is_compat(mm))
		return 0;

	/*
	 * Extract the subpage slice alignment remainder of the user-supplied
	 * offset relative to the host page boundary for file-backed and shared
	 * anonymous mappings. Private anonymous mappings do not track slices.
	 */
	if (file || (vm_flags & VM_SHARED))
		return pgoff & PPPS_SLICE_MASK;

	return 0;
}

#else /* !CONFIG_ARM64_PER_PROCESS_PAGE_SIZE */

static inline bool ppps_anon_pte_is_packed(struct vm_area_struct *vma,
					   struct folio *folio,
					   unsigned long address, pte_t *ptep)
{
	return false;
}

static inline bool ppps_anon_pte_is_zero_tuple(struct vm_area_struct *vma,
					       unsigned long address, pte_t *ptep)
{
	return false;
}

static inline int ppps_swap_pte_batch(pte_t *ptep, int max_nr, pte_t first,
				      unsigned long address)
{
	return 0;
}

static inline pgoff_t vma_native_pages(const struct vm_area_struct *vma)
{
	return (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
}

static inline unsigned int vma_slice_offset(struct vm_area_struct *vma,
					   unsigned long addr)
{
	return 0;
}

static inline unsigned long vmg_pages(const struct vma_merge_struct *vmg)
{
	return (vmg->end - vmg->start) >> PAGE_SHIFT;
}

static inline pgoff_t vmg_native_pages(const struct vma_merge_struct *vmg)
{
	return vmg_pages(vmg);
}

static inline bool vmg_can_merge_offsets(const struct vma_merge_struct *vmg,
					 bool merge_next)
{
	if (merge_next) {
		pgoff_t pglen = vmg_native_pages(vmg);

		return vmg->next->vm_pgoff == vmg->pgoff + pglen;
	}

	pgoff_t pglen = vma_native_pages(vmg->prev);

	return vmg->prev->vm_pgoff + pglen == vmg->pgoff;
}

static inline pgoff_t mmap_pgoff_offset(struct mm_struct *mm, pgoff_t pgoff,
					vm_flags_t vm_flags, struct file *file)
{
	return pgoff;
}

static inline unsigned int mmap_slice_offset(struct mm_struct *mm,
					     pgoff_t pgoff,
					     vm_flags_t vm_flags,
					     struct file *file)
{
	return 0;
}

#endif /* CONFIG_ARM64_PER_PROCESS_PAGE_SIZE */

#endif /* MM_PPPS_H */
