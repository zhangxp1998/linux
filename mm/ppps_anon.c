// SPDX-License-Identifier: GPL-2.0-only
/*
 * PPPS packed anonymous mappings.
 *
 * Keep the full-tuple policy and its lifecycle operations in this file so
 * generic MM paths only need narrow, configuration-gated hooks.  A packed
 * mapping is always one native folio represented by four adjacent 4K PTEs;
 * operations that need independent slice lifetimes depack it first.
 */

#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/ppps.h>
#include <linux/delayacct.h>
#include <linux/highmem.h>
#include <linux/kmsan.h>
#include <linux/ksm.h>
#include <linux/memcontrol.h>
#include <linux/mmu_notifier.h>
#include <linux/rmap.h>
#include <linux/sched/task.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/userfaultfd_k.h>

#include <asm/tlbflush.h>

#include <trace/hooks/mm.h>

#include "internal.h"
#include "ppps.h"
#include "swap.h"

static struct folio *ppps_folio_prealloc(struct mm_struct *mm,
					 struct vm_area_struct *vma,
					 unsigned long address, bool zero)
{
	struct folio *folio;

	if (zero)
		folio = vma_alloc_zeroed_movable_folio(vma, address);
	else
		folio = vma_alloc_folio(GFP_HIGHUSER_MOVABLE, 0, vma,
					address);
	if (!folio)
		return NULL;
	if (mem_cgroup_charge(folio, mm, GFP_KERNEL)) {
		folio_put(folio);
		return NULL;
	}
	folio_throttle_swaprate(folio, GFP_KERNEL);
	return folio;
}

/*
 * A packed anonymous mapping uses one native folio as four consecutive
 * process-page PTEs.  The invariant is deliberately strict: a mapping
 * instance is either this complete tuple, or a singleton mapping of slice 0.
 */
bool ppps_anon_pte_is_packed(struct vm_area_struct *vma,
			     struct folio *folio, unsigned long address,
			     pte_t *ptep)
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
 * A zero tuple maps the four slices of the native global zero page.  It owns
 * no rmap or page-table references and can be replaced atomically by one
 * private packed folio on the first write.
 */
static bool ppps_anon_pte_is_zero_tuple(struct vm_area_struct *vma,
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
 * Packed swap PTEs contain one identical native swap entry, unlike the
 * consecutive offsets handled by swap_pte_batch().
 */
int ppps_swap_pte_batch(pte_t *ptep, int max_nr, pte_t first,
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

static void ppps_copy_present_pte(struct vm_area_struct *dst_vma,
				  struct vm_area_struct *src_vma,
				  pte_t *dst_pte, pte_t *src_pte,
				  pte_t pte, unsigned long addr)
{
	if (is_cow_mapping(src_vma->vm_flags) && pte_write(pte)) {
		wrprotect_ptes(src_vma->vm_mm, addr, src_pte, 1);
		pte = pte_wrprotect(pte);
	}
	if (src_vma->vm_flags & VM_SHARED)
		pte = pte_mkclean(pte);
	pte = pte_mkold(pte);
	if (!userfaultfd_wp(dst_vma))
		pte = pte_clear_uffd_wp(pte);
	set_ptes(dst_vma->vm_mm, addr, dst_pte, pte, 1);
}

static int ppps_anon_dup_rmap(struct folio *folio,
			      struct vm_area_struct *src_vma);

int ppps_anon_copy_present_ptes(struct vm_area_struct *dst_vma,
				struct vm_area_struct *src_vma,
				pte_t *dst_pte, pte_t *src_pte,
				unsigned long addr, int max_nr, int *rss,
				struct folio *folio, struct folio **prealloc)
{
	const int nr_ptes = PPPS_SLICES_PER_PAGE;
	struct folio *new_folio;
	int err;
	int i;

	if (max_nr < nr_ptes || !IS_ALIGNED(addr, PAGE_SIZE) ||
	    !ppps_anon_pte_is_packed(src_vma, folio, addr, src_pte))
		return 0;

	new_folio = *prealloc;
	if (new_folio) {
		if (copy_mc_user_highpage(&new_folio->page, &folio->page,
					  addr, src_vma))
			return -EHWPOISON;
		*prealloc = NULL;
		__folio_mark_uptodate(new_folio);
		folio_ref_add(new_folio, nr_ptes - 1);
		ppps_anon_add_new_rmap(new_folio, dst_vma, addr, nr_ptes);
		folio_add_lru_vma(new_folio, dst_vma);
		rss[MM_ANONPAGES] += folio_nr_pages(new_folio);

		for (i = 0; i < nr_ptes; i++) {
			unsigned long cur = addr + i * PAGE_SIZE_COMPAT;
			pte_t src = ptep_get(src_pte + i);
			pte_t dst = mk_pte(&new_folio->page,
					   dst_vma->vm_page_prot);

			dst = ppps_folio_mk_pte_explicit_slice(dst_vma,
							       new_folio, dst, i);
			dst = maybe_mkwrite(pte_mkdirty(dst), dst_vma);
			if (userfaultfd_pte_wp(dst_vma, src))
				dst = pte_mkuffd_wp(dst);
			set_pte_at(dst_vma->vm_mm, cur, dst_pte + i, dst);
		}
		return nr_ptes;
	}

	folio_ref_add(folio, nr_ptes);
	err = ppps_anon_dup_rmap(folio, src_vma);
	if (unlikely(err)) {
		folio_ref_sub(folio, nr_ptes);
		return -EAGAIN;
	}

	rss[MM_ANONPAGES] += folio_nr_pages(folio);
	for (i = 0; i < nr_ptes; i++) {
		pte_t pte = ptep_get(src_pte + i);
		unsigned long cur = addr + i * PAGE_SIZE_COMPAT;

		ppps_copy_present_pte(dst_vma, src_vma, dst_pte + i,
				      src_pte + i, pte, cur);
	}

	return nr_ptes;
}

void ppps_anon_add_new_rmap(struct folio *folio,
			    struct vm_area_struct *vma,
			    unsigned long address, int nr_ptes)
{
	bool packed = ppps_mm_is_compat(vma->vm_mm) &&
		vma_is_anonymous(vma) && !folio_test_large(folio) &&
		nr_ptes == PPPS_SLICES_PER_PAGE;

	if (packed)
		folio_set_ppps_packed_anon(folio);
	folio_add_new_anon_rmap(folio, vma, address, RMAP_EXCLUSIVE);
}

void ppps_anon_remove_rmap(struct folio *folio,
			   struct vm_area_struct *vma)
{
	bool last;

	VM_WARN_ON_FOLIO(!folio_test_anon(folio) || folio_test_large(folio) ||
			 !ppps_mm_is_compat(vma->vm_mm), folio);

	last = atomic_add_negative(-(int)PPPS_SLICES_PER_PAGE,
				   &folio->_mapcount);
	if (last) {
		folio_clear_ppps_packed_anon(folio);
		__lruvec_stat_mod_folio(folio, NR_ANON_MAPPED, -1);
	}
	trace_android_vh_folio_remove_rmap(folio, &folio->page, 1,
					   PGTABLE_LEVEL_PTE);
	trace_android_vh_folio_remove_rmap_ptes(folio);
	munlock_vma_folio(folio, vma);
}

static int ppps_anon_dup_rmap(struct folio *folio,
			      struct vm_area_struct *src_vma)
{
	VM_WARN_ON_FOLIO(!folio_test_anon(folio) || folio_test_large(folio) ||
			 !ppps_mm_is_compat(src_vma->vm_mm) ||
			 !folio_test_ppps_packed_anon(folio), folio);

	if (PageAnonExclusive(&folio->page)) {
		if (unlikely(folio_needs_cow_for_dma(src_vma, folio)))
			return -EBUSY;
		ClearPageAnonExclusive(&folio->page);
	}
	atomic_add(PPPS_SLICES_PER_PAGE, &folio->_mapcount);
	return 0;
}

unsigned long ppps_anon_folio_nr_pages(struct folio *folio)
{
	return folio_test_ppps_packed_anon(folio) ?
		PPPS_SLICES_PER_PAGE : folio_nr_pages(folio);
}

static void ppps_put_depack_folios(struct folio **folios)
{
	int i;

	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++)
		if (folios[i])
			folio_put(folios[i]);
}

/* Hardware may update access/dirty state while the PTL is dropped. */
static bool ppps_pte_same_mapping(pte_t a, pte_t b)
{
	return pte_same(pte_mkold(pte_mkclean(a)),
			pte_mkold(pte_mkclean(b)));
}

static pte_t ppps_pte_merge_ad(pte_t pte, pte_t ad)
{
	pte = pte_young(ad) ? pte_mkyoung(pte) : pte_mkold(pte);
	pte = pte_dirty(ad) ? pte_mkdirty(pte) : pte_mkclean(pte);
	return pte;
}

/*
 * Replace one packed anonymous mapping with four ordinary singleton folios.
 * The caller holds mmap_lock for writing and has write-locked the VMA, which
 * prevents concurrent faults while the tuple is write-protected and copied.
 */
static int ppps_depack_anon_folio(struct vm_area_struct *vma,
				  unsigned long base)
{
	struct folio *new_folios[PPPS_SLICES_PER_PAGE] = {};
	struct mm_struct *mm = vma->vm_mm;
	struct folio *old_folio;
	struct mmu_notifier_range range;
	pte_t old_ptes[PPPS_SLICES_PER_PAGE];
	pte_t frozen_ptes[PPPS_SLICES_PER_PAGE];
	bool was_exclusive;
	spinlock_t *ptl; /* Protects the complete packed tuple. */
	struct page *page;
	pte_t *ptep;
	pmd_t *pmd;
	int ret = 0;
	int i;

	if (!ppps_mm_is_compat(mm) || !vma_is_anonymous(vma) ||
	    (vma->vm_flags & (VM_SPECIAL | VM_PFNMAP | VM_MIXEDMAP | VM_IO)) ||
	    base < vma->vm_start || base + PAGE_SIZE > vma->vm_end)
		return 0;

	mmap_assert_write_locked(mm);
	vma_assert_write_locked(vma);
	pmd = mm_find_pmd(mm, base);
	if (!pmd)
		return 0;
	ptep = pte_offset_map_lock(mm, pmd, base, &ptl);
	if (!ptep)
		return 0;
	if (!pte_present(ptep_get(ptep)) || pte_special(ptep_get(ptep)) ||
	    !pfn_valid(pte_pfn(ptep_get(ptep)))) {
		pte_unmap_unlock(ptep, ptl);
		return 0;
	}
	page = vm_normal_page(vma, base, ptep_get(ptep));
	if (!page) {
		pte_unmap_unlock(ptep, ptl);
		return 0;
	}
	old_folio = page_folio(page);
	if (!ppps_anon_pte_is_packed(vma, old_folio, base, ptep)) {
		pte_unmap_unlock(ptep, ptl);
		return 0;
	}
	folio_get(old_folio);
	pte_unmap_unlock(ptep, ptl);

	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		unsigned long address = base + i * PAGE_SIZE_COMPAT;

		new_folios[i] = ppps_folio_prealloc(mm, vma, address, false);
		if (!new_folios[i]) {
			ret = -ENOMEM;
			goto out_put;
		}
	}

	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm, base,
				base + PAGE_SIZE);
	mmu_notifier_invalidate_range_start(&range);

	pmd = mm_find_pmd(mm, base);
	if (!pmd) {
		ret = -EAGAIN;
		goto out_notifier;
	}
	ptep = pte_offset_map_lock(mm, pmd, base, &ptl);
	if (!ptep) {
		ret = -EAGAIN;
		goto out_notifier;
	}
	if (!ppps_anon_pte_is_packed(vma, old_folio, base, ptep)) {
		pte_unmap_unlock(ptep, ptl);
		ret = -EAGAIN;
		goto out_notifier;
	}
	if (folio_maybe_dma_pinned(old_folio)) {
		pte_unmap_unlock(ptep, ptl);
		ret = -EBUSY;
		goto out_notifier;
	}

	was_exclusive = PageAnonExclusive(&old_folio->page);
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		unsigned long address = base + i * PAGE_SIZE_COMPAT;

		old_ptes[i] = ptep_get(ptep + i);
		ptep_set_wrprotect(mm, address, ptep + i);
		frozen_ptes[i] = ptep_get(ptep + i);
	}
	if (was_exclusive)
		ClearPageAnonExclusive(&old_folio->page);
	flush_tlb_range(vma, base, base + PAGE_SIZE);
	pte_unmap_unlock(ptep, ptl);

	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		void *kaddr;

		if (copy_mc_highpage(&new_folios[i]->page,
				     &old_folio->page)) {
			ret = -EHWPOISON;
			goto restore_mapping;
		}
		kmsan_copy_page_meta(&new_folios[i]->page, &old_folio->page);
		kaddr = kmap_local_page(&new_folios[i]->page);
		memmove(kaddr, kaddr + i * PAGE_SIZE_COMPAT, PAGE_SIZE_COMPAT);
		memset(kaddr + PAGE_SIZE_COMPAT, 0,
		       PAGE_SIZE - PAGE_SIZE_COMPAT);
		kunmap_local(kaddr);
		flush_dcache_page(&new_folios[i]->page);
		__folio_mark_uptodate(new_folios[i]);
	}

	pmd = mm_find_pmd(mm, base);
	if (!pmd) {
		ret = -EAGAIN;
		goto restore_mapping;
	}
	ptep = pte_offset_map_lock(mm, pmd, base, &ptl);
	if (!ptep) {
		ret = -EAGAIN;
		goto restore_mapping;
	}
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		pte_t cur_pte = ptep_get(ptep + i);

		if (!ppps_pte_same_mapping(cur_pte, frozen_ptes[i])) {
			pte_unmap_unlock(ptep, ptl);
			ret = -EAGAIN;
			goto restore_mapping;
		}
		old_ptes[i] = ppps_pte_merge_ad(old_ptes[i], cur_pte);
	}

	flush_cache_range(vma, base, base + PAGE_SIZE);
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++)
		ptep_clear_flush(vma, base + i * PAGE_SIZE_COMPAT, ptep + i);

	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		unsigned long address = base + i * PAGE_SIZE_COMPAT;
		pte_t entry;

		folio_add_new_anon_rmap(new_folios[i], vma, address,
					RMAP_EXCLUSIVE);
		folio_add_lru_vma(new_folios[i], vma);
		entry = mk_pte(&new_folios[i]->page, pte_pgprot(old_ptes[i]));
		entry = clear_pte_slice_offset(entry);
		set_pte_at(mm, address, ptep + i, entry);
		update_mmu_cache(vma, address, ptep + i);
		new_folios[i] = NULL;
	}
	ppps_anon_remove_rmap(old_folio, vma);
	add_mm_counter(mm, MM_ANONPAGES,
		       PPPS_SLICES_PER_PAGE - folio_nr_pages(old_folio));
	pte_unmap_unlock(ptep, ptl);
	mmu_notifier_invalidate_range_end(&range);

	folio_ref_sub(old_folio, PPPS_SLICES_PER_PAGE);
	free_swap_cache(old_folio);
	folio_put(old_folio);
	return 0;

restore_mapping:
	pmd = mm_find_pmd(mm, base);
	if (pmd) {
		ptep = pte_offset_map_lock(mm, pmd, base, &ptl);
		if (ptep) {
			bool unchanged = true;
			pte_t restore_ptes[PPPS_SLICES_PER_PAGE];

			for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
				pte_t cur_pte = ptep_get(ptep + i);

				if (!ppps_pte_same_mapping(cur_pte,
							   frozen_ptes[i]))
					unchanged = false;
				restore_ptes[i] =
					ppps_pte_merge_ad(old_ptes[i], cur_pte);
			}
			if (unchanged) {
				for (i = 0; i < PPPS_SLICES_PER_PAGE; i++)
					set_pte_at(mm,
						   base + i * PAGE_SIZE_COMPAT,
						   ptep + i, restore_ptes[i]);
				if (was_exclusive)
					SetPageAnonExclusive(&old_folio->page);
				flush_tlb_range(vma, base, base + PAGE_SIZE);
			}
			pte_unmap_unlock(ptep, ptl);
		}
	}
out_notifier:
	mmu_notifier_invalidate_range_end(&range);
out_put:
	ppps_put_depack_folios(new_folios);
	folio_put(old_folio);
	return ret;
}

/* Materialize packed swap before an operation can move its PTEs. */
static int ppps_swapin_anon_folio(struct vm_area_struct *vma,
				  unsigned long base)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long start = max(base, vma->vm_start);
	unsigned long end = min(base + PAGE_SIZE, vma->vm_end);
	unsigned long address;

	if (!ppps_mm_is_compat(mm) || !vma_is_anonymous(vma))
		return 0;
	vma_assert_write_locked(vma);

	for (address = start; address < end; address += PAGE_SIZE_COMPAT) {
		vm_fault_t fault;
		spinlock_t *ptl; /* Protects the inspected PTE. */
		pte_t *ptep;
		pmd_t *pmd;
		pte_t pte;

		pmd = mm_find_pmd(mm, address);
		if (!pmd)
			continue;
		ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
		if (!ptep)
			continue;
		pte = ptep_get(ptep);
		pte_unmap_unlock(ptep, ptl);
		if (!is_swap_pte(pte) || !pte_swp_ppps_packed(pte))
			continue;

		fault = handle_mm_fault(vma, address, FAULT_FLAG_REMOTE, NULL);
		if (fault & VM_FAULT_ERROR)
			return vm_fault_to_errno(fault, 0);
		if (WARN_ON_ONCE(fault & (VM_FAULT_RETRY | VM_FAULT_COMPLETED)))
			return -EAGAIN;
	}
	return 0;
}

static int ppps_depack_anon_at(struct vm_area_struct *vma,
			       unsigned long base)
{
	int retries = 0;
	int ret;

	do {
		vma_start_write(vma);
		ret = ppps_swapin_anon_folio(vma, base);
		if (!ret)
			ret = ppps_depack_anon_folio(vma, base);
		if (ret != -EAGAIN)
			return ret;
		if (fatal_signal_pending(current))
			return -EINTR;
		cond_resched();
	} while (++retries < 16);

	return -EAGAIN;
}

int ppps_depack_anon_range(struct mm_struct *mm, unsigned long start,
			   unsigned long end, bool all)
{
	struct vm_area_struct *vma;
	unsigned long base;
	int ret;

	if (!ppps_mm_is_compat(mm) || start >= end)
		return 0;
	mmap_assert_write_locked(mm);

	if (!all) {
		if (!IS_ALIGNED(start, PAGE_SIZE)) {
			vma = find_vma(mm, start);
			if (vma && start >= vma->vm_start) {
				base = ALIGN_DOWN(start, PAGE_SIZE);
				ret = ppps_depack_anon_at(vma, base);
				if (ret)
					return ret;
			}
		}
		if (!IS_ALIGNED(end, PAGE_SIZE)) {
			vma = find_vma(mm, end - 1);
			if (vma && end - 1 >= vma->vm_start) {
				base = ALIGN_DOWN(end, PAGE_SIZE);
				if (IS_ALIGNED(start, PAGE_SIZE) ||
				    base != ALIGN_DOWN(start, PAGE_SIZE)) {
					ret = ppps_depack_anon_at(vma, base);
					if (ret)
						return ret;
				}
			}
		}
		return 0;
	}

	for (vma = find_vma(mm, start); vma && vma->vm_start < end;
	     vma = find_vma(mm, vma->vm_end)) {
		unsigned long first = max(start, vma->vm_start);
		unsigned long last = min(end, vma->vm_end);

		for (base = ALIGN_DOWN(first, PAGE_SIZE); base < last;
		     base += PAGE_SIZE) {
			ret = ppps_depack_anon_at(vma, base);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static bool ppps_anon_vma_can_pack(struct vm_area_struct *vma,
				   unsigned long address)
{
	unsigned long base = ALIGN_DOWN(address, PAGE_SIZE);

	return ppps_mm_is_compat(vma->vm_mm) && !userfaultfd_armed(vma) &&
		!(vma->vm_flags & (VM_MTE | VM_DROPPABLE | VM_LOCKED |
				    VM_MERGEABLE)) &&
		base >= vma->vm_start && base + PAGE_SIZE <= vma->vm_end;
}

static bool ppps_pte_range_none(pte_t *ptep, unsigned int nr)
{
	unsigned int i;

	for (i = 0; i < nr; i++) {
		if (!pte_none(ptep_get(ptep + i)))
			return false;
	}
	return true;
}

enum ppps_anon_wp_type ppps_anon_wp_type(struct vm_area_struct *vma,
					 struct folio *folio,
					 unsigned long address, pte_t *ptep)
{
	if (folio && ppps_anon_pte_is_packed(vma, folio, address, ptep))
		return PPPS_ANON_WP_PACKED;
	if (!folio && ppps_anon_vma_can_pack(vma, address) &&
	    ppps_anon_pte_is_zero_tuple(vma, address, ptep))
		return PPPS_ANON_WP_ZERO;
	return PPPS_ANON_WP_NONE;
}

bool ppps_anon_wp_can_reuse(struct folio *folio,
			    struct vm_area_struct *vma)
{
	int expected_refs = PPPS_SLICES_PER_PAGE +
		folio_test_swapcache(folio);

	if (!folio_test_ppps_packed_anon(folio) ||
	    folio_mapcount(folio) != PPPS_SLICES_PER_PAGE ||
	    folio_ref_count(folio) > expected_refs)
		return false;
	if (!folio_test_lru(folio))
		lru_add_drain();
	if (folio_ref_count(folio) > PPPS_SLICES_PER_PAGE +
		folio_test_swapcache(folio))
		return false;
	if (!folio_trylock(folio))
		return false;
	if (folio_test_swapcache(folio))
		folio_free_swap(folio);
	if (!folio_test_ppps_packed_anon(folio) ||
	    folio_mapcount(folio) != PPPS_SLICES_PER_PAGE ||
	    folio_ref_count(folio) != PPPS_SLICES_PER_PAGE ||
	    folio_maybe_dma_pinned(folio)) {
		folio_unlock(folio);
		return false;
	}
	folio_move_anon_rmap(folio, vma);
	folio_unlock(folio);
	return true;
}

static vm_fault_t ppps_wp_zero_tuple_copy(struct vm_fault *vmf)
{
	const bool unshare = vmf->flags & FAULT_FLAG_UNSHARE;
	const int nr_ptes = PPPS_SLICES_PER_PAGE;
	struct vm_area_struct *vma = vmf->vma;
	struct mm_struct *mm = vma->vm_mm;
	unsigned long base = ALIGN_DOWN(vmf->address, PAGE_SIZE);
	unsigned int fault_slice = (vmf->address - base) >> PAGE_SHIFT_COMPAT;
	struct folio *new_folio;
	struct mmu_notifier_range range;
	pte_t old_ptes[PPPS_SLICES_PER_PAGE];
	spinlock_t *ptl; /* Protects the complete zero tuple. */
	pte_t *ptep;
	vm_fault_t ret;
	int i;

	delayacct_wpcopy_start();
	ret = vmf_anon_prepare(vmf);
	if (ret)
		goto out;

	new_folio = ppps_folio_prealloc(mm, vma, base, true);
	if (!new_folio) {
		ret = VM_FAULT_OOM;
		goto out;
	}
	__folio_mark_uptodate(new_folio);

	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm, base,
				base + PAGE_SIZE);
	mmu_notifier_invalidate_range_start(&range);

	ptep = pte_offset_map_lock(mm, vmf->pmd, base, &ptl);
	if (!ptep || !ppps_anon_vma_can_pack(vma, base) ||
	    !ppps_anon_pte_is_zero_tuple(vma, base, ptep) ||
	    !pte_same(ptep_get(ptep + fault_slice), vmf->orig_pte)) {
		if (ptep)
			pte_unmap_unlock(ptep, ptl);
		mmu_notifier_invalidate_range_end(&range);
		folio_put(new_folio);
		ret = 0;
		goto out;
	}

	for (i = 0; i < nr_ptes; i++)
		old_ptes[i] = ptep_get(ptep + i);
	flush_cache_range(vma, base, base + PAGE_SIZE);
	for (i = 0; i < nr_ptes; i++) {
		unsigned long address = base + i * PAGE_SIZE_COMPAT;

		ptep_clear_flush(vma, address, ptep + i);
		ksm_might_unmap_zero_page(mm, old_ptes[i]);
	}

	folio_ref_add(new_folio, nr_ptes - 1);
	add_mm_counter(mm, MM_ANONPAGES, folio_nr_pages(new_folio));
	ppps_anon_add_new_rmap(new_folio, vma, base, nr_ptes);
	folio_add_lru_vma(new_folio, vma);

	for (i = 0; i < nr_ptes; i++) {
		unsigned long address = base + i * PAGE_SIZE_COMPAT;
		pte_t entry = mk_pte(&new_folio->page, vma->vm_page_prot);

		entry = ppps_folio_mk_pte_explicit_slice(vma, new_folio,
							 entry, i);
		entry = pte_sw_mkyoung(entry);
		if (pte_soft_dirty(old_ptes[i]))
			entry = pte_mksoft_dirty(entry);
		if (pte_uffd_wp(old_ptes[i]))
			entry = pte_mkuffd_wp(entry);
		if (i == fault_slice && !unshare)
			entry = maybe_mkwrite(pte_mkdirty(entry), vma);
		else
			entry = pte_wrprotect(entry);

		set_pte_at(mm, address, ptep + i, entry);
		update_mmu_cache_range(vmf, vma, address, ptep + i, 1);
	}

	pte_unmap_unlock(ptep, ptl);
	mmu_notifier_invalidate_range_end(&range);
	ret = 0;
out:
	delayacct_wpcopy_end();
	return ret;
}

static vm_fault_t ppps_wp_packed_copy(struct vm_fault *vmf,
				      struct folio *old_folio)
{
	const bool unshare = vmf->flags & FAULT_FLAG_UNSHARE;
	const int nr_ptes = PPPS_SLICES_PER_PAGE;
	struct vm_area_struct *vma = vmf->vma;
	struct mm_struct *mm = vma->vm_mm;
	unsigned long base = ALIGN_DOWN(vmf->address, PAGE_SIZE);
	unsigned int fault_slice = (vmf->address - base) >> PAGE_SHIFT_COMPAT;
	struct folio *new_folio;
	struct mmu_notifier_range range;
	pte_t old_ptes[PPPS_SLICES_PER_PAGE];
	spinlock_t *ptl; /* Protects the complete packed tuple. */
	pte_t *ptep;
	int i;

	delayacct_wpcopy_start();
	new_folio = ppps_folio_prealloc(mm, vma, base, false);
	if (!new_folio) {
		folio_put(old_folio);
		delayacct_wpcopy_end();
		return VM_FAULT_OOM;
	}

	if (copy_mc_user_highpage(&new_folio->page, &old_folio->page,
				  base, vma)) {
		folio_put(new_folio);
		folio_put(old_folio);
		delayacct_wpcopy_end();
		return VM_FAULT_HWPOISON;
	}
	kmsan_copy_page_meta(&new_folio->page, &old_folio->page);
	__folio_mark_uptodate(new_folio);

	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm, base,
				base + PAGE_SIZE);
	mmu_notifier_invalidate_range_start(&range);

	ptep = pte_offset_map_lock(mm, vmf->pmd, base, &ptl);
	if (!ptep ||
	    !ppps_anon_pte_is_packed(vma, old_folio, base, ptep) ||
	    !pte_same(ptep_get(ptep + fault_slice), vmf->orig_pte)) {
		if (ptep)
			pte_unmap_unlock(ptep, ptl);
		mmu_notifier_invalidate_range_end(&range);
		folio_put(new_folio);
		folio_put(old_folio);
		delayacct_wpcopy_end();
		return 0;
	}

	for (i = 0; i < nr_ptes; i++)
		old_ptes[i] = ptep_get(ptep + i);
	flush_cache_range(vma, base, base + PAGE_SIZE);
	for (i = 0; i < nr_ptes; i++)
		ptep_clear_flush(vma, base + i * PAGE_SIZE_COMPAT, ptep + i);

	folio_ref_add(new_folio, nr_ptes - 1);
	ppps_anon_add_new_rmap(new_folio, vma, base, nr_ptes);
	folio_add_lru_vma(new_folio, vma);

	for (i = 0; i < nr_ptes; i++) {
		unsigned long address = base + i * PAGE_SIZE_COMPAT;
		pte_t entry = mk_pte(&new_folio->page, vma->vm_page_prot);

		entry = ppps_folio_mk_pte_explicit_slice(vma, new_folio,
							 entry, i);
		entry = pte_sw_mkyoung(entry);
		if (pte_soft_dirty(old_ptes[i]))
			entry = pte_mksoft_dirty(entry);
		if (pte_uffd_wp(old_ptes[i]))
			entry = pte_mkuffd_wp(entry);
		if (i == fault_slice && !unshare)
			entry = maybe_mkwrite(pte_mkdirty(entry), vma);
		else
			entry = pte_wrprotect(entry);

		set_pte_at(mm, address, ptep + i, entry);
		update_mmu_cache_range(vmf, vma, address, ptep + i, 1);
	}

	ppps_anon_remove_rmap(old_folio, vma);
	pte_unmap_unlock(ptep, ptl);
	mmu_notifier_invalidate_range_end(&range);

	folio_ref_sub(old_folio, PPPS_SLICES_PER_PAGE);
	free_swap_cache(old_folio);
	folio_put(old_folio);
	delayacct_wpcopy_end();
	return 0;
}

vm_fault_t ppps_anon_wp_copy(struct vm_fault *vmf, struct folio *folio,
			     enum ppps_anon_wp_type type)
{
	if (type == PPPS_ANON_WP_PACKED)
		return ppps_wp_packed_copy(vmf, folio);
	if (type == PPPS_ANON_WP_ZERO)
		return ppps_wp_zero_tuple_copy(vmf);
	return 0;
}

bool ppps_anon_fault_can_pack(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = ALIGN_DOWN(vmf->address, PAGE_SIZE);
	spinlock_t *ptl; /* Protects the candidate packed tuple. */
	pte_t *pte;
	bool empty;

	if (!ppps_anon_vma_can_pack(vma, addr))
		return false;

	pte = pte_offset_map_lock(vma->vm_mm, vmf->pmd, addr, &ptl);
	if (!pte)
		return false;
	empty = ppps_pte_range_none(pte, PPPS_SLICES_PER_PAGE);
	pte_unmap_unlock(pte, ptl);
	return empty;
}

void ppps_anon_swapin_init(struct ppps_anon_swapin *swapin,
			   struct vm_fault *vmf)
{
	swapin->active = ppps_mm_is_compat(vmf->vma->vm_mm) &&
		pte_swp_ppps_packed(vmf->orig_pte);
	if (swapin->active)
		swapin->slice = offset_in_page(vmf->address) >>
			PAGE_SHIFT_COMPAT;
}

bool ppps_anon_swapin_active(const struct ppps_anon_swapin *swapin)
{
	return swapin->active;
}

vm_fault_t ppps_anon_swapin_prepare(struct ppps_anon_swapin *swapin,
				    struct vm_fault *vmf,
				    struct folio *swapcache)
{
	struct vm_area_struct *vma = vmf->vma;

	if (!swapin->active)
		return 0;
	if (unlikely(!folio_test_uptodate(swapcache)))
		return VM_FAULT_SIGBUS;

	swapin->folio = ppps_folio_prealloc(vma->vm_mm, vma,
					    ALIGN_DOWN(vmf->address, PAGE_SIZE),
					    false);
	if (unlikely(!swapin->folio))
		return VM_FAULT_OOM;

	__folio_set_locked(swapin->folio);
	if (copy_mc_highpage(&swapin->folio->page, &swapcache->page))
		return VM_FAULT_HWPOISON;
	kmsan_copy_page_meta(&swapin->folio->page, &swapcache->page);
	flush_dcache_page(&swapin->folio->page);
	__folio_mark_uptodate(swapin->folio);
	return 0;
}

bool ppps_anon_swapin_revalidate(struct ppps_anon_swapin *swapin,
				 struct vm_fault *vmf, swp_entry_t entry,
				 struct folio **folio, struct page **page)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long base;
	pte_t *base_ptep;
	unsigned int i;

	if (!swapin->active)
		return true;

	base = ALIGN_DOWN(vmf->address, PAGE_SIZE);
	base_ptep = vmf->pte - swapin->slice;
	if (unlikely(base < vma->vm_start || base + PAGE_SIZE > vma->vm_end))
		return false;
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		swapin->ptes[i] = ptep_get(base_ptep + i);
		if (unlikely(!is_swap_pte(swapin->ptes[i]) ||
			     !pte_swp_ppps_packed(swapin->ptes[i]) ||
			     pte_to_swp_entry(swapin->ptes[i]).val != entry.val))
			return false;
	}

	*folio = swapin->folio;
	swapin->folio = NULL;
	*page = &(*folio)->page;
	swapin->mapped = true;
	return true;
}

static bool ppps_should_try_to_free_swap(struct folio *folio,
					 struct vm_area_struct *vma,
					 unsigned int fault_flags)
{
	if (!folio_test_swapcache(folio))
		return false;
	if (mem_cgroup_swap_full(folio) || (vma->vm_flags & VM_LOCKED) ||
	    folio_test_mlocked(folio))
		return true;
	return (fault_flags & FAULT_FLAG_WRITE) && !folio_test_ksm(folio) &&
		folio_ref_count(folio) == 1 + folio_nr_pages(folio);
}

bool ppps_anon_swapin_map(struct ppps_anon_swapin *swapin,
			  struct vm_fault *vmf, struct folio *swapcache,
			  struct folio *folio, swp_entry_t entry,
			  unsigned long *address, pte_t **ptep,
			  int *nr_pages)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long base;
	pte_t *base_ptep;
	unsigned int i;

	if (!swapin->mapped)
		return false;
	base = ALIGN_DOWN(vmf->address, PAGE_SIZE);
	base_ptep = vmf->pte - swapin->slice;
	*address = base;
	*ptep = base_ptep;

	arch_swap_restore(folio_swap(entry, folio), folio);
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++)
		swap_free(entry);
	if (ppps_should_try_to_free_swap(swapcache, vma, vmf->flags))
		folio_free_swap(swapcache);

	add_mm_counter(vma->vm_mm, MM_ANONPAGES, folio_nr_pages(folio));
	add_mm_counter(vma->vm_mm, MM_SWAPENTS, -folio_nr_pages(folio));
	folio_ref_add(folio, PPPS_SLICES_PER_PAGE - 1);
	flush_icache_page(vma, &folio->page);
	ppps_anon_add_new_rmap(folio, vma, base, PPPS_SLICES_PER_PAGE);
	folio_add_lru_vma(folio, vma);
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		unsigned long addr = base + i * PAGE_SIZE_COMPAT;
		pte_t new_pte = mk_pte(&folio->page, vma->vm_page_prot);

		new_pte = ppps_folio_mk_pte_explicit_slice(vma, folio,
							   new_pte, i);
		if (pte_swp_soft_dirty(swapin->ptes[i]))
			new_pte = pte_mksoft_dirty(new_pte);
		if (pte_swp_uffd_wp(swapin->ptes[i]))
			new_pte = pte_mkuffd_wp(new_pte);
		if ((vma->vm_flags & VM_WRITE) &&
		    !userfaultfd_pte_wp(vma, new_pte) &&
		    !pte_needs_soft_dirty_wp(vma, new_pte))
			new_pte = pte_mkwrite(pte_mkdirty(new_pte), vma);
		if (i == swapin->slice) {
			trace_android_vh_do_swap_page(folio, &new_pte, vmf,
						      entry);
			vmf->orig_pte = new_pte;
			if (pte_write(new_pte))
				vmf->flags &= ~FAULT_FLAG_WRITE;
		}
		if (WARN_ON_ONCE(pte_write(new_pte) &&
				 !PageAnonExclusive(&folio->page)))
			new_pte = pte_wrprotect(new_pte);
		set_pte_at(vma->vm_mm, addr, base_ptep + i, new_pte);
		arch_do_swap_page_nr(vma->vm_mm, vma, addr, new_pte,
				     swapin->ptes[i], 1);
	}
	*nr_pages = PPPS_SLICES_PER_PAGE;
	return true;
}

void ppps_anon_swapin_cleanup(struct ppps_anon_swapin *swapin)
{
	if (!swapin->folio)
		return;
	folio_unlock(swapin->folio);
	folio_put(swapin->folio);
	swapin->folio = NULL;
}

static int ppps_try_to_unmap_lazyfree(struct folio *folio,
				      struct vm_area_struct *vma,
				      unsigned long address, pte_t *ptep)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long base = ALIGN_DOWN(address, PAGE_SIZE);
	unsigned int slice = (address - base) >> PAGE_SHIFT_COMPAT;
	pte_t old_ptes[PPPS_SLICES_PER_PAGE];
	pte_t *base_ptep = ptep - slice;
	int ref_count, map_count;
	unsigned int i;

	if (userfaultfd_armed(vma) || folio_test_swapbacked(folio) ||
	    !ppps_anon_pte_is_packed(vma, folio, address, ptep))
		return -EBUSY;

	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		unsigned long addr = base + i * PAGE_SIZE_COMPAT;

		old_ptes[i] = ptep_get(base_ptep + i);
		flush_cache_page(vma, addr, pte_pfn(old_ptes[i]));
		old_ptes[i] = ptep_clear_flush(vma, addr, base_ptep + i);
		if (pte_dirty(old_ptes[i]))
			folio_mark_dirty(folio);
	}

	smp_mb(); /* Pairs with the lockless GUP PTE recheck. */
	ref_count = folio_ref_count(folio);
	map_count = folio_mapcount(folio);
	smp_rmb(); /* Order refcount read before the dirty test. */

	if (map_count == PPPS_SLICES_PER_PAGE &&
	    ref_count == 1 + map_count && !folio_test_dirty(folio)) {
		update_hiwater_rss(mm);
		add_mm_counter(mm, MM_ANONPAGES, -folio_nr_pages(folio));
		ppps_anon_remove_rmap(folio, vma);
		folio_ref_sub(folio, PPPS_SLICES_PER_PAGE);
		return 0;
	}

	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++)
		set_pte_at(mm, base + i * PAGE_SIZE_COMPAT,
			   base_ptep + i, old_ptes[i]);
	folio_set_swapbacked(folio);
	return -EBUSY;
}

int ppps_anon_try_to_unmap(struct folio *folio,
			   struct vm_area_struct *vma,
			   unsigned long address, pte_t *ptep)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long base = ALIGN_DOWN(address, PAGE_SIZE);
	unsigned int slice = (address - base) >> PAGE_SHIFT_COMPAT;
	pte_t old_ptes[PPPS_SLICES_PER_PAGE];
	swp_entry_t entry;
	bool anon_exclusive = PageAnonExclusive(&folio->page);
	pte_t *base_ptep = ptep - slice;
	int duplicates = 0;
	unsigned int i;

	if (!anon_exclusive)
		return -EBUSY;

	if (!folio_test_swapbacked(folio))
		return ppps_try_to_unmap_lazyfree(folio, vma, address, ptep);
	if (userfaultfd_armed(vma) || PageHWPoison(&folio->page) ||
	    unlikely(folio_test_swapbacked(folio) !=
		     folio_test_swapcache(folio)) ||
	    !ppps_anon_pte_is_packed(vma, folio, address, ptep))
		return -EBUSY;
	entry = page_swap_entry(&folio->page);

	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		old_ptes[i] = ptep_get(base_ptep + i);
		if (pte_unused(old_ptes[i]))
			return -EBUSY;
	}

	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		unsigned long addr = base + i * PAGE_SIZE_COMPAT;

		flush_cache_page(vma, addr, pte_pfn(old_ptes[i]));
		old_ptes[i] = ptep_clear_flush(vma, addr, base_ptep + i);
		if (pte_dirty(old_ptes[i]))
			folio_mark_dirty(folio);
	}

	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		unsigned long addr = base + i * PAGE_SIZE_COMPAT;

		if (swap_duplicate(entry) < 0)
			goto restore;
		duplicates++;
		if (arch_unmap_one(mm, vma, addr, old_ptes[i]) < 0)
			goto restore;
	}

	if (folio_try_share_anon_rmap_pte(folio, &folio->page))
		goto restore;

	if (list_empty(&mm->mmlist)) {
		spin_lock(&mmlist_lock);
		if (list_empty(&mm->mmlist))
			list_add(&mm->mmlist, &init_mm.mmlist);
		spin_unlock(&mmlist_lock);
	}

	update_hiwater_rss(mm);
	add_mm_counter(mm, MM_ANONPAGES, -folio_nr_pages(folio));
	add_mm_counter(mm, MM_SWAPENTS, folio_nr_pages(folio));
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		unsigned long addr = base + i * PAGE_SIZE_COMPAT;
		pte_t swp_pte = swp_entry_to_pte(entry);

		swp_pte = pte_swp_mk_ppps_packed(swp_pte);
		if (anon_exclusive)
			swp_pte = pte_swp_mkexclusive(swp_pte);
		if (pte_soft_dirty(old_ptes[i]))
			swp_pte = pte_swp_mksoft_dirty(swp_pte);
		if (pte_uffd_wp(old_ptes[i]))
			swp_pte = pte_swp_mkuffd_wp(swp_pte);
		set_pte_at(mm, addr, base_ptep + i, swp_pte);
	}

	ppps_anon_remove_rmap(folio, vma);
	folio_ref_sub(folio, PPPS_SLICES_PER_PAGE);
	return 0;

restore:
	while (duplicates--)
		swap_free(entry);
	if (anon_exclusive)
		SetPageAnonExclusive(&folio->page);
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++)
		set_pte_at(mm, base + i * PAGE_SIZE_COMPAT,
			   base_ptep + i, old_ptes[i]);
	flush_tlb_range(vma, base, base + PAGE_SIZE);
	return -EBUSY;
}
