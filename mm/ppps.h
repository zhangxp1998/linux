/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Private helpers for Per-Process Page Size (PPPS) support inside mm/
 */
#ifndef MM_PPPS_H
#define MM_PPPS_H

#include "vma.h"

enum ppps_anon_wp_type {
	PPPS_ANON_WP_NONE,
	PPPS_ANON_WP_PACKED,
	PPPS_ANON_WP_ZERO,
};

struct ppps_anon_swapin {
	struct folio *folio;
	unsigned int slice;
	bool active;
	bool mapped;
	pte_t ptes[PPPS_SLICES_PER_PAGE];
};

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE

bool ppps_anon_pte_is_packed(struct vm_area_struct *vma,
			     struct folio *folio, unsigned long address,
			     pte_t *ptep);
int ppps_swap_pte_batch(pte_t *ptep, int max_nr, pte_t first,
			unsigned long address);
int ppps_anon_copy_present_ptes(struct vm_area_struct *dst_vma,
				struct vm_area_struct *src_vma,
				pte_t *dst_pte, pte_t *src_pte,
				unsigned long addr, int max_nr, int *rss,
				struct folio *folio, struct folio **prealloc);
enum ppps_anon_wp_type ppps_anon_wp_type(struct vm_area_struct *vma,
					 struct folio *folio,
					 unsigned long address, pte_t *ptep);
bool ppps_anon_wp_can_reuse(struct folio *folio,
			    struct vm_area_struct *vma);
vm_fault_t ppps_anon_wp_copy(struct vm_fault *vmf, struct folio *folio,
			     enum ppps_anon_wp_type type);
bool ppps_anon_fault_can_pack(struct vm_fault *vmf);
void ppps_anon_add_new_rmap(struct folio *folio,
			    struct vm_area_struct *vma,
			    unsigned long address, int nr_ptes);
void ppps_anon_remove_rmap(struct folio *folio,
			   struct vm_area_struct *vma);
int ppps_anon_try_to_unmap(struct folio *folio,
			   struct vm_area_struct *vma,
			   unsigned long address, pte_t *ptep);
unsigned long ppps_anon_folio_nr_pages(struct folio *folio);
void ppps_anon_swapin_init(struct ppps_anon_swapin *swapin,
			   struct vm_fault *vmf);
bool ppps_anon_swapin_active(const struct ppps_anon_swapin *swapin);
vm_fault_t ppps_anon_swapin_prepare(struct ppps_anon_swapin *swapin,
				    struct vm_fault *vmf,
				    struct folio *swapcache);
bool ppps_anon_swapin_revalidate(struct ppps_anon_swapin *swapin,
				 struct vm_fault *vmf, swp_entry_t entry,
				 struct folio **folio, struct page **page);
bool ppps_anon_swapin_map(struct ppps_anon_swapin *swapin,
			  struct vm_fault *vmf, struct folio *swapcache,
			  struct folio *folio, swp_entry_t entry,
			  unsigned long *address, pte_t **ptep,
			  int *nr_pages);
void ppps_anon_swapin_cleanup(struct ppps_anon_swapin *swapin);

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

/*
 * No-op fallbacks so callers don't need #ifdef guards: every call site is
 * predicated (directly or transitively) on ppps_mm_is_compat(), which is
 * constant false here, so the compiler discards these entirely.
 */
static inline bool ppps_anon_pte_is_packed(struct vm_area_struct *vma,
					   struct folio *folio,
					   unsigned long address, pte_t *ptep)
{
	return false;
}

static inline int ppps_swap_pte_batch(pte_t *ptep, int max_nr, pte_t first,
				      unsigned long address)
{
	return 0;
}

static inline int ppps_anon_copy_present_ptes(struct vm_area_struct *dst_vma,
					      struct vm_area_struct *src_vma,
					      pte_t *dst_pte, pte_t *src_pte,
					      unsigned long addr, int max_nr,
					      int *rss, struct folio *folio,
					      struct folio **prealloc)
{
	return 0;
}

static inline enum ppps_anon_wp_type
ppps_anon_wp_type(struct vm_area_struct *vma, struct folio *folio,
		  unsigned long address, pte_t *ptep)
{
	return PPPS_ANON_WP_NONE;
}

static inline bool ppps_anon_wp_can_reuse(struct folio *folio,
					  struct vm_area_struct *vma)
{
	return false;
}

static inline vm_fault_t ppps_anon_wp_copy(struct vm_fault *vmf,
					   struct folio *folio,
					   enum ppps_anon_wp_type type)
{
	return 0;
}

static inline bool ppps_anon_fault_can_pack(struct vm_fault *vmf)
{
	return false;
}

static inline void ppps_anon_add_new_rmap(struct folio *folio,
					  struct vm_area_struct *vma,
					  unsigned long address, int nr_ptes)
{
}

static inline void ppps_anon_remove_rmap(struct folio *folio,
					 struct vm_area_struct *vma)
{
}

static inline int ppps_anon_try_to_unmap(struct folio *folio,
					 struct vm_area_struct *vma,
					 unsigned long address, pte_t *ptep)
{
	return 0;
}

static inline unsigned long ppps_anon_folio_nr_pages(struct folio *folio)
{
	return 0;
}

static inline void ppps_anon_swapin_init(struct ppps_anon_swapin *swapin,
					 struct vm_fault *vmf)
{
}

static inline bool ppps_anon_swapin_active(const struct ppps_anon_swapin *swapin)
{
	return false;
}

static inline vm_fault_t ppps_anon_swapin_prepare(struct ppps_anon_swapin *swapin,
						  struct vm_fault *vmf,
						  struct folio *swapcache)
{
	return 0;
}

static inline bool ppps_anon_swapin_revalidate(struct ppps_anon_swapin *swapin,
					       struct vm_fault *vmf,
					       swp_entry_t entry,
					       struct folio **folio,
					       struct page **page)
{
	return true;
}

static inline bool ppps_anon_swapin_map(struct ppps_anon_swapin *swapin,
					struct vm_fault *vmf,
					struct folio *swapcache,
					struct folio *folio, swp_entry_t entry,
					unsigned long *address, pte_t **ptep,
					int *nr_pages)
{
	return false;
}

static inline void ppps_anon_swapin_cleanup(struct ppps_anon_swapin *swapin)
{
}

static inline pgoff_t vma_native_pages(const struct vm_area_struct *vma)
{
	return (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
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
