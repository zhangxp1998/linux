// SPDX-License-Identifier: GPL-2.0-only
/* PPPS tuple and slice operations for userfaultfd. */

#include <linux/highmem.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/ppps.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/userfaultfd_k.h>

#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#include "internal.h"
#include "ppps.h"

/*
 * PPPS: is this mfill step allowed to install a whole native tuple at once?
 *
 * A compat (4K) process backs each of its PTEs with a slice of a native (16K)
 * folio.  The generic mfill loop walks MM_PAGE_SIZE() (4K) at a time and
 * allocates one native folio per step, so every UFFDIO_COPY-installed page
 * becomes a "singleton": a 16K folio with a single 4K PTE on it (4x waste).
 * When userspace hands us at least one whole native page worth of a private
 * anonymous COPY, install all PPPS_SLICES_PER_PAGE slices on one folio
 * instead, exactly like the packed branch of do_anonymous_page().
 *
 * Native configurations use the false predicate stub in mm/ppps.h.
 */
bool ppps_uffd_copy_tuple_ok(struct vm_area_struct *dst_vma, unsigned long dst_addr,
		unsigned long remaining, uffd_flags_t flags)
{
	if (!ppps_mm_is_compat(dst_vma->vm_mm))
		return false;

	/* COPY only: zeropage/continue/poison have their own semantics. */
	if (!uffd_flags_mode_is(flags, MFILL_ATOMIC_COPY))
		return false;

	/* v1 leaves uffd-wp installs on the single-slice path. */
	if (flags & MFILL_ATOMIC_WP)
		return false;

	/*
	 * Private anonymous only: a folio is only marked packed for
	 * vma_is_anonymous() VMAs, and private shmem keeps its slice indices.
	 */
	if (!vma_is_anonymous(dst_vma) || ppps_vma_has_slices(dst_vma) ||
	    (dst_vma->vm_flags & VM_SHARED))
		return false;

	/* Same VMA exclusions the packed fault path applies. */
	if (dst_vma->vm_flags & (VM_MTE | VM_DROPPABLE | VM_LOCKED |
				 VM_MERGEABLE))
		return false;

	/* A tuple must be a whole, native-aligned page inside the VMA. */
	if (!IS_ALIGNED(dst_addr, PAGE_SIZE) || remaining < PAGE_SIZE)
		return false;
	if (dst_addr < dst_vma->vm_start ||
	    dst_addr + PAGE_SIZE > dst_vma->vm_end)
		return false;

	return true;
}

static bool mfill_pte_range_none(pte_t *ptep, unsigned int nr)
{
	unsigned int i;

	for (i = 0; i < nr; i++) {
		if (!pte_none(ptep_get_lockless(ptep + i)))
			return false;
	}

	return true;
}

/*
 * Copy one whole native page and map it behind PPPS_SLICES_PER_PAGE
 * consecutive compat PTEs under a single pte lock, all-or-nothing.
 *
 * Returns 0 on success, -ENOENT with *foliop / *tuple_folio set when the
 * user copy must be retried outside the mmap lock, or -EBUSY when the pte
 * range is not entirely none -- the caller must then fall back to the
 * single-slice path for this tuple.  -EBUSY (never -EEXIST) on purpose:
 * -EEXIST would terminate the whole ioctl even when the leading slices are
 * installable, whereas the single-slice path reproduces the exact
 * stop-at-first-EEXIST semantics and byte-accurate uffdio_copy.copy.
 */
int ppps_uffd_copy_tuple(pmd_t *dst_pmd, struct vm_area_struct *dst_vma, unsigned long dst_addr,
		unsigned long src_addr, struct folio **foliop, bool *tuple_folio)
{
	struct mm_struct *dst_mm = dst_vma->vm_mm;
	pte_t _dst_pte, *dst_pte;
	struct folio *folio;
	/* Protects all PPPS_SLICES_PER_PAGE PTEs in the tuple. */
	spinlock_t *ptl;
	void *kaddr;
	int ret;

	if (!*foliop) {
		ret = -ENOMEM;
		folio = vma_alloc_folio(GFP_HIGHUSER_MOVABLE, 0, dst_vma,
					dst_addr, false);
		if (!folio)
			goto out;

		/*
		 * No memset() here: unlike the single-slice path -- which
		 * leaves PAGE_SIZE - MM_PAGE_SIZE() bytes of the native folio
		 * uncopied and must not leak them -- the copy below overwrites
		 * the entire folio.
		 *
		 * See mfill_atomic_pte_copy() for why faults are disabled
		 * across the copy while the mmap lock is held.
		 */
		kaddr = kmap_local_folio(folio, 0);
		pagefault_disable();
		ret = copy_from_user(kaddr, (const void __user *)src_addr,
				     PAGE_SIZE);
		pagefault_enable();
		kunmap_local(kaddr);

		/* fallback to copy_from_user outside mmap_lock */
		if (unlikely(ret)) {
			ret = -ENOENT;
			*foliop = folio;
			*tuple_folio = true;
			/* don't free the page */
			goto out;
		}

		flush_dcache_folio(folio);
	} else {
		folio = *foliop;
		*foliop = NULL;
		*tuple_folio = false;
	}

	/*
	 * The memory barrier inside __folio_mark_uptodate makes sure that
	 * preceding stores to the page contents become visible before
	 * the set_ptes() write.
	 */
	__folio_mark_uptodate(folio);

	ret = -ENOMEM;
	if (mem_cgroup_charge(folio, dst_mm, GFP_KERNEL))
		goto out_release;

	ret = -EAGAIN;
	dst_pte = pte_offset_map_lock(dst_mm, dst_pmd, dst_addr, &ptl);
	if (!dst_pte)
		goto out_release;

	if (!mfill_pte_range_none(dst_pte, PPPS_SLICES_PER_PAGE)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	add_mm_counter(dst_mm, MM_ANONPAGES, folio_nr_pages(folio));
	folio_add_new_anon_rmap(folio, dst_vma, dst_addr, RMAP_EXCLUSIVE);
	folio_add_lru_vma(folio, dst_vma);

	_dst_pte = mk_pte(&folio->page, dst_vma->vm_page_prot);
	_dst_pte = pte_mkdirty(_dst_pte);
	if (dst_vma->vm_flags & VM_WRITE)
		_dst_pte = pte_mkwrite(_dst_pte, dst_vma);

	/* PPPS set_ptes() advances by MM_PAGE_SIZE() for a compat mm. */
	set_ptes(dst_mm, dst_addr, dst_pte, _dst_pte, PPPS_SLICES_PER_PAGE);

	/* No need to invalidate - they were non-present before */
	update_mmu_cache_range(NULL, dst_vma, dst_addr, dst_pte,
			       PPPS_SLICES_PER_PAGE);
	pte_unmap_unlock(dst_pte, ptl);

	return 0;

out_unlock:
	pte_unmap_unlock(dst_pte, ptl);
out_release:
	folio_put(folio);
out:
	return ret;
}

bool ppps_uffd_move_tuple_ok(struct vm_area_struct *dst_vma, struct vm_area_struct *src_vma,
		unsigned long dst_addr, unsigned long src_addr, unsigned long len)
{
	vm_flags_t excluded = VM_MTE | VM_DROPPABLE | VM_LOCKED |
			      VM_MERGEABLE;

	if (!ppps_mm_is_compat(src_vma->vm_mm))
		return false;
	if (!IS_ALIGNED(src_addr, PAGE_SIZE) ||
	    !IS_ALIGNED(dst_addr, PAGE_SIZE) || len < PAGE_SIZE)
		return false;
	if (src_addr < src_vma->vm_start ||
	    src_addr + PAGE_SIZE > src_vma->vm_end ||
	    dst_addr < dst_vma->vm_start ||
	    dst_addr + PAGE_SIZE > dst_vma->vm_end)
		return false;
	if ((src_vma->vm_flags | dst_vma->vm_flags) & excluded)
		return false;

	return true;
}

/*
 * Relocate one complete PPPS tuple without splitting its native folio.
 * The caller holds a reference and the folio lock. Both page-table locks
 * cover every PTE because native-page-aligned tuples cannot cross a PTE page.
 */
long ppps_uffd_move_tuple(struct mm_struct *mm, struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, unsigned long dst_addr, unsigned long src_addr,
		pte_t *dst_pte, pte_t *src_pte, pte_t orig_dst_pte, pte_t orig_src_pte,
		pmd_t *dst_pmd, pmd_t dst_pmdval, spinlock_t *dst_ptl, spinlock_t *src_ptl,
		struct folio **src_foliop, bool *ppps_fallback)
{
	struct folio *folio = *src_foliop;
	pte_t src_ptes[PPPS_SLICES_PER_PAGE];
	long ret = -EAGAIN;
	unsigned int i;

	flush_cache_range(src_vma, src_addr, src_addr + PAGE_SIZE);
	double_pt_lock(dst_ptl, src_ptl);
	if (!is_pte_pages_stable(dst_pte, src_pte, orig_dst_pte,
				 orig_src_pte, dst_pmd, dst_pmdval))
		goto out;

	if (!ppps_anon_tuple_is_complete(src_vma, folio, src_addr, src_pte)) {
		*ppps_fallback = true;
		goto out;
	}
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		if (!pte_none(ptep_get(dst_pte + i))) {
			/* Preserve ordinary MOVE's byte-accurate partial result. */
			*ppps_fallback = true;
			goto out;
		}
		src_ptes[i] = ptep_get(src_pte + i);
	}
	if (folio_maybe_dma_pinned(folio) ||
	    !PageAnonExclusive(&folio->page)) {
		ret = -EBUSY;
		goto out;
	}

	/* The four page-table mappings keep the folio alive from here. */
	folio_put(*src_foliop);
	*src_foliop = NULL;
	arch_enter_lazy_mmu_mode();
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++)
		ptep_get_and_clear(mm, src_addr + i * PAGE_SIZE_COMPAT,
				   src_pte + i);

	/* Fast GUP can acquire a pin while the present PTEs are being cleared. */
	if (folio_maybe_dma_pinned(folio)) {
		for (i = 0; i < PPPS_SLICES_PER_PAGE; i++)
			set_pte_at(mm, src_addr + i * PAGE_SIZE_COMPAT,
				   src_pte + i, src_ptes[i]);
		ret = -EBUSY;
		goto out_lazy;
	}

	folio_move_anon_rmap(folio, dst_vma);
	folio->index = ppps_tuple_index(dst_vma, dst_addr);
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		pte_t dst = pte_modify(src_ptes[i], dst_vma->vm_page_prot);

#ifdef CONFIG_MEM_SOFT_DIRTY
		dst = pte_mksoft_dirty(dst);
#endif
		if (pte_dirty(src_ptes[i]))
			dst = pte_mkdirty(dst);
		dst = pte_mkwrite(dst, dst_vma);
		set_pte_at(mm, dst_addr + i * PAGE_SIZE_COMPAT,
			   dst_pte + i, dst);
	}
	ret = PAGE_SIZE;

out_lazy:
	arch_leave_lazy_mmu_mode();
	if (ret == PAGE_SIZE)
		flush_tlb_range(src_vma, src_addr, src_addr + PAGE_SIZE);
	folio_unlock(folio);
out:
	double_pt_unlock(dst_ptl, src_ptl);
	return ret;
}

long ppps_uffd_move_slice(struct mm_struct *mm, struct vm_area_struct *dst_vma,
		struct vm_area_struct *src_vma, unsigned long dst_addr, unsigned long src_addr,
		pte_t *dst_pte, pte_t *src_pte, pte_t orig_dst_pte, pte_t orig_src_pte,
		pmd_t *dst_pmd, pmd_t dst_pmdval, spinlock_t *dst_ptl, spinlock_t *src_ptl,
		struct folio *src_folio)
{
	struct folio *dst_folio;
	struct folio *prealloc;
	unsigned long src_off = vma_address_to_slice(src_vma, src_addr) *
		PAGE_SIZE_COMPAT;
	unsigned long dst_off = vma_address_to_slice(dst_vma, dst_addr) *
		PAGE_SIZE_COMPAT;
	pte_t dst;
	long ret = -EAGAIN;
	bool new_tuple = false;

	if (folio_maybe_dma_pinned(src_folio))
		return -EBUSY;
	prealloc = vma_alloc_zeroed_movable_folio(dst_vma, dst_addr);
	if (!prealloc)
		return -ENOMEM;
	if (mem_cgroup_charge(prealloc, mm, GFP_KERNEL)) {
		folio_put(prealloc);
		return -ENOMEM;
	}
	__folio_mark_uptodate(prealloc);

	double_pt_lock(dst_ptl, src_ptl);
	if (!is_pte_pages_stable(dst_pte, src_pte, orig_dst_pte,
				 orig_src_pte, dst_pmd, dst_pmdval))
		goto out_unlock;
	if (!pte_none(ptep_get(dst_pte))) {
		ret = -EEXIST;
		goto out_unlock;
	}
	dst_folio = ppps_anon_hole_fill_folio(dst_vma, dst_pte, dst_addr, NULL);
	if (!dst_folio) {
		dst_folio = prealloc;
		new_tuple = true;
	}

	ptep_get_and_clear(mm, src_addr, src_pte);
	flush_tlb_page(src_vma, src_addr);
	if (folio_maybe_dma_pinned(src_folio)) {
		set_pte_at(mm, src_addr, src_pte, orig_src_pte);
		flush_tlb_page(src_vma, src_addr);
		ret = -EBUSY;
		goto out_unlock;
	}
	ppps_anon_copy_slice(dst_folio, dst_off >> PAGE_SHIFT_COMPAT,
			     src_folio, src_off >> PAGE_SHIFT_COMPAT);

	if (new_tuple) {
		folio_add_new_anon_rmap(dst_folio, dst_vma, dst_addr,
					RMAP_EXCLUSIVE);
		folio_add_lru_vma(dst_folio, dst_vma);
		inc_mm_counter(mm, MM_ANONPAGES);
		prealloc = NULL;
	}
	dst = vma_pte_mkslice(dst_vma, mk_pte(&dst_folio->page, dst_vma->vm_page_prot), dst_addr);
	dst = ppps_pte_inherit(dst, orig_src_pte);
#ifdef CONFIG_MEM_SOFT_DIRTY
	dst = pte_mksoft_dirty(dst);
#endif
	if (pte_write(orig_src_pte))
		dst = pte_mkwrite(dst, dst_vma);
	set_pte_at(mm, dst_addr, dst_pte, dst);

	if (ppps_anon_slice_unmap(src_vma, src_folio, src_pte, src_addr))
		dec_mm_counter(mm, MM_ANONPAGES);
	ret = MM_PAGE_SIZE(mm);

out_unlock:
	double_pt_unlock(dst_ptl, src_ptl);
	if (prealloc)
		folio_put(prealloc);
	return ret;
}
