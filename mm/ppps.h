/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Private helpers for Per-Process Page Size (PPPS) support inside mm/
 */
#ifndef MM_PPPS_H
#define MM_PPPS_H

#include <linux/pagemap.h>
#include <linux/userfaultfd_k.h>

#include "vma.h"

struct page_vma_mapped_walk;
struct ppps_mremap_folios;

/* Only the PPPS implementation reads or writes these walk/fault states. */
struct ppps_anon_unmap_ctx {
#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
	bool compat;
	bool exclusive;
	bool shared;
	bool last;
#endif
};

struct ppps_anon_swapin_ctx {
#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
	bool compat;
	bool owner;
	unsigned int siblings;
#endif
};

/* Which PPPS-specific copy do_wp_page() must make instead of wp_page_copy(). */
enum ppps_anon_wp_type {
	PPPS_ANON_WP_NONE,
	PPPS_ANON_WP_COMPAT,	/* Copy the whole packed tuple. */
	PPPS_ANON_WP_ZERO,	/* Zero page inside a compat tuple. */
	PPPS_ANON_WP_FILE_COW,	/* Private file page joining a tuple. */
};

/*
 * Carry the access, dirty, soft-dirty and uffd-wp state of @old over to
 * @new.  Write permission is decided by the caller.
 */
#ifdef CONFIG_MMU
static inline pte_t ppps_pte_inherit(pte_t new, pte_t old)
{
	if (pte_young(old))
		new = pte_mkyoung(new);
	if (pte_dirty(old))
		new = pte_mkdirty(new);
	if (pte_soft_dirty(old))
		new = pte_mksoft_dirty(new);
	if (pte_uffd_wp(old))
		new = pte_mkuffd_wp(new);
	return new;
}
#endif

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE

/* Tuple geometry. */
/* Return zero to use the generic walk end (also for native mms). */
unsigned long ppps_pvmw_walk_end(struct page_vma_mapped_walk *pvmw);
bool ppps_vma_shares_tuple(struct vm_area_struct *vma);
bool ppps_vma_address_shares_tuple(struct vm_area_struct *vma,
				   unsigned long address);
pgoff_t ppps_tuple_index(struct vm_area_struct *vma, unsigned long address);

/*
 * Tuple slice accounting, all under the PTE lock.  The present slices of one
 * packed folio inside a (mm, tuple) share a single rmap entry, folio reference
 * and RSS charge: the first slice installed takes them, the last slice removed
 * releases them.  Migration entries of the folio keep only the RSS charge.
 */
bool ppps_anon_slice_takes_ownership(struct vm_area_struct *vma, struct folio *folio,
			   pte_t *ptep, unsigned long address);
bool ppps_anon_slice_last(struct vm_area_struct *vma, struct folio *folio,
			  pte_t *ptep, unsigned long address);
bool ppps_anon_slice_unmap(struct vm_area_struct *vma, struct folio *folio,
			   pte_t *ptep, unsigned long address);
bool ppps_anon_folio_has_other_entries(struct vm_area_struct *vma,
		struct folio *folio, pte_t *ptep, unsigned long address);

/* Tuple queries under the PTE lock. */
bool ppps_anon_tuple_is_complete(struct vm_area_struct *vma,
			     struct folio *folio, unsigned long address,
			     pte_t *ptep);
bool ppps_anon_tuple_within(struct vm_area_struct *vma, struct folio *folio,
			    pte_t *ptep, unsigned long address,
			    unsigned long start, unsigned long end);
int ppps_anon_installable_run(struct vm_area_struct *vma, pte_t *ptep,
			      unsigned long address, unsigned long *base);

/* The one decision point for writing into a packed folio in place. */
bool ppps_anon_try_reuse_folio(struct folio *folio, struct vm_area_struct *vma);

/* Hole filling: reuse an exclusive tuple folio for a missing slice. */
bool ppps_anon_tuple_has_folio_hint(struct vm_fault *vmf);
struct folio *ppps_anon_hole_fill_folio(struct vm_area_struct *vma,
					pte_t *ptep, unsigned long address,
					bool *multi_folio);
void ppps_anon_clear_slice(struct vm_area_struct *vma, struct folio *folio,
			   unsigned long address);
void ppps_anon_copy_slice(struct folio *dst, unsigned int dst_slice,
			  struct folio *src, unsigned int src_slice);
void ppps_anon_fill_slice_from(struct folio *dst,
		struct vm_area_struct *vma, unsigned long address,
		struct page *src);

/* Fault and rmap operation contracts are documented in ppps_anon.c. */
void ppps_anon_unmap_begin(struct ppps_anon_unmap_ctx *ctx,
		struct vm_area_struct *vma, struct folio *folio,
		unsigned long *address, unsigned long *end);
bool ppps_anon_unmap_sample(struct ppps_anon_unmap_ctx *ctx,
		struct folio *folio, struct page *subpage,
		struct page_vma_mapped_walk *pvmw);
void ppps_anon_unmap_clear(struct ppps_anon_unmap_ctx *ctx,
		struct vm_area_struct *vma, struct folio *folio,
		pte_t *ptep, unsigned long address);
bool ppps_anon_unmap_last(const struct ppps_anon_unmap_ctx *ctx);
bool ppps_anon_unmap_can_batch(const struct ppps_anon_unmap_ctx *ctx);
bool ppps_anon_unmap_needs_share(const struct ppps_anon_unmap_ctx *ctx);
void ppps_anon_unmap_commit_share(struct ppps_anon_unmap_ctx *ctx);
void ppps_anon_swapin_begin(struct ppps_anon_swapin_ctx *ctx,
		struct vm_area_struct *vma, struct folio *folio,
		pte_t *ptep, unsigned long address, swp_entry_t entry);
void ppps_anon_swapin_release(const struct ppps_anon_swapin_ctx *ctx,
		swp_entry_t entry, bool *exclusive);
bool ppps_anon_swapin_takes_ownership(const struct ppps_anon_swapin_ctx *ctx);
unsigned int ppps_anon_swapin_swap_delta(const struct ppps_anon_swapin_ctx *ctx);
void ppps_anon_swapin_install(const struct ppps_anon_swapin_ctx *ctx,
		struct vm_area_struct *vma, struct page *page, pte_t *ptep,
		unsigned long address, pte_t pte);
bool ppps_anon_fault_anon_prepare(struct vm_fault *vmf, struct folio **foliop,
		int *nr_pages, unsigned long *addr);
bool ppps_anon_file_cow_no_prealloc(struct vm_fault *vmf);
bool ppps_anon_fault_file_cow(struct vm_fault *vmf, struct folio **foliop,
		vm_fault_t *ret);

/* Generic MM hooks. */
struct ppps_mremap_folios *ppps_anon_mremap_prepare(struct vm_area_struct *vma,
		unsigned long old_addr, unsigned long new_addr, unsigned long len);
void ppps_anon_mremap_finish(struct ppps_mremap_folios *ctx);
int ppps_anon_reslice_range(struct mm_struct *mm, unsigned long old_addr,
			     unsigned long new_addr, unsigned long len);
int ppps_anon_copy_present_ptes(struct vm_area_struct *dst_vma,
				struct vm_area_struct *src_vma,
				pte_t *dst_pte, pte_t *src_pte,
				unsigned long addr, int *rss,
				struct folio *folio, struct folio **prealloc);
enum ppps_anon_wp_type ppps_anon_wp_type(struct vm_area_struct *vma,
					 struct folio *folio,
					 unsigned long address, pte_t pte);
vm_fault_t ppps_anon_wp_copy(struct vm_fault *vmf, struct folio *folio,
			     enum ppps_anon_wp_type type);
int ppps_vm_insert_pages(struct vm_area_struct *vma, unsigned long addr,
			 struct page **pages, unsigned long *num);

#else /* !CONFIG_ARM64_PER_PROCESS_PAGE_SIZE */

static inline unsigned long
ppps_pvmw_walk_end(struct page_vma_mapped_walk *pvmw)
{
	return 0;
}

static inline void ppps_anon_unmap_begin(struct ppps_anon_unmap_ctx *ctx,
		struct vm_area_struct *vma, struct folio *folio,
		unsigned long *address, unsigned long *end)
{
}

static inline bool ppps_anon_unmap_sample(struct ppps_anon_unmap_ctx *ctx,
		struct folio *folio, struct page *subpage,
		struct page_vma_mapped_walk *pvmw)
{
	return folio_test_anon(folio) && PageAnonExclusive(subpage);
}

static inline void ppps_anon_unmap_clear(struct ppps_anon_unmap_ctx *ctx,
		struct vm_area_struct *vma, struct folio *folio,
		pte_t *ptep, unsigned long address)
{
}

static inline bool ppps_anon_unmap_last(const struct ppps_anon_unmap_ctx *ctx)
{
	return true;
}

static inline bool ppps_anon_unmap_can_batch(const struct ppps_anon_unmap_ctx *ctx)
{
	return true;
}

static inline bool ppps_anon_unmap_needs_share(const struct ppps_anon_unmap_ctx *ctx)
{
	return true;
}

static inline void ppps_anon_unmap_commit_share(struct ppps_anon_unmap_ctx *ctx)
{
}

static inline void ppps_anon_swapin_begin(struct ppps_anon_swapin_ctx *ctx,
		struct vm_area_struct *vma, struct folio *folio,
		pte_t *ptep, unsigned long address, swp_entry_t entry)
{
}

static inline void ppps_anon_swapin_release(const struct ppps_anon_swapin_ctx *ctx,
		swp_entry_t entry, bool *exclusive)
{
}

static inline bool ppps_anon_swapin_takes_ownership(const struct ppps_anon_swapin_ctx *ctx)
{
	return true;
}

static inline unsigned int ppps_anon_swapin_swap_delta(const struct ppps_anon_swapin_ctx *ctx)
{
	return 0;
}

static inline void ppps_anon_swapin_install(const struct ppps_anon_swapin_ctx *ctx,
		struct vm_area_struct *vma, struct page *page, pte_t *ptep,
		unsigned long address, pte_t pte)
{
}

static inline bool ppps_anon_fault_anon_prepare(struct vm_fault *vmf, struct folio **foliop,
		int *nr_pages, unsigned long *addr)
{
	return false;
}

static inline bool ppps_anon_file_cow_no_prealloc(struct vm_fault *vmf)
{
	return false;
}

static inline bool ppps_anon_fault_file_cow(struct vm_fault *vmf, struct folio **foliop,
		vm_fault_t *ret)
{
	return false;
}

/*
 * Native fallbacks: events are unhandled, preparation does not reuse,
 * ownership predicates are true, and extra sibling work is empty.
 */
static inline bool ppps_vma_shares_tuple(struct vm_area_struct *vma)
{
	return false;
}

static inline bool ppps_vma_address_shares_tuple(struct vm_area_struct *vma,
		unsigned long address)
{
	return false;
}

static inline pgoff_t ppps_tuple_index(struct vm_area_struct *vma,
		unsigned long address)
{
	return linear_page_index(vma, address);
}

static inline bool ppps_anon_slice_takes_ownership(struct vm_area_struct *vma,
		struct folio *folio, pte_t *ptep, unsigned long address)
{
	return true;
}

static inline bool ppps_anon_slice_last(struct vm_area_struct *vma,
		struct folio *folio, pte_t *ptep, unsigned long address)
{
	return true;
}

static inline bool ppps_anon_slice_unmap(struct vm_area_struct *vma,
		struct folio *folio, pte_t *ptep, unsigned long address)
{
	return true;
}

static inline bool ppps_anon_try_reuse_folio(struct folio *folio,
					 struct vm_area_struct *vma)
{
	return false;
}

static inline bool ppps_anon_folio_has_other_entries(struct vm_area_struct *vma,
		struct folio *folio, pte_t *ptep, unsigned long address)
{
	return false;
}

static inline bool ppps_anon_tuple_is_complete(struct vm_area_struct *vma,
					   struct folio *folio,
					   unsigned long address, pte_t *ptep)
{
	return false;
}

static inline bool ppps_anon_tuple_within(struct vm_area_struct *vma,
		struct folio *folio, pte_t *ptep, unsigned long address,
		unsigned long start, unsigned long end)
{
	return true;
}

static inline int ppps_anon_installable_run(struct vm_area_struct *vma,
					    pte_t *ptep,
					    unsigned long address,
					    unsigned long *base)
{
	*base = address;
	return 1;
}

static inline bool ppps_anon_tuple_has_folio_hint(struct vm_fault *vmf)
{
	return false;
}

static inline struct folio *
ppps_anon_hole_fill_folio(struct vm_area_struct *vma, pte_t *ptep,
			  unsigned long address, bool *multi_folio)
{
	if (multi_folio)
		*multi_folio = false;
	return NULL;
}

static inline void ppps_anon_clear_slice(struct vm_area_struct *vma,
		struct folio *folio, unsigned long address)
{
}

static inline void ppps_anon_copy_slice(struct folio *dst,
		unsigned int dst_slice, struct folio *src,
		unsigned int src_slice)
{
}

static inline void ppps_anon_fill_slice_from(struct folio *dst,
		struct vm_area_struct *vma, unsigned long address,
		struct page *src)
{
}

static inline struct ppps_mremap_folios *
ppps_anon_mremap_prepare(struct vm_area_struct *vma, unsigned long old_addr,
		unsigned long new_addr, unsigned long len)
{
	return NULL;
}

static inline void ppps_anon_mremap_finish(struct ppps_mremap_folios *ctx)
{
}

static inline int ppps_anon_reslice_range(struct mm_struct *mm,
		unsigned long old_addr, unsigned long new_addr,
		unsigned long len)
{
	return 0;
}

static inline int ppps_anon_copy_present_ptes(struct vm_area_struct *dst_vma,
					      struct vm_area_struct *src_vma,
					      pte_t *dst_pte, pte_t *src_pte,
					      unsigned long addr, int *rss,
					      struct folio *folio,
					      struct folio **prealloc)
{
	return 0;
}

static inline enum ppps_anon_wp_type
ppps_anon_wp_type(struct vm_area_struct *vma, struct folio *folio,
		  unsigned long address, pte_t pte)
{
	return PPPS_ANON_WP_NONE;
}

static inline vm_fault_t ppps_anon_wp_copy(struct vm_fault *vmf,
					   struct folio *folio,
					   enum ppps_anon_wp_type type)
{
	return 0;
}

static inline int ppps_vm_insert_pages(struct vm_area_struct *vma,
				       unsigned long addr, struct page **pages,
				       unsigned long *num)
{
	return -EINVAL;
}

#endif /* CONFIG_ARM64_PER_PROCESS_PAGE_SIZE */

#ifdef CONFIG_USERFAULTFD
#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
bool ppps_uffd_copy_tuple_ok(struct vm_area_struct *dst_vma, unsigned long dst_addr,
		unsigned long remaining, uffd_flags_t flags);
int ppps_uffd_copy_tuple(pmd_t *dst_pmd, struct vm_area_struct *dst_vma, unsigned long dst_addr,
		unsigned long src_addr, struct folio **foliop, bool *tuple_folio);
bool ppps_uffd_move_tuple_ok(struct vm_area_struct *dst_vma, struct vm_area_struct *src_vma,
		unsigned long dst_addr, unsigned long src_addr, unsigned long len);
struct folio *ppps_uffd_move_prealloc(struct mm_struct *mm,
		struct vm_area_struct *dst_vma, unsigned long dst_addr);
long ppps_uffd_move_tuple(struct mm_struct *mm, struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, unsigned long dst_addr, unsigned long src_addr,
		pte_t *dst_pte, pte_t *src_pte, pte_t orig_dst_pte, pte_t orig_src_pte,
		pmd_t *dst_pmd, pmd_t dst_pmdval, spinlock_t *dst_ptl, spinlock_t *src_ptl,
		struct folio **src_foliop, bool *ppps_fallback);
long ppps_uffd_move_slice(struct mm_struct *mm, struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, unsigned long dst_addr, unsigned long src_addr,
		pte_t *dst_pte, pte_t *src_pte, pte_t orig_dst_pte, pte_t orig_src_pte,
		pmd_t *dst_pmd, pmd_t dst_pmdval, spinlock_t *dst_ptl, spinlock_t *src_ptl,
		struct folio *src_folio, struct folio **preallocp);
#else
static inline bool ppps_uffd_copy_tuple_ok(struct vm_area_struct *dst_vma,
		unsigned long dst_addr, unsigned long remaining, uffd_flags_t flags)
{
	return false;
}

static inline int ppps_uffd_copy_tuple(pmd_t *dst_pmd, struct vm_area_struct *dst_vma,
		unsigned long dst_addr, unsigned long src_addr, struct folio **foliop,
		bool *tuple_folio)
{
	return -EOPNOTSUPP;
}

static inline bool ppps_uffd_move_tuple_ok(struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, unsigned long dst_addr, unsigned long src_addr,
		unsigned long len)
{
	return false;
}

static inline struct folio *ppps_uffd_move_prealloc(struct mm_struct *mm,
		struct vm_area_struct *dst_vma, unsigned long dst_addr)
{
	return NULL;
}

static inline long ppps_uffd_move_tuple(struct mm_struct *mm, struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, unsigned long dst_addr, unsigned long src_addr,
		pte_t *dst_pte, pte_t *src_pte, pte_t orig_dst_pte, pte_t orig_src_pte,
		pmd_t *dst_pmd, pmd_t dst_pmdval, spinlock_t *dst_ptl, spinlock_t *src_ptl,
		struct folio **src_foliop, bool *ppps_fallback)
{
	return -EOPNOTSUPP;
}

static inline long ppps_uffd_move_slice(struct mm_struct *mm, struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, unsigned long dst_addr, unsigned long src_addr,
		pte_t *dst_pte, pte_t *src_pte, pte_t orig_dst_pte, pte_t orig_src_pte,
		pmd_t *dst_pmd, pmd_t dst_pmdval, spinlock_t *dst_ptl, spinlock_t *src_ptl,
		struct folio *src_folio, struct folio **preallocp)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_ARM64_PER_PROCESS_PAGE_SIZE */
#endif /* CONFIG_USERFAULTFD */

/* Compare complete offsets; the shared helper owns slice carry arithmetic. */
static inline bool vmg_can_merge_offsets(const struct vma_merge_struct *vmg,
					 bool merge_next)
{
	bool sliced = ppps_mm_is_compat(vmg->mm) && vmg->file;
	struct vma_offset offset = { vmg->pgoff, vmg->slice_off };
	struct vma_offset end, next;

	if (merge_next) {
		end = vma_offset_advance(vmg->mm, offset, sliced,
					 vmg->end - vmg->start);
		next = vma_get_offset(vmg->next);
	} else {
		end = vma_offset_advance(vmg->mm, vma_get_offset(vmg->prev),
					 ppps_vma_has_slices(vmg->prev),
					 vmg->prev->vm_end -
						 vmg->prev->vm_start);
		next = offset;
	}
	return end.pgoff == next.pgoff && (!sliced || end.slice == next.slice);
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
