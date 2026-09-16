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
	/* Tuple creation waits for generic-MM activation. */
	if (!ppps_vma_shares_tuple(dst_vma))
		return false;

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
					dst_addr);
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

