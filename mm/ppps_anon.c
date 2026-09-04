// SPDX-License-Identifier: GPL-2.0-only
/*
 * PPPS slice-consistent anonymous mappings.
 *
 * A compat anonymous folio keeps a stable physical-slice identity while any
 * subset of its four 4K PTEs may be present.  Keep tuple lifecycle operations
 * here so generic MM paths only need narrow, configuration-gated hooks.
 */

#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/ppps.h>
#include <linux/delayacct.h>
#include <linux/export.h>
#include <linux/highmem.h>
#include <linux/kmsan.h>
#include <linux/ksm.h>
#include <linux/memcontrol.h>
#include <linux/mmu_notifier.h>
#include <linux/rmap.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/userfaultfd_k.h>
#include <linux/vmstat.h>

#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/mte.h>
#include <asm/tlbflush.h>

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
					address, false);
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
 * Tuple geometry
 *
 * vma_address_to_slice() selects a slice by virtual address for anonymous
 * VMAs and by file offset for private file VMAs.
 */

static inline unsigned long ppps_tuple_base(struct vm_area_struct *vma,
					     unsigned long address)
{
	return address - vma_address_to_slice(vma, address) * PAGE_SIZE_COMPAT;
}

static inline pte_t *ppps_tuple_base_ptep(struct vm_area_struct *vma,
		pte_t *ptep, unsigned long address)
{
	unsigned int slice = vma_address_to_slice(vma, address);

	if (!ppps_vma_address_shares_tuple(vma, address))
		return NULL;
	return ptep - slice;
}

pgoff_t ppps_tuple_index(struct vm_area_struct *vma, unsigned long address)
{
	pgoff_t index = linear_page_index(vma, address);

	if (!ppps_vma_has_slices(vma))
		index -= vma_address_to_slice(vma, address);
	return index;
}

/**
 * ppps_pvmw_walk_end - end of a single-folio compat rmap walk
 * @pvmw: walk with a valid starting address and a referenced folio
 *
 * A packed anonymous tuple has a fixed native-page end even as the walk
 * advances through its slices.  A singleton (including KSM) instead spans
 * one process page.  Neither may extend past the current VMA.  Only geometry
 * and folio flags are read; no PTE lock is needed and no PTE is inspected.
 *
 * Return: the exclusive end address, or zero to use generic geometry for
 * native mms, multi-folio ranges and sliced file VMAs.
 */
unsigned long ppps_pvmw_walk_end(struct page_vma_mapped_walk *pvmw)
{
	struct vm_area_struct *vma = pvmw->vma;
	unsigned long address;

	if (!ppps_mm_is_compat(vma->vm_mm) || pvmw->nr_pages != 1 ||
	    ppps_vma_has_slices(vma))
		return 0;
	if (folio_test_ppps_compat_anon(pfn_folio(pvmw->pfn)))
		address = ALIGN_DOWN(pvmw->address, PAGE_SIZE) + PAGE_SIZE;
	else
		address = pvmw->address + MM_PAGE_SIZE(vma->vm_mm);
	return min(address, vma->vm_end);
}
EXPORT_SYMBOL_GPL(ppps_pvmw_walk_end);

bool ppps_vma_shares_tuple(struct vm_area_struct *vma)
{
	if (!ppps_mm_is_compat(vma->vm_mm) ||
	    (vma->vm_flags & (VM_DROPPABLE | VM_MERGEABLE)))
		return false;
	if (vma_is_anonymous(vma))
		return true;
	return vma->vm_file && vma->vm_ops &&
		is_cow_mapping(vma->vm_flags) &&
		!(vma->vm_flags & (VM_PFNMAP | VM_MIXEDMAP)) &&
		!vma_is_dax(vma);
}

/* A tuple straddling two PTE tables cannot share one lock, rmap or ref. */
bool ppps_vma_address_shares_tuple(struct vm_area_struct *vma,
				   unsigned long address)
{
	unsigned long base, pmd_mask;

	if (!ppps_vma_shares_tuple(vma))
		return false;
	base = ppps_tuple_base(vma, address);
	pmd_mask = MM_PMD_MASK(vma->vm_mm);
	return (base & pmd_mask) == ((base + PAGE_SIZE - 1) & pmd_mask);
}

/*
 * Tuple queries
 *
 * All of these run under the PTE lock covering the whole tuple.
 */

static struct folio *ppps_anon_pte_folio(pte_t pte)
{
	struct folio *folio;

	if (!pte_present(pte) || pte_special(pte) || !pfn_valid(pte_pfn(pte)))
		return NULL;
	folio = page_folio(pfn_to_page(pte_pfn(pte)));
	if (!folio_test_anon(folio) || folio_test_large(folio) ||
	    !folio_test_ppps_compat_anon(folio))
		return NULL;
	return folio;
}

/*
 * A folio may only be shared with a VMA whose anon_vma chain contains the
 * anon_vma recorded in folio->mapping.  Merely sharing the same root is not
 * sufficient: once the original VMA disappears, an anon_vma which is absent
 * from every remaining VMA chain can be freed while the folio is still
 * mapped.  This matters for a VA tuple which straddles independently-created
 * VMAs.  Falling back to a second folio is always valid for such a tuple.
 */
static bool ppps_anon_folio_visible_in_vma(struct folio *folio,
					   struct vm_area_struct *vma)
{
	struct anon_vma *mapping = folio_anon_vma(folio);
	struct anon_vma_chain *avc;

	if (!mapping)
		return false;
	list_for_each_entry(avc, &vma->anon_vma_chain, same_vma)
		if (avc->anon_vma == mapping)
			return true;
	return false;
}

static bool ppps_anon_folio_has_other_slices(struct vm_area_struct *vma,
		struct folio *folio, pte_t *ptep, unsigned long address)
{
	pte_t *base_ptep = ppps_tuple_base_ptep(vma, ptep, address);
	unsigned int slice = vma_address_to_slice(vma, address);
	unsigned int i;

	if (!base_ptep)
		return false;
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		if (i != slice &&
		    ppps_anon_pte_folio(ptep_get(base_ptep + i)) == folio)
			return true;
	}
	return false;
}

/*
 * Installing a PTE for @folio: does this slice carry the (mm, tuple) rmap,
 * reference, or does another present slice already hold them? RSS also
 * accounts migration entries: use ppps_anon_folio_has_other_entries() for it.
 */
bool ppps_anon_slice_takes_ownership(struct vm_area_struct *vma, struct folio *folio,
			   pte_t *ptep, unsigned long address)
{
	return !ppps_anon_folio_has_other_slices(vma, folio, ptep, address);
}

/*
 * The PTE at @address has been cleared: was it the last present slice of
 * @folio in its tuple, so that the rmap and reference go with it? Migration
 * entries can still own its RSS charge after the final present slice.
 */
bool ppps_anon_slice_last(struct vm_area_struct *vma, struct folio *folio,
			  pte_t *ptep, unsigned long address)
{
	return !ppps_anon_folio_has_other_slices(vma, folio, ptep, address);
}

/*
 * Release the (mm, tuple) rmap and folio reference after the slice PTE at
 * @address was cleared, unless another present slice still maps @folio.  The
 * RSS charge is left to the caller because it is not always dropped along
 * (migration keeps it).  Returns true when the rmap and reference were dropped.
 */
bool ppps_anon_slice_unmap(struct vm_area_struct *vma, struct folio *folio,
			   pte_t *ptep, unsigned long address)
{
	if (!ppps_anon_slice_last(vma, folio, ptep, address))
		return false;
	folio_remove_rmap_pte(folio, &folio->page, vma);
	folio_put(folio);
	return true;
}

/*
 * True when every process PTE mapping @folio in its tuple lies inside
 * [@start, @end), so an operation over that range covers the whole mapped
 * tuple the way it covers a whole native page.
 */
bool ppps_anon_tuple_within(struct vm_area_struct *vma, struct folio *folio,
			    pte_t *ptep, unsigned long address,
			    unsigned long start, unsigned long end)
{
	pte_t *base_ptep = ppps_tuple_base_ptep(vma, ptep, address);
	unsigned long base = ppps_tuple_base(vma, address);
	unsigned int i;

	if (!base_ptep)
		return true;
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		unsigned long slice_addr = base + i * PAGE_SIZE_COMPAT;

		if (ppps_anon_pte_folio(ptep_get(base_ptep + i)) == folio &&
		    (slice_addr < start || slice_addr >= end))
			return false;
	}
	return true;
}

/*
 * Migration preserves RSS while replacing every present slice with a
 * migration entry.  Fork and zap can encounter that transient tuple, so they
 * must recognize both restored present slices and migration entries which
 * still point at the same packed folio.
 */
bool ppps_anon_folio_has_other_entries(struct vm_area_struct *vma,
		struct folio *folio, pte_t *ptep, unsigned long address)
{
	pte_t *base_ptep = ppps_tuple_base_ptep(vma, ptep, address);
	unsigned int slice = vma_address_to_slice(vma, address);
	unsigned int i;

	if (!base_ptep)
		return false;
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		pte_t pte;
		swp_entry_t entry;

		if (i == slice)
			continue;
		pte = ptep_get(base_ptep + i);
		if (ppps_anon_pte_folio(pte) == folio)
			return true;
		if (pte_present(pte) || pte_none(pte))
			continue;
		entry = pte_to_swp_entry(pte);
		if (is_migration_entry(entry) &&
		    pfn_swap_entry_folio(entry) == folio)
			return true;
	}
	return false;
}

/*
 * The single packed folio which the other slices of this tuple map, or NULL
 * when the tuple is empty or maps more than one candidate.
 */
static struct folio *ppps_anon_tuple_folio(struct vm_area_struct *vma,
					   pte_t *ptep, unsigned long address)
{
	pte_t *base_ptep = ppps_tuple_base_ptep(vma, ptep, address);
	struct folio *folio = NULL;
	pgoff_t tuple_index = ppps_tuple_index(vma, address);
	unsigned int slice = vma_address_to_slice(vma, address);
	unsigned int i;

	if (!base_ptep)
		return NULL;
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		struct folio *cur;
		pte_t pte;

		if (i == slice)
			continue;
		pte = ptep_get(base_ptep + i);
		cur = ppps_anon_pte_folio(pte);
		if (!cur)
			continue;
		/*
		 * Anonymous PTEs in this VMA must use their VA-selected slice.
		 * An adjacent private file VMA can instead select its slice by
		 * file offset, so do not apply this VMA's invariant to it.
		 */
		VM_WARN_ON_ONCE(vma_is_anonymous(vma) &&
				in_range(ppps_tuple_base(vma, address) +
					 i * PAGE_SIZE_COMPAT, vma->vm_start,
					 vma->vm_end - vma->vm_start) &&
				pte_page_offset(pte) != i * PAGE_SIZE_COMPAT);
		/*
		 * The four PTE slots may straddle VMA boundaries.  Ignore a folio
		 * belonging to the adjacent VMA: each VMA may still pack its own
		 * subset of the file page.  A candidate must match the physical
		 * slice, file index and this VMA's anon_vma chain.
		 */
		if (pte_page_offset(pte) != i * PAGE_SIZE_COMPAT ||
		    cur->index != tuple_index ||
		    !ppps_anon_folio_visible_in_vma(cur, vma))
			continue;
		if (folio && folio != cur)
			return NULL;
		folio = cur;
	}
	return folio;
}

static bool ppps_anon_tuple_has_any_folio(struct vm_area_struct *vma,
					  pte_t *ptep, unsigned long address)
{
	pte_t *base_ptep = ppps_tuple_base_ptep(vma, ptep, address);
	unsigned int i;

	if (!base_ptep)
		return false;
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++)
		if (ppps_anon_pte_folio(ptep_get(base_ptep + i)))
			return true;
	return false;
}

/*
 * Detect the complete-tuple subset used by the zero-copy UFFDIO_MOVE fast
 * path.  Partial tuples remain valid compat-anonymous mappings.
 */
bool ppps_anon_tuple_is_complete(struct vm_area_struct *vma,
			     struct folio *folio, unsigned long address,
			     pte_t *ptep)
{
	unsigned long base;
	unsigned int i, slice;
	pte_t *base_ptep;

	if (!vma_is_anonymous(vma) || !folio_test_ppps_compat_anon(folio))
		return false;

	base = ppps_tuple_base(vma, address);
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
 * The run of none PTEs around @address, within the tuple and the VMA, which
 * a fresh folio can populate in one go.  Returns its length and sets @base
 * to its first address.
 */
int ppps_anon_installable_run(struct vm_area_struct *vma, pte_t *ptep,
			      unsigned long address, unsigned long *base)
{
	unsigned long tuple_base = ppps_tuple_base(vma, address);
	unsigned long lower = max(tuple_base, vma->vm_start);
	unsigned long upper = min(tuple_base + PAGE_SIZE, vma->vm_end);
	unsigned long start = address;
	unsigned long end = address + PAGE_SIZE_COMPAT;
	pte_t *first = ptep;
	pte_t *last = ptep + 1;

	*base = address;
	if (!vma_is_anonymous(vma) ||
	    !ppps_vma_address_shares_tuple(vma, address) ||
	    !pte_none(ptep_get(ptep)))
		return 1;
	while (start > lower && pte_none(ptep_get(first - 1))) {
		start -= PAGE_SIZE_COMPAT;
		first--;
	}
	while (end < upper && pte_none(ptep_get(last))) {
		end += PAGE_SIZE_COMPAT;
		last++;
	}
	*base = start;
	return (end - start) >> PAGE_SHIFT_COMPAT;
}

/*
 * Hole filling
 *
 * A missing slice of a tuple whose folio is exclusive to this mm is filled in
 * place instead of allocating a second native folio.
 */

/*
 * The one decision point for writing into a packed folio in place, shared by
 * do_wp_page(), hole filling and every other caller which wants to reuse a
 * tuple folio rather than copy it.  PG_anon_exclusive settles it directly;
 * otherwise the generic reuse test (references, swapcache, pins) runs and the
 * flag is set on success, exactly as do_wp_page() does for a native folio.
 */
bool ppps_anon_try_reuse_folio(struct folio *folio, struct vm_area_struct *vma)
{
	if (PageAnonExclusive(&folio->page))
		return true;
	if (!wp_can_reuse_anon_folio(folio, vma))
		return false;
	SetPageAnonExclusive(&folio->page);
	return true;
}

/*
 * Optimistic pre-check used before allocating for a fault: does the tuple at
 * vmf->address already hold a folio that a hole fill could reuse?  This must
 * never say yes where ppps_anon_try_reuse_folio() would say no, or a fault which
 * skipped its preallocation would retry forever; a folio the LRU batch still
 * pins or which sits in the swapcache is therefore reported as not reusable
 * and the caller simply preallocates.
 */
bool ppps_anon_tuple_has_folio_hint(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct folio *folio;
	spinlock_t *ptl; /* Protects the tuple PTEs. */
	pte_t *ptep;
	bool found;

	/* This is only a hint; drain before taking the page-table lock. */
	lru_add_drain();
	ptep = pte_offset_map_lock(vma->vm_mm, vmf->pmd, vmf->address,
				   &ptl);
	if (!ptep)
		return false;
	folio = ppps_anon_tuple_folio(vma, ptep, vmf->address);
	found = folio && (PageAnonExclusive(&folio->page) ||
			  (!folio_test_swapcache(folio) &&
			   folio_ref_count(folio) == 1 &&
			   folio_mapcount(folio) == 1));
	pte_unmap_unlock(ptep, ptl);
	return found;
}

/*
 * Return the exclusive folio which the missing slice at @address may join,
 * or NULL when a new folio is needed.  @multi_folio, when given, reports
 * whether the tuple already holds any anonymous folio, i.e. whether a new
 * allocation would make this tuple span two native folios.
 */
struct folio *ppps_anon_hole_fill_folio(struct vm_area_struct *vma,
					pte_t *ptep, unsigned long address,
					bool *multi_folio)
{
	struct folio *folio = ppps_anon_tuple_folio(vma, ptep, address);

	if (multi_folio)
		*multi_folio = folio ||
			ppps_anon_tuple_has_any_folio(vma, ptep, address);
	if (!folio || !ppps_anon_try_reuse_folio(folio, vma))
		return NULL;
	return folio;
}

void ppps_anon_clear_slice(struct vm_area_struct *vma, struct folio *folio,
			   unsigned long address)
{
	unsigned int start = vma_address_to_slice(vma, address) * PAGE_SIZE_COMPAT;
	void *kaddr = kmap_local_page(&folio->page);

	if (system_supports_mte() && page_mte_tagged(&folio->page))
		mte_zero_clear_tags_range(kaddr + start, PAGE_SIZE_COMPAT);
	else
		memset(kaddr + start, 0, PAGE_SIZE_COMPAT);
	kunmap_local(kaddr);
	flush_dcache_page(&folio->page);
	/* Make the cleared slice visible before installing its PTE. */
	smp_wmb();
}

/*
 * Copy one process-sized slice, tags included when both pages carry them.
 * @src may be a tail page of a large page-cache folio, so the page itself is
 * mapped rather than its folio head.  Callers install a PTE for the slice
 * right after this, so the data and tags are made visible first, just as
 * __folio_mark_uptodate() does for a brand-new folio.
 */
static void ppps_copy_slice_page(struct page *dst, unsigned int dst_slice,
				 struct page *src, unsigned int src_slice)
{
	void *src_addr = kmap_local_page(src);
	void *dst_addr = kmap_local_page(dst);
	void *from = src_addr + src_slice * PAGE_SIZE_COMPAT;
	void *to = dst_addr + dst_slice * PAGE_SIZE_COMPAT;

	memcpy(to, from, PAGE_SIZE_COMPAT);
	if (system_supports_mte() && page_mte_tagged(dst)) {
		if (page_mte_tagged(src))
			mte_copy_tags_range(to, from, PAGE_SIZE_COMPAT);
		else
			mte_clear_tags_range(to, PAGE_SIZE_COMPAT);
	}
	kunmap_local(dst_addr);
	kunmap_local(src_addr);
	flush_dcache_page(dst);
	/* Make the copied slice visible before installing its PTE. */
	smp_wmb();
}

void ppps_anon_copy_slice(struct folio *dst, unsigned int dst_slice,
			  struct folio *src, unsigned int src_slice)
{
	ppps_copy_slice_page(&dst->page, dst_slice, &src->page, src_slice);
}

void ppps_anon_fill_slice_from(struct folio *dst, struct vm_area_struct *vma,
			       unsigned long address, struct page *src)
{
	unsigned int slice = vma_address_to_slice(vma, address);

	ppps_copy_slice_page(&dst->page, slice, src, slice);
}

/*
 * mremap reslicing
 *
 * move_page_tables() moves PTEs verbatim.  After a move by a non-multiple of
 * the native page size, an anonymous PTE no longer maps the slice its new VA
 * selects, so the affected destination tuples are regrouped into new folios.
 */

/* Keep the old physical slice identities stable until reslicing finishes. */
struct ppps_mremap_folios {
	unsigned long nr;
	unsigned long locked;
	struct folio *folios[];
};

static int ppps_mremap_folio_cmp(const void *a, const void *b)
{
	unsigned long x = (unsigned long)*(struct folio * const *)a;
	unsigned long y = (unsigned long)*(struct folio * const *)b;

	return (x > y) - (x < y);
}

void ppps_anon_mremap_finish(struct ppps_mremap_folios *ctx)
{
	unsigned long i;

	if (!ctx)
		return;
	for (i = 0; i < ctx->locked; i++)
		folio_unlock(ctx->folios[i]);
	for (i = 0; i < ctx->nr; i++)
		folio_put(ctx->folios[i]);
	kvfree(ctx);
}

/*
 * Skip a PMD range that has no PTE table: nothing to fault in, nothing to
 * lock.  Leaves @address at the last compat page of the range so the
 * caller's increment moves on to the next PMD.
 */
static bool ppps_mremap_skip_empty_pmd(struct mm_struct *mm,
		unsigned long *address, unsigned long end)
{
	pmd_t *pmd = mm_find_pmd(mm, *address);

	if (pmd && !pmd_none(*pmd))
		return false;
	*address = min(pmd_addr_end_mm(mm, *address, end), end) -
		   PAGE_SIZE_COMPAT;
	return true;
}

/*
 * Collect or validate a source PTE without sleeping under its PTL.
 *
 * Return: 0 when done with this PTE, 1 for a swap or migration entry that
 * must be faulted in first, -EAGAIN when validation finds a present folio
 * outside the locked set.
 */
static int ppps_mremap_source(struct vm_area_struct *vma,
		unsigned long address, struct ppps_mremap_folios *ctx,
		bool validate)
{
	struct mm_struct *mm = vma->vm_mm;
	struct folio *folio;
	spinlock_t *ptl;
	pmd_t *pmd;
	pte_t *ptep, pte;
	int ret = 0;

	/*
	 * mmap_lock is write-held, so a missing PTE table is not a race:
	 * there is simply nothing at this address.
	 */
	pmd = mm_find_pmd(mm, address);
	if (!pmd || pmd_none(*pmd))
		return 0;
	ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
	if (!ptep)
		return 0;
	pte = ptep_get(ptep);
	if (is_swap_pte(pte)) {
		swp_entry_t entry = pte_to_swp_entry(pte);

		if (!non_swap_entry(entry) || is_migration_entry(entry))
			ret = 1;
	} else {
		folio = ppps_anon_pte_folio(pte);
		if (folio) {
			if (validate) {
				unsigned long lo = 0, hi = ctx->nr;

				while (lo < hi) {
					unsigned long mid = lo + (hi - lo) / 2;

					if ((unsigned long)ctx->folios[mid] <
					    (unsigned long)folio)
						lo = mid + 1;
					else
						hi = mid;
				}
				if (lo == ctx->nr || ctx->folios[lo] != folio)
					ret = -EAGAIN;
			} else {
				folio_get(folio);
				ctx->folios[ctx->nr++] = folio;
			}
		}
	}
	pte_unmap_unlock(ptep, ptl);
	return ret;
}

/* Fault a swap or migration entry in at its source address. */
static int ppps_mremap_fault_source(struct vm_area_struct *vma,
				    unsigned long address)
{
	vm_fault_t fault;

	fault = handle_mm_fault(vma, address, FAULT_FLAG_REMOTE, NULL);
	if (fault & VM_FAULT_ERROR)
		return vm_fault_to_errno(fault, 0);
	/* mmap_lock is write-held; the fault may neither drop nor retry it. */
	if (WARN_ON_ONCE(fault & (VM_FAULT_RETRY | VM_FAULT_COMPLETED)))
		return -EAGAIN;
	return 0;
}

/* Reclaim keeps evicting one page between our faults: restart instead. */
#define PPPS_MREMAP_MAX_FAULTS		16
/* Restart budget for racing folio locks and PTE changes before giving up. */
#define PPPS_MREMAP_MAX_ATTEMPTS	128

/*
 * Take a reference on every present source folio, faulting swap entries in
 * first.  Return: 0 done, 1 restart preparation, <0 fatal error.
 */
static int ppps_mremap_collect(struct vm_area_struct *vma,
		unsigned long old_addr, unsigned long len,
		struct ppps_mremap_folios *ctx)
{
	unsigned long end = old_addr + len;
	unsigned long address;

	for (address = old_addr; address < end; address += PAGE_SIZE_COMPAT) {
		unsigned int faults;
		int ret;

		if (ppps_mremap_skip_empty_pmd(vma->vm_mm, &address, end))
			continue;
		for (faults = 0; ; faults++) {
			ret = ppps_mremap_source(vma, address, ctx, false);
			if (ret != 1)
				break;
			if (faults == PPPS_MREMAP_MAX_FAULTS)
				return 1;
			ret = ppps_mremap_fault_source(vma, address);
			if (ret)
				return ret;
		}
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Sort, deduplicate and trylock the collected folios.  On a lock miss
 * return 1 with a reference to the busy folio in @wait_folio, so the caller
 * can wait for it without holding any other folio lock.
 */
static int ppps_mremap_lock_folios(struct ppps_mremap_folios *ctx,
				   struct folio **wait_folio)
{
	unsigned long i, nr = 0;

	sort(ctx->folios, ctx->nr, sizeof(*ctx->folios),
	     ppps_mremap_folio_cmp, NULL);
	for (i = 0; i < ctx->nr; i++) {
		if (nr && ctx->folios[nr - 1] == ctx->folios[i])
			folio_put(ctx->folios[i]);
		else
			ctx->folios[nr++] = ctx->folios[i];
	}
	ctx->nr = nr;

	for (i = 0; i < nr; i++) {
		if (!folio_trylock(ctx->folios[i])) {
			*wait_folio = ctx->folios[i];
			folio_get(*wait_folio);
			return 1;
		}
		ctx->locked++;
	}
	return 0;
}

/* Recheck every source PTE against the locked set.  Return: 0 or 1 restart. */
static int ppps_mremap_validate(struct vm_area_struct *vma,
		unsigned long old_addr, unsigned long len,
		struct ppps_mremap_folios *ctx)
{
	unsigned long end = old_addr + len;
	unsigned long address;

	for (address = old_addr; address < end; address += PAGE_SIZE_COMPAT) {
		if (ppps_mremap_skip_empty_pmd(vma->vm_mm, &address, end))
			continue;
		if (ppps_mremap_source(vma, address, ctx, true))
			return 1;
	}
	return 0;
}

/*
 * One preparation attempt.  Return: 0 with *@ctxp ready, 1 to restart
 * (@wait_folio set when a folio lock was missed), <0 fatal error.  The
 * context is released here on anything but success.
 */
static int ppps_mremap_attempt(struct vm_area_struct *vma,
		unsigned long old_addr, unsigned long len,
		struct ppps_mremap_folios **ctxp, struct folio **wait_folio)
{
	struct ppps_mremap_folios *ctx;
	int ret;

	ctx = kvzalloc(struct_size(ctx, folios, len >> PAGE_SHIFT_COMPAT),
		       GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ret = ppps_mremap_collect(vma, old_addr, len, ctx);
	if (!ret)
		ret = ppps_mremap_lock_folios(ctx, wait_folio);
	if (!ret)
		ret = ppps_mremap_validate(vma, old_addr, len, ctx);
	if (ret) {
		ppps_anon_mremap_finish(ctx);
		return ret;
	}
	*ctxp = ctx;
	return 0;
}

/*
 * Swap entries identify a native folio, not the original 4K slice. Fault them
 * at their source addresses before moving them. Hold all source anon folios
 * locked through move/reslice (and rollback) so reclaim and migration cannot
 * replace a present source by a swap/migration entry in between.
 *
 * Take references under PTL, sort/deduplicate, then trylock. Never wait for a
 * second folio while holding the first: other mms may share these folios.
 * Recheck the PTEs after locking; racing migration/reclaim restarts preparation.
 * mmap_lock stays write-held; no retry-capable fault may drop it.  Every
 * restart reason is transient and makes progress, so the attempt budget only
 * turns an unexpected non-transient condition into an error instead of a
 * spin under mmap_lock.
 */
struct ppps_mremap_folios *ppps_anon_mremap_prepare(struct vm_area_struct *vma,
		unsigned long old_addr, unsigned long new_addr, unsigned long len)
{
	struct ppps_mremap_folios *ctx = NULL;
	unsigned int attempts;
	int ret;

	if (!ppps_vma_shares_tuple(vma) || !len)
		return NULL;
	if (!ppps_vma_has_slices(vma) && IS_ALIGNED(old_addr, PAGE_SIZE) &&
	    IS_ALIGNED(new_addr, PAGE_SIZE) && IS_ALIGNED(len, PAGE_SIZE))
		return NULL;
	mmap_assert_write_locked(vma->vm_mm);
	vma_start_write(vma);

	for (attempts = 0; ; attempts++) {
		struct folio *wait_folio = NULL;

		ret = ppps_mremap_attempt(vma, old_addr, len, &ctx, &wait_folio);
		if (ret <= 0)
			break;
		if (wait_folio) {
			ret = folio_wait_locked_killable(wait_folio);
			folio_put(wait_folio);
			if (ret)
				break;
		}
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}
		if (WARN_ON_ONCE(attempts == PPPS_MREMAP_MAX_ATTEMPTS)) {
			ret = -EAGAIN;
			break;
		}
		cond_resched();
	}
	return ret ? ERR_PTR(ret) : ctx;
}

struct ppps_reslice_tuple {
	struct folio *folio;
	struct folio *fallback[PPPS_SLICES_PER_PAGE];
	unsigned long first;
	bool normal;
	bool boundary;
	bool zero;
};

/* The source folios drained by one destination tuple. */
struct ppps_reslice_sources {
	struct folio *folios[PPPS_SLICES_PER_PAGE];
	unsigned long bases[PPPS_SLICES_PER_PAGE];
	int nr;
};

static struct folio *ppps_anon_folio_at(struct mm_struct *mm,
					unsigned long address)
{
	struct folio *folio = NULL;
	spinlock_t *ptl; /* Protects the queried PTE. */
	pte_t *ptep;
	pmd_t *pmd;

	pmd = mm_find_pmd(mm, address);
	if (!pmd)
		return NULL;
	ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
	if (!ptep)
		return NULL;
	folio = ppps_anon_pte_folio(ptep_get(ptep));
	pte_unmap_unlock(ptep, ptl);
	return folio;
}

static bool ppps_anon_folio_at_address(struct mm_struct *mm,
		struct folio *folio, unsigned long address)
{
	return ppps_anon_folio_at(mm, address) == folio;
}

static bool ppps_anon_source_slices_remain(struct mm_struct *mm,
		struct folio *folio, unsigned long source_base,
		unsigned long old_addr, unsigned long len)
{
	unsigned long old_end = old_addr + len;
	unsigned int i;

	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		unsigned long address = source_base + i * PAGE_SIZE_COMPAT;

		if ((address < old_addr || address >= old_end) &&
		    ppps_anon_folio_at_address(mm, folio, address))
			return true;
	}
	return false;
}

/*
 * During a misaligned move, the four logical slices of one source tuple can
 * temporarily straddle two destination VA tuples.  This helper checks both
 * the unmoved source positions and the translated destination positions so
 * the old (mm, tuple) rmap/reference is removed exactly once.
 */
static bool ppps_anon_folio_still_mapped(struct mm_struct *mm,
		struct folio *folio, unsigned long source_base,
		unsigned long old_addr, unsigned long new_addr,
		unsigned long len)
{
	unsigned long old_end = old_addr + len;
	unsigned int i;

	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		unsigned long source = source_base + i * PAGE_SIZE_COMPAT;

		if (ppps_anon_folio_at_address(mm, folio, source))
			return true;
		if (source >= old_addr && source < old_end &&
		    ppps_anon_folio_at_address(mm, folio,
			new_addr + source - old_addr))
			return true;
	}
	return false;
}

/* Remember @folio, mapped by @pte, as a source drained from @source. */
static void ppps_reslice_sources_add(struct ppps_reslice_sources *sources,
		struct folio *folio, pte_t pte, unsigned long source)
{
	int i;

	for (i = 0; i < sources->nr; i++)
		if (sources->folios[i] == folio)
			return;
	folio_get(folio);
	sources->folios[sources->nr] = folio;
	sources->bases[sources->nr] = source - pte_page_offset(pte);
	sources->nr++;
}

/* Drop the (mm, tuple) rmap and reference of every fully drained source. */
static void ppps_reslice_sources_drop(struct mm_struct *mm,
		struct ppps_reslice_sources *sources, unsigned long old_addr,
		unsigned long new_addr, unsigned long len)
{
	int i;

	for (i = 0; i < sources->nr; i++) {
		struct folio *folio = sources->folios[i];
		struct vm_area_struct *vma;

		if (!ppps_anon_folio_still_mapped(mm, folio, sources->bases[i],
						  old_addr, new_addr, len)) {
			vma = find_vma(mm, sources->bases[i]);
			if (!vma || sources->bases[i] < vma->vm_start)
				vma = find_vma(mm, new_addr);
			if (!WARN_ON_ONCE(!vma)) {
				folio_remove_rmap_pte(folio, &folio->page, vma);
				add_mm_counter(mm, MM_ANONPAGES,
					       -folio_nr_pages(folio));
				folio_put(folio);
			}
		}
		folio_put(folio);
	}
}

static pte_t ppps_anon_reslice_pte(struct vm_area_struct *vma,
		struct folio *folio, pte_t old, unsigned long address)
{
	pte_t entry;

	entry = vma_pte_mkslice(vma, mk_pte(&folio->page, pte_pgprot(old)), address);
	entry = ppps_pte_inherit(entry, old);
	if (pte_write(old))
		return maybe_mkwrite(entry, vma);
	return pte_wrprotect(entry);
}

/* Re-point a zero-page PTE whose physical slice no longer matches its VA. */
static void ppps_anon_reslice_zero_slice(struct mm_struct *mm,
		struct vm_area_struct *vma, pte_t *ptep,
		unsigned long address, pte_t old)
{
	pte_t entry;

	if (!pte_present(old) || !is_zero_pfn(pte_pfn(old)) ||
	    pte_page_offset(old) ==
		vma_address_to_slice(vma, address) * PAGE_SIZE_COMPAT)
		return;
	ptep_clear_flush(vma, address, ptep);
	entry = pte_mkspecial(pfn_pte(my_zero_pfn(address), pte_pgprot(old)));
	entry = ppps_pte_inherit(entry, old);
	set_pte_at(mm, address, ptep, entry);
	update_mmu_cache(vma, address, ptep);
}

static void ppps_anon_reslice_tuple(struct mm_struct *mm,
		struct ppps_reslice_tuple *tuple, unsigned long old_addr,
		unsigned long new_addr, unsigned long len)
{
	struct ppps_reslice_sources sources = {};
	unsigned long end = min(tuple->first + PAGE_SIZE, new_addr + len);
	unsigned long start = max(tuple->first, new_addr);
	struct vm_area_struct *vmas[PPPS_SLICES_PER_PAGE] = {};
	struct folio *pte_folios[PPPS_SLICES_PER_PAGE] = {};
	unsigned long addresses[PPPS_SLICES_PER_PAGE] = {};
	pte_t old_ptes[PPPS_SLICES_PER_PAGE] = {};
	spinlock_t *ptl; /* Protects the destination tuple. */
	pte_t *ptep;
	pmd_t *pmd;
	int rmap_idx = -1;
	int nr = 0;
	int i;

	pmd = mm_find_pmd(mm, start);
	if (WARN_ON_ONCE(!pmd))
		return;
	ptep = pte_offset_map_lock(mm, pmd, start, &ptl);
	if (WARN_ON_ONCE(!ptep))
		return;

	/* Snapshot the destination slices and stop writes to their sources. */
	for (; start < end; start += PAGE_SIZE_COMPAT, nr++) {
		struct vm_area_struct *vma = find_vma(mm, start);
		pte_t pte = ptep_get(ptep + nr);

		if (WARN_ON_ONCE(!vma || start < vma->vm_start))
			continue;
		vmas[nr] = vma;
		addresses[nr] = start;
		old_ptes[nr] = pte;
		pte_folios[nr] = ppps_anon_pte_folio(pte);
		/*
		 * Without a replacement folio only the zero-page slices are
		 * re-pointed below: the packed folio slices already sit in
		 * their VA-selected slots and must stay mapped.
		 */
		if (!pte_folios[nr] || !tuple->folio)
			continue;
		if (rmap_idx < 0)
			rmap_idx = nr;
		ppps_reslice_sources_add(&sources, pte_folios[nr], pte,
					 old_addr + start - new_addr);
		ptep_set_wrprotect(mm, start, ptep + nr);
		flush_tlb_page(vma, start);
	}

	if (tuple->folio) {
		for (i = 0; i < nr; i++) {
			if (!pte_folios[i])
				continue;
			ppps_anon_copy_slice(tuple->folio,
				vma_address_to_slice(vmas[i], addresses[i]),
				pte_folios[i],
				pte_page_offset(old_ptes[i]) >> PAGE_SHIFT_COMPAT);
		}
		__folio_mark_uptodate(tuple->folio);
	}

	for (i = 0; i < nr; i++) {
		if (!vmas[i])
			continue;
		if (!pte_folios[i])
			ppps_anon_reslice_zero_slice(mm, vmas[i], ptep + i,
						     addresses[i], old_ptes[i]);
		else if (tuple->folio)
			ptep_clear_flush(vmas[i], addresses[i], ptep + i);
	}

	if (tuple->folio) {
		if (!WARN_ON_ONCE(rmap_idx < 0)) {
			add_mm_counter(mm, MM_ANONPAGES,
				       folio_nr_pages(tuple->folio));
			folio_add_new_anon_rmap(tuple->folio, vmas[rmap_idx],
						addresses[rmap_idx],
						RMAP_EXCLUSIVE);
			folio_add_lru_vma(tuple->folio, vmas[rmap_idx]);
		}
		for (i = 0; i < nr; i++) {
			pte_t entry;

			if (!pte_folios[i])
				continue;
			entry = ppps_anon_reslice_pte(vmas[i], tuple->folio,
						      old_ptes[i], addresses[i]);
			set_pte_at(mm, addresses[i], ptep + i, entry);
			update_mmu_cache(vmas[i], addresses[i], ptep + i);
		}
	}

	pte_unmap_unlock(ptep, ptl);
	ppps_reslice_sources_drop(mm, &sources, old_addr, new_addr, len);
}

/*
 * A compat tuple can straddle two PTE tables.  Such a tuple cannot share one
 * rmap/reference, and the two PTE pages are protected by different locks.
 * Reslice each populated address independently into a singleton folio
 * instead of incrementing a ptep past the end of the first table.
 */
static void ppps_anon_reslice_boundary(struct mm_struct *mm,
		struct ppps_reslice_tuple *tuple, unsigned long old_addr,
		unsigned long new_addr, unsigned long len)
{
	struct ppps_reslice_sources sources = {};
	unsigned long end = min(tuple->first + PAGE_SIZE, new_addr + len);
	unsigned long address = max(tuple->first, new_addr);

	for (; address < end; address += PAGE_SIZE_COMPAT) {
		struct vm_area_struct *vma;
		struct folio *folio;
		struct folio *dst;
		spinlock_t *ptl; /* Protects this slice's PTE table. */
		unsigned int dst_slice;
		pte_t entry, old_pte;
		pte_t *ptep;
		pmd_t *pmd;

		vma = find_vma(mm, address);
		if (WARN_ON_ONCE(!vma || address < vma->vm_start))
			continue;
		pmd = mm_find_pmd(mm, address);
		if (WARN_ON_ONCE(!pmd))
			continue;
		ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
		if (WARN_ON_ONCE(!ptep))
			continue;
		old_pte = ptep_get(ptep);
		folio = ppps_anon_pte_folio(old_pte);
		if (!folio) {
			ppps_anon_reslice_zero_slice(mm, vma, ptep, address,
						     old_pte);
			pte_unmap_unlock(ptep, ptl);
			continue;
		}
		/* Zero-page-only tuple: the folio slices are already in place. */
		if (!tuple->normal) {
			pte_unmap_unlock(ptep, ptl);
			continue;
		}

		ppps_reslice_sources_add(&sources, folio, old_pte,
					 old_addr + address - new_addr);
		dst_slice = vma_address_to_slice(vma, address);
		dst = tuple->fallback[dst_slice];
		if (WARN_ON_ONCE(!dst)) {
			pte_unmap_unlock(ptep, ptl);
			continue;
		}
		ptep_set_wrprotect(mm, address, ptep);
		flush_tlb_page(vma, address);
		ppps_anon_copy_slice(dst, dst_slice, folio,
				     pte_page_offset(old_pte) >> PAGE_SHIFT_COMPAT);
		__folio_mark_uptodate(dst);
		ptep_clear_flush(vma, address, ptep);

		add_mm_counter(mm, MM_ANONPAGES, folio_nr_pages(dst));
		folio_add_new_anon_rmap(dst, vma, address, RMAP_EXCLUSIVE);
		folio_add_lru_vma(dst, vma);
		entry = ppps_anon_reslice_pte(vma, dst, old_pte, address);
		set_pte_at(mm, address, ptep, entry);
		update_mmu_cache(vma, address, ptep);
		tuple->fallback[dst_slice] = NULL;
		pte_unmap_unlock(ptep, ptl);
	}

	ppps_reslice_sources_drop(mm, &sources, old_addr, new_addr, len);
}

/*
 * Restore the VA-selected physical slice after mremap.  Complete tuples
 * moved by a native-page multiple need no work.  A misaligned move is
 * regrouped one destination tuple at a time, so a 4K-shifted 16K source uses
 * two 16K folios rather than four singleton native folios.
 */
int ppps_anon_reslice_range(struct mm_struct *mm, unsigned long old_addr,
			     unsigned long new_addr, unsigned long len)
{
	struct vm_area_struct *first_vma;
	unsigned long first;
	unsigned long last;
	unsigned long nr_tuples;
	struct ppps_reslice_tuple *tuples;
	struct mmu_notifier_range range;
	unsigned long address;
	int ret = 0;
	int i, j;

	if (!ppps_mm_is_compat(mm) || !len)
		return 0;
	mmap_assert_write_locked(mm);
	first_vma = find_vma(mm, new_addr);
	if (!first_vma || new_addr < first_vma->vm_start)
		return -EFAULT;
	/* Preserve the established no-op fast path for complete anon tuples. */
	if (!ppps_vma_has_slices(first_vma) && IS_ALIGNED(old_addr, PAGE_SIZE) &&
	    IS_ALIGNED(new_addr, PAGE_SIZE) && IS_ALIGNED(len, PAGE_SIZE))
		return 0;
	first = ppps_tuple_base(first_vma, new_addr);
	last = ppps_tuple_base(first_vma,
				new_addr + len - PAGE_SIZE_COMPAT) + PAGE_SIZE;
	nr_tuples = (last - first) >> PAGE_SHIFT;
	tuples = kvcalloc(nr_tuples, sizeof(*tuples), GFP_KERNEL);
	if (!tuples)
		return -ENOMEM;

	for (i = 0; i < nr_tuples; i++)
		tuples[i].first = first + i * PAGE_SIZE;

	/* Decide and allocate everything before changing the first PTE. */
	for (address = new_addr; address < new_addr + len;
	     address += PAGE_SIZE_COMPAT) {
		struct ppps_reslice_tuple *tuple;
		struct vm_area_struct *vma;
		struct folio *folio;
		spinlock_t *ptl; /* Protects the inspected PTE. */
		unsigned long source, source_base;
		pte_t *ptep;
		pmd_t *pmd;
		pte_t pte;
		bool wrong = false;

		pmd = mm_find_pmd(mm, address);
		if (!pmd)
			continue;
		ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
		if (!ptep)
			continue;
		pte = ptep_get(ptep);
		folio = ppps_anon_pte_folio(pte);
		vma = find_vma(mm, address);
		if (!vma || address < vma->vm_start) {
			pte_unmap_unlock(ptep, ptl);
			ret = -EFAULT;
			goto out;
		}
		if (folio && folio_maybe_dma_pinned(folio)) {
			pte_unmap_unlock(ptep, ptl);
			ret = -EBUSY;
			goto out;
		}
		tuple = &tuples[(ppps_tuple_base(vma, address) - first) >>
				PAGE_SHIFT];
		if (!ppps_vma_address_shares_tuple(vma, address))
			tuple->boundary = true;
		if (folio) {
			source = old_addr + address - new_addr;
			source_base = source - pte_page_offset(pte);
			wrong = pte_page_offset(pte) !=
				vma_address_to_slice(vma, address) * PAGE_SIZE_COMPAT ||
				folio->index != ppps_tuple_index(vma, address) ||
				tuple->boundary;
			folio_get(folio);
		} else if (pte_present(pte) && is_zero_pfn(pte_pfn(pte)) &&
			   pte_page_offset(pte) !=
				vma_address_to_slice(vma, address) * PAGE_SIZE_COMPAT) {
			tuple->zero = true;
		} else if (pte_present(pte) && !pte_special(pte) &&
			   vma_is_anonymous(vma)) {
			/*
			 * A file VMA keeps its source slice in vm_slice_off across
			 * mremap.  Clean page-cache PTEs and generic singleton COW
			 * PTEs therefore need no reslice; only packed anonymous
			 * folios recognized above participate in tuple regrouping.
			 */
			pte_unmap_unlock(ptep, ptl);
			ret = -EINVAL;
			goto out;
		}
		pte_unmap_unlock(ptep, ptl);
		if (folio) {
			wrong |= ppps_anon_source_slices_remain(mm, folio,
					source_base, old_addr, len);
			folio_put(folio);
			if (wrong)
				tuple->normal = true;
		}
	}

	for (i = 0; i < nr_tuples; i++) {
		struct vm_area_struct *vma;
		unsigned long end, start;

		if (!tuples[i].normal)
			continue;
		start = max(tuples[i].first, new_addr);
		end = min(tuples[i].first + PAGE_SIZE, new_addr + len);
		vma = find_vma(mm, start);
		if (!vma || start < vma->vm_start) {
			ret = -EFAULT;
			goto out;
		}
		if (!tuples[i].boundary) {
			/*
			 * This folio is populated one slice at a time before its
			 * PTEs are installed, so under VM_MTE its tag storage must
			 * be initialised (__GFP_ZEROTAGS) up front.  Otherwise
			 * mte_sync_tags() would wipe the tags copied into earlier
			 * slices when the first tagged PTE is set.  Whole-folio
			 * copies (tuple COW) get their tags from copy_highpage()
			 * and do not need this.
			 */
			tuples[i].folio = ppps_folio_prealloc(mm, vma, start,
				!!(vma->vm_flags & VM_MTE));
			if (!tuples[i].folio) {
				ret = -ENOMEM;
				goto out;
			}
			continue;
		}
		for (address = start; address < end;
		     address += PAGE_SIZE_COMPAT) {
			unsigned int slice;

			vma = find_vma(mm, address);
			if (!vma || address < vma->vm_start) {
				ret = -EFAULT;
				goto out;
			}
			slice = vma_address_to_slice(vma, address);
			tuples[i].fallback[slice] =
				ppps_folio_prealloc(mm, vma, address, true);
			if (!tuples[i].fallback[slice]) {
				ret = -ENOMEM;
				goto out;
			}
		}
	}

	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm, new_addr,
				new_addr + len);
	mmu_notifier_invalidate_range_start(&range);
	for (i = 0; i < nr_tuples; i++) {
		if (!tuples[i].normal && !tuples[i].zero)
			continue;
		if (tuples[i].boundary)
			ppps_anon_reslice_boundary(mm, &tuples[i], old_addr,
						   new_addr, len);
		else
			ppps_anon_reslice_tuple(mm, &tuples[i], old_addr,
						new_addr, len);
		tuples[i].folio = NULL;
	}
	mmu_notifier_invalidate_range_end(&range);
out:
	for (i = 0; i < nr_tuples; i++) {
		if (tuples[i].folio)
			folio_put(tuples[i].folio);
		for (j = 0; j < PPPS_SLICES_PER_PAGE; j++)
			if (tuples[i].fallback[j])
				folio_put(tuples[i].fallback[j]);
	}
	kvfree(tuples);
	return ret;
}

/*
 * Fork
 *
 * Only the first slice of a tuple copied into the child takes a reference and
 * a mapcount; the remaining slices just duplicate their PTEs.
 */

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

int ppps_anon_copy_present_ptes(struct vm_area_struct *dst_vma,
				struct vm_area_struct *src_vma,
				pte_t *dst_pte, pte_t *src_pte,
				unsigned long addr, int *rss,
				struct folio *folio, struct folio **prealloc)
{
	struct folio *new_folio;
	pte_t pte = ptep_get(src_pte);

	/* Another slice of the child's tuple already dup'ed the folio. */
	if (!ppps_anon_slice_takes_ownership(dst_vma, folio, dst_pte, addr)) {
		ppps_copy_present_pte(dst_vma, src_vma, dst_pte, src_pte,
				      pte, addr);
		return 1;
	}

	folio_get(folio);
	if (likely(!folio_try_dup_anon_rmap_pte(folio, &folio->page,
						src_vma))) {
		if (!ppps_anon_folio_has_other_entries(dst_vma, folio,
						       dst_pte, addr))
			rss[MM_ANONPAGES] += folio_nr_pages(folio);
		ppps_copy_present_pte(dst_vma, src_vma, dst_pte, src_pte,
				      pte, addr);
		return 1;
	}

	/* The folio is pinned: give the child its own copy. */
	folio_put(folio);
	new_folio = *prealloc;
	if (!new_folio)
		return -EAGAIN;
	if (copy_mc_user_highpage(&new_folio->page, &folio->page, addr,
				  src_vma))
		return -EHWPOISON;
	*prealloc = NULL;
	__folio_mark_uptodate(new_folio);
	folio_add_new_anon_rmap(new_folio, dst_vma, addr, RMAP_EXCLUSIVE);
	folio_add_lru_vma(new_folio, dst_vma);
	rss[MM_ANONPAGES] += folio_nr_pages(new_folio);
	pte = vma_pte_mkslice(dst_vma, mk_pte(&new_folio->page, dst_vma->vm_page_prot), addr);
	pte = maybe_mkwrite(pte_mkdirty(pte), dst_vma);
	if (userfaultfd_pte_wp(dst_vma, ptep_get(src_pte)))
		pte = pte_mkuffd_wp(pte);
	set_pte_at(dst_vma->vm_mm, addr, dst_pte, pte);
	return 1;
}

/*
 * Whole-tuple swapin
 *
 * A 16K kernel swaps a native page in as a whole, and a packed tuple behaves
 * the same: the fault on one slice also restores the other slices of the
 * tuple which are swap PTEs for the same entry inside the faulting VMA.  This
 * keeps upstream's rule that a folio still reachable through a swap PTE is
 * never PG_anon_exclusive: the tuple either owns its swap entry completely
 * after the fault or, when slices in another VMA stay swapped, is mapped as
 * shared.
 */

/* Bitmask of the tuple's sibling slices that swap in with @address. */
static unsigned int ppps_anon_swapin_siblings(struct vm_area_struct *vma, pte_t *ptep,
				       unsigned long address, swp_entry_t entry)
{
	pte_t *base_ptep = ppps_tuple_base_ptep(vma, ptep, address);
	unsigned long base = ppps_tuple_base(vma, address);
	unsigned int slice = vma_address_to_slice(vma, address);
	unsigned int siblings = 0;
	unsigned int i;

	if (!base_ptep)
		return 0;
	for (i = 0; i < PPPS_SLICES_PER_PAGE; i++) {
		unsigned long addr = base + i * PAGE_SIZE_COMPAT;
		pte_t pte;

		if (i == slice || addr < vma->vm_start || addr >= vma->vm_end)
			continue;
		pte = ptep_get(base_ptep + i);
		if (!is_swap_pte(pte) ||
		    non_swap_entry(pte_to_swp_entry(pte)) ||
		    pte_to_swp_entry(pte).val != entry.val)
			continue;
		siblings |= 1U << i;
	}
	return siblings;
}

/*
 * Install the PTEs of @siblings next to the faulting slice, whose new PTE is
 * @pte: same page, same write permission, each slice's own soft-dirty and
 * uffd-wp state carried over from its swap PTE.
 */
static void ppps_anon_swapin_install_siblings(struct vm_area_struct *vma, struct page *page,
			      pte_t *ptep, unsigned long address,
			      unsigned int siblings, pte_t pte)
{
	struct mm_struct *mm = vma->vm_mm;
	pte_t *base_ptep = ptep - vma_address_to_slice(vma, address);
	unsigned long base = ppps_tuple_base(vma, address);
	unsigned long mask = siblings;
	unsigned int i;

	for_each_set_bit(i, &mask, PPPS_SLICES_PER_PAGE) {
		unsigned long addr = base + i * PAGE_SIZE_COMPAT;
		pte_t old = ptep_get(base_ptep + i);
		pte_t entry = mk_pte(page, vma->vm_page_prot);

		entry = vma_pte_mkslice(vma, entry, addr);
		if (pte_swp_soft_dirty(old))
			entry = pte_mksoft_dirty(entry);
		if (pte_swp_uffd_wp(old) && userfaultfd_wp(vma))
			entry = pte_mkuffd_wp(entry);
		if (pte_write(pte) && !pte_uffd_wp(entry) &&
		    !pte_needs_soft_dirty_wp(vma, entry))
			entry = pte_mkwrite(entry, vma);
		set_pte_at(mm, addr, base_ptep + i, entry);
		update_mmu_cache(vma, addr, base_ptep + i);
	}
}

/**
 * ppps_anon_swapin_begin - sample ownership and sibling swap PTEs
 * @ctx: private state, initialized here even for native faults
 * @vma: faulting VMA
 * @folio: locked swap-in folio with a temporary reference
 * @ptep: faulting swap PTE under the caller's PTL
 * @address: address of @ptep
 * @entry: swap entry to restore
 *
 * No PTE/ref/rmap/RSS changes. Keep the PTL through release and installation.
 * The caller chooses the rmap-add variant and releases a redundant temporary
 * folio reference after unlocking the folio when ownership is already held.
 */
void ppps_anon_swapin_begin(struct ppps_anon_swapin_ctx *ctx,
		struct vm_area_struct *vma, struct folio *folio,
		pte_t *ptep, unsigned long address, swp_entry_t entry)
{
	*ctx = (struct ppps_anon_swapin_ctx) {
		.compat = ppps_vma_address_shares_tuple(vma, address),
		.owner = true,
	};
	if (ctx->compat) {
		VM_BUG_ON_FOLIO(pte_present(ptep_get(ptep)), folio);
		ctx->owner = ppps_anon_slice_takes_ownership(vma, folio,
							    ptep, address);
		ctx->siblings = ppps_anon_swapin_siblings(vma, ptep, address,
							entry);
	}
}

/**
 * ppps_anon_swapin_release - release sibling swap references before reuse
 * @ctx: state sampled under the still-held PTL
 * @entry: primary swap entry, already released by swap_free_nr()
 * @exclusive: caller's candidate exclusivity, possibly cleared here
 *
 * Call after arch_swap_restore() and the primary swap_free_nr(), but before
 * folio_free_swap() or any PTE installation. Release extra swap references
 * first, then test the remaining count. Rejoining an existing tuple also
 * clears @exclusive: no rmap addition will establish PG_anon_exclusive.
 * No folio refs, rmap, RSS or PTEs are changed; native faults leave
 * @exclusive untouched.
 */
void ppps_anon_swapin_release(const struct ppps_anon_swapin_ctx *ctx,
		swp_entry_t entry, bool *exclusive)
{
	unsigned int n;

	if (!ctx->compat)
		return;
	for (n = ctx->siblings; n; n &= n - 1)
		swap_free(entry);
	/*
	 * Slices left swapped in another VMA keep the folio shared.  If this
	 * tuple already has a present slice, the caller skips rmap addition
	 * and cannot establish RMAP_EXCLUSIVE there.  Keep this swap-in
	 * read-only and let do_wp_page() establish exclusivity after the
	 * temporary swap-in reference has been dropped.
	 */
	if (*exclusive && (!ctx->owner || __swap_count(entry)))
		*exclusive = false;
}

/**
 * ppps_anon_swapin_takes_ownership - query rmap/RSS/reference ownership
 * @ctx: initialized swap-in state
 *
 * Return: true for native faults or the first present (mm, tuple, folio)
 * slice. The caller performs the matching accounting and rmap-add variant.
 */
bool ppps_anon_swapin_takes_ownership(const struct ppps_anon_swapin_ctx *ctx)
{
	return ctx->owner;
}

/**
 * ppps_anon_swapin_swap_delta - query the extra swap references released
 * @ctx: initialized swap-in state
 *
 * Return: positive sibling count, excluding the primary generic nr_pages.
 * Native faults return zero. The caller subtracts it from MM_SWAPENTS.
 */
unsigned int ppps_anon_swapin_swap_delta(const struct ppps_anon_swapin_ctx *ctx)
{
	return hweight8(ctx->siblings);
}

/**
 * ppps_anon_swapin_install - install sibling PTEs after the primary PTE
 * @ctx: sampled/released swap-in state under the still-held PTL
 * @vma: faulting VMA
 * @page: backing native page
 * @ptep: primary PTE, already installed by generic MM
 * @address: primary address
 * @pte: installed primary PTE, supplying the final write permission
 *
 * Preserve each sibling's soft-dirty and UFFD-WP state. The caller has already
 * performed RSS/rmap accounting; no extra folio references are taken. Leaves
 * all locks held. Native faults have no siblings and do nothing.
 */
void ppps_anon_swapin_install(const struct ppps_anon_swapin_ctx *ctx,
		struct vm_area_struct *vma, struct page *page, pte_t *ptep,
		unsigned long address, pte_t pte)
{
	if (ctx->siblings)
		ppps_anon_swapin_install_siblings(vma, page, ptep, address,
						 ctx->siblings, pte);
}

/*
 * Write protection faults
 */

enum ppps_anon_wp_type ppps_anon_wp_type(struct vm_area_struct *vma,
					 struct folio *folio,
					 unsigned long address, pte_t pte)
{
	if (!ppps_mm_is_compat(vma->vm_mm))
		return PPPS_ANON_WP_NONE;
	if (folio && folio_test_ppps_compat_anon(folio))
		return PPPS_ANON_WP_COMPAT;
	if (folio && !folio_test_anon(folio) &&
	    ppps_vma_address_shares_tuple(vma, address))
		return PPPS_ANON_WP_FILE_COW;
	if (!folio && vma_is_anonymous(vma) && is_zero_pfn(pte_pfn(pte)))
		return PPPS_ANON_WP_ZERO;
	return PPPS_ANON_WP_NONE;
}

/* Zero page inside a compat tuple: fill the hole or start a new tuple. */
static vm_fault_t ppps_wp_zero_tuple_copy(struct vm_fault *vmf)
{
	const bool unshare = vmf->flags & FAULT_FLAG_UNSHARE;
	struct vm_area_struct *vma = vmf->vma;
	struct mm_struct *mm = vma->vm_mm;
	unsigned long address = vmf->address;
	struct folio *tuple_folio;
	struct folio *new_folio;
	struct mmu_notifier_range range;
	spinlock_t *ptl; /* Protects the faulting PTE and tuple lookup. */
	pte_t entry;
	pte_t *ptep;
	vm_fault_t ret;

	delayacct_wpcopy_start();
	ret = vmf_anon_prepare(vmf);
	if (ret)
		goto out;

	new_folio = ppps_folio_prealloc(mm, vma, address, true);
	if (!new_folio) {
		ret = VM_FAULT_OOM;
		goto out;
	}
	__folio_mark_uptodate(new_folio);

	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm, address,
				address + PAGE_SIZE_COMPAT);
	mmu_notifier_invalidate_range_start(&range);

	ptep = pte_offset_map_lock(mm, vmf->pmd, address, &ptl);
	if (!ptep || !pte_same(ptep_get(ptep), vmf->orig_pte)) {
		if (ptep)
			pte_unmap_unlock(ptep, ptl);
		mmu_notifier_invalidate_range_end(&range);
		folio_put(new_folio);
		ret = 0;
		goto out;
	}

	tuple_folio = ppps_anon_hole_fill_folio(vma, ptep, address, NULL);
	if (tuple_folio) {
		ppps_anon_clear_slice(vma, tuple_folio, address);
		folio_put(new_folio);
		new_folio = tuple_folio;
	}
	ptep_clear_flush(vma, address, ptep);
	ksm_might_unmap_zero_page(mm, vmf->orig_pte);
	if (!tuple_folio) {
		add_mm_counter(mm, MM_ANONPAGES, folio_nr_pages(new_folio));
		folio_add_new_anon_rmap(new_folio, vma, address,
					RMAP_EXCLUSIVE);
		folio_add_lru_vma(new_folio, vma);
	}
	entry = vma_pte_mkslice(vma, mk_pte(&new_folio->page, vma->vm_page_prot), address);
	entry = ppps_pte_inherit(pte_sw_mkyoung(entry), vmf->orig_pte);
	if (!unshare)
		entry = maybe_mkwrite(pte_mkdirty(entry), vma);
	else
		entry = pte_wrprotect(entry);
	set_pte_at(mm, address, ptep, entry);
	update_mmu_cache_range(vmf, vma, address, ptep, 1);

	pte_unmap_unlock(ptep, ptl);
	mmu_notifier_invalidate_range_end(&range);
	ret = 0;
out:
	delayacct_wpcopy_end();
	return ret;
}

/*
 * Copy a whole packed tuple.  Every present slice of @old_folio in this
 * tuple moves to the copy so the (mm, tuple) keeps a single folio.
 */
static vm_fault_t ppps_wp_compat_copy(struct vm_fault *vmf,
				      struct folio *old_folio)
{
	const bool unshare = vmf->flags & FAULT_FLAG_UNSHARE;
	const int nr_ptes = PPPS_SLICES_PER_PAGE;
	struct vm_area_struct *vma = vmf->vma;
	struct mm_struct *mm = vma->vm_mm;
	unsigned long base = ppps_tuple_base(vma, vmf->address);
	unsigned int fault_slice = vma_address_to_slice(vma, vmf->address);
	struct folio *new_folio;
	struct mmu_notifier_range range;
	pte_t old_ptes[PPPS_SLICES_PER_PAGE];
	bool mapped[PPPS_SLICES_PER_PAGE] = {};
	spinlock_t *ptl; /* Protects the complete packed tuple. */
	pte_t *ptep;
	int i;

	delayacct_wpcopy_start();
	new_folio = ppps_folio_prealloc(mm, vma, vmf->address, false);
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
	if (!ptep || !pte_same(ptep_get(ptep + fault_slice),
				       vmf->orig_pte)) {
		if (ptep)
			pte_unmap_unlock(ptep, ptl);
		mmu_notifier_invalidate_range_end(&range);
		folio_put(new_folio);
		folio_put(old_folio);
		delayacct_wpcopy_end();
		return 0;
	}

	for (i = 0; i < nr_ptes; i++) {
		old_ptes[i] = ptep_get(ptep + i);
		mapped[i] = ppps_anon_pte_folio(old_ptes[i]) == old_folio;
	}
	if (!mapped[fault_slice]) {
		pte_unmap_unlock(ptep, ptl);
		mmu_notifier_invalidate_range_end(&range);
		folio_put(new_folio);
		folio_put(old_folio);
		delayacct_wpcopy_end();
		return 0;
	}
	flush_cache_range(vma, max(base, vma->vm_start),
			  min(base + PAGE_SIZE, vma->vm_end));
	for (i = 0; i < nr_ptes; i++) {
		unsigned long address;

		if (!mapped[i])
			continue;
		address = base + i * PAGE_SIZE_COMPAT;
		ptep_clear_flush(vma, address, ptep + i);
	}

	/*
	 * The copy is packed because its source was, whatever this VMA's
	 * flags say today; mark it before the rmap picks a folio index.
	 */
	folio_set_ppps_compat_anon(new_folio);
	folio_add_new_anon_rmap(new_folio, vma, vmf->address, RMAP_EXCLUSIVE);
	folio_add_lru_vma(new_folio, vma);

	for (i = 0; i < nr_ptes; i++) {
		unsigned long address = base + i * PAGE_SIZE_COMPAT;
		pte_t entry;

		if (!mapped[i])
			continue;
		/*
		 * Preserve per-slice protection across VMA splits without
		 * requiring the mmap lock in the per-VMA-lock fault path.
		 */
		entry = pte_mkslice(mk_pte(&new_folio->page, pte_pgprot(old_ptes[i])), i);
		entry = ppps_pte_inherit(entry, old_ptes[i]);
		if (i == fault_slice && !unshare)
			entry = maybe_mkwrite(pte_mkdirty(entry), vma);
		else if (!pte_write(old_ptes[i]))
			entry = pte_wrprotect(entry);

		set_pte_at(mm, address, ptep + i, entry);
		update_mmu_cache_range(vmf, vma, address, ptep + i, 1);
	}

	/* Every mapped slice of @old_folio was cleared above. */
	ppps_anon_slice_unmap(vma, old_folio, ptep + fault_slice,
			      base + fault_slice * PAGE_SIZE_COMPAT);
	pte_unmap_unlock(ptep, ptl);
	mmu_notifier_invalidate_range_end(&range);

	free_swap_cache(old_folio);
	folio_put(old_folio);
	delayacct_wpcopy_end();
	return 0;
}

/*
 * Replace one private file PTE with a slice of an already existing compat
 * anonymous tuple.  The tuple owns one rmap/reference/RSS charge for the
 * whole (mm, tuple), so adding another slice changes only the file RSS and
 * file-page rmap for the PTE being replaced.  Falls back to wp_page_copy()
 * when no exclusive tuple folio exists.
 */
static vm_fault_t ppps_file_cow_wp(struct vm_fault *vmf,
				   struct folio *src_folio)
{
	struct vm_area_struct *vma = vmf->vma;
	struct mm_struct *mm = vma->vm_mm;
	struct mmu_notifier_range range;
	struct folio *tuple_folio;
	spinlock_t *ptl; /* Protects the faulting PTE and tuple lookup. */
	pte_t old_pte, entry;
	pte_t *ptep;
	vm_fault_t ret;
	bool multi_folio;

	if (WARN_ON_ONCE(vmf->flags & FAULT_FLAG_UNSHARE))
		return wp_page_copy(vmf);

	delayacct_wpcopy_start();
	ret = vmf_anon_prepare(vmf);
	if (ret)
		goto out_put;

	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm,
				vmf->address, vmf->address + PAGE_SIZE_COMPAT);
	mmu_notifier_invalidate_range_start(&range);

	ptep = pte_offset_map_lock(mm, vmf->pmd, vmf->address, &ptl);
	if (!ptep || !pte_same(ptep_get(ptep), vmf->orig_pte)) {
		if (ptep)
			pte_unmap_unlock(ptep, ptl);
		mmu_notifier_invalidate_range_end(&range);
		ret = 0;
		goto out_put;
	}

	tuple_folio = ppps_anon_hole_fill_folio(vma, ptep, vmf->address,
						&multi_folio);
	if (!tuple_folio) {
		pte_unmap_unlock(ptep, ptl);
		mmu_notifier_invalidate_range_end(&range);
		delayacct_wpcopy_end();
		ret = wp_page_copy(vmf);
		if (!ret)
			count_vm_event(multi_folio ? PPPS_FILE_COW_MULTI_FOLIO :
						     PPPS_FILE_COW_ALLOC);
		return ret;
	}

	old_pte = ptep_get(ptep);
	ppps_anon_fill_slice_from(tuple_folio, vma, vmf->address, vmf->page);
	flush_cache_page(vma, vmf->address, pte_pfn(old_pte));
	ptep_clear_flush(vma, vmf->address, ptep);

	dec_mm_counter(mm, mm_counter_file(src_folio));
	entry = vma_pte_mkslice(vma, mk_pte(&tuple_folio->page, vma->vm_page_prot), vmf->address);
	entry = pte_sw_mkyoung(maybe_mkwrite(pte_mkdirty(entry), vma));
	entry = ppps_pte_inherit(entry, old_pte);
	set_pte_at(mm, vmf->address, ptep, entry);
	update_mmu_cache_range(vmf, vma, vmf->address, ptep, 1);
	folio_remove_rmap_pte(src_folio, vmf->page, vma);
	count_vm_event(PPPS_FILE_COW_FILL);

	pte_unmap_unlock(ptep, ptl);
	mmu_notifier_invalidate_range_end(&range);
	ret = 0;
out_put:
	folio_put(src_folio);
	delayacct_wpcopy_end();
	return ret;
}

/*
 * Called from do_wp_page() with the old folio referenced and the PTE lock
 * dropped, exactly like wp_page_copy().
 */
vm_fault_t ppps_anon_wp_copy(struct vm_fault *vmf, struct folio *folio,
			     enum ppps_anon_wp_type type)
{
	switch (type) {
	case PPPS_ANON_WP_COMPAT:
		return ppps_wp_compat_copy(vmf, folio);
	case PPPS_ANON_WP_ZERO:
		return ppps_wp_zero_tuple_copy(vmf);
	case PPPS_ANON_WP_FILE_COW:
		return ppps_file_cow_wp(vmf, folio);
	default:
		return wp_page_copy(vmf);
	}
}

/**
 * ppps_anon_unmap_begin - initialize one folio/VMA reverse-map walk
 * @ctx: private state, reinitialized for every walk
 * @vma: VMA protected by the caller's rmap lock
 * @folio: locked folio with a reference held by the caller
 * @address: notifier start, narrowed for compat anonymous folios
 * @end: notifier end, narrowed with @address
 *
 * No PTE, reference or accounting changes. Native walks retain their range
 * and the neutral last=true result, including paths which bypass PTEs.
 */
void ppps_anon_unmap_begin(struct ppps_anon_unmap_ctx *ctx,
		struct vm_area_struct *vma, struct folio *folio,
		unsigned long *address, unsigned long *end)
{
	*ctx = (struct ppps_anon_unmap_ctx) {
		.compat = ppps_mm_is_compat(vma->vm_mm) &&
			  folio_test_ppps_compat_anon(folio),
		.last = true,
	};
	if (ctx->compat) {
		*address = ppps_tuple_base(vma, *address);
		/* Sharing write-protects every alias, including adjacent VMAs. */
		*end = *address + PAGE_SIZE;
	}
}

/**
 * ppps_anon_unmap_sample - sample exclusivity before clearing a PTE
 * @ctx: state of the current folio/VMA walk
 * @folio: folio containing @subpage
 * @subpage: native subpage selected by the present PTE
 * @pvmw: current locked PTE walk, including the VMA and address
 *
 * Caller holds the tuple PTL. Write-protect all present aliases before the
 * first share; no rmap, reference or counter changes. After the first
 * successful share, retain the pre-share answer for its siblings.
 * Across VMA callbacks, recover that answer from migration entries of this
 * same folio under the tuple PTL, without sharing the folio a second time.
 * Ordinary swap entries do not establish migration exclusivity.
 *
 * Return: exclusivity to encode in this slice's swap/migration entry.
 */
bool ppps_anon_unmap_sample(struct ppps_anon_unmap_ctx *ctx,
		struct folio *folio, struct page *subpage,
		struct page_vma_mapped_walk *pvmw)
{
	pte_t *base_ptep;
	unsigned int i;

	if (!ctx->compat)
		return folio_test_anon(folio) && PageAnonExclusive(subpage);
	VM_BUG_ON_FOLIO(subpage != &folio->page, folio);
	if (ctx->shared)
		return ctx->exclusive;
	ctx->exclusive = PageAnonExclusive(&folio->page);
	if (ctx->exclusive) {
		unsigned long base = ppps_tuple_base(pvmw->vma, pvmw->address);
		bool changed = false;

		/*
		 * Every slice shares PageAnonExclusive.  GUP-fast must see
		 * a PTE change (or a read-only, nonexclusive mapping) before
		 * the caller shares the folio.  Keep these PTEs read-only
		 * even if this walk aborts after sharing an earlier slice.
		 */
		base_ptep = ppps_tuple_base_ptep(pvmw->vma, pvmw->pte,
						pvmw->address);
		for (i = 0; base_ptep && i < PPPS_SLICES_PER_PAGE; i++) {
			pte_t pte = ptep_get(base_ptep + i);

			if (ppps_anon_pte_folio(pte) != folio || !pte_write(pte))
				continue;
			ptep_set_wrprotect(pvmw->vma->vm_mm,
					   base + i * PAGE_SIZE_COMPAT,
					   base_ptep + i);
			changed = true;
		}
		if (changed)
			flush_tlb_range(pvmw->vma, base, base + PAGE_SIZE);
		return true;
	}

	/* A preceding VMA may already have shared this tuple for migration. */
	base_ptep = ppps_tuple_base_ptep(pvmw->vma, pvmw->pte, pvmw->address);
	for (i = 0; base_ptep && i < PPPS_SLICES_PER_PAGE; i++) {
		pte_t pte = ptep_get(base_ptep + i);
		swp_entry_t entry;

		if (pte_present(pte) || pte_none(pte))
			continue;
		entry = pte_to_swp_entry(pte);
		if (!is_migration_entry(entry) ||
		    pfn_swap_entry_folio(entry) != folio)
			continue;
		ctx->exclusive = !is_readable_migration_entry(entry);
		ctx->shared = true;
		break;
	}
	return ctx->exclusive;
}

/**
 * ppps_anon_unmap_clear - record ownership after clearing a PTE
 * @ctx: state of the current folio/VMA walk
 * @vma: VMA of the cleared PTE
 * @folio: folio formerly mapped by the PTE
 * @ptep: cleared PTE, still under the caller's PTL
 * @address: address of @ptep
 *
 * Only records whether this was the last present slice of (mm, tuple, folio).
 * The caller still owns PTE replacement/restoration, RSS and the successful
 * remove_rmap -> mlock_drain_local -> folio_put tail. On abort, restore the
 * PTE and discard this result: do not execute that successful tail.
 */
void ppps_anon_unmap_clear(struct ppps_anon_unmap_ctx *ctx,
		struct vm_area_struct *vma, struct folio *folio,
		pte_t *ptep, unsigned long address)
{
	if (ctx->compat) {
		VM_BUG_ON_FOLIO(!pte_none(ptep_get(ptep)), folio);
		ctx->last = ppps_anon_slice_last(vma, folio, ptep, address);
	}
}

/**
 * ppps_anon_unmap_last - query the successful clear's rmap/RSS ownership
 * @ctx: state sampled by ppps_anon_unmap_clear(), not an aborted clear
 *
 * Return: true for native walks, or the last present compat slice.
 */
bool ppps_anon_unmap_last(const struct ppps_anon_unmap_ctx *ctx)
{
	return ctx->last;
}

/**
 * ppps_anon_unmap_can_batch - test whether native batching is permitted
 * @ctx: initialized walk state
 *
 * Compat aliases share one rmap/reference. Invalidate each alias synchronously
 * before the last one releases it. This also excludes native PTE batching.
 * No changes to PTEs, locks, references or counters.
 *
 * Return: true for native walks only.
 */
bool ppps_anon_unmap_can_batch(const struct ppps_anon_unmap_ctx *ctx)
{
	return !ctx->compat;
}

/**
 * ppps_anon_unmap_needs_share - decide whether to run the rmap share step
 * @ctx: current walk state, with exclusivity already sampled under the PTL
 *
 * Return: true for native walks or a tuple not yet successfully shared.
 * Caller must also test its sampled anon_exclusive value. No side effects.
 */
bool ppps_anon_unmap_needs_share(const struct ppps_anon_unmap_ctx *ctx)
{
	return !ctx->compat || !ctx->shared;
}

/**
 * ppps_anon_unmap_commit_share - commit the successful rmap share step
 * @ctx: current walk state
 *
 * Call under the PTL, after clearing the PTE and successfully completing the
 * conditional folio_try_share_anon_rmap_pte(). Never call on its abort path.
 * Only updates private state; the caller owns all PTE/ref/rmap/RSS changes.
 */
void ppps_anon_unmap_commit_share(struct ppps_anon_unmap_ctx *ctx)
{
	if (ctx->compat)
		ctx->shared = true;
}

/**
 * ppps_anon_fault_anon_prepare - prepare a missing anonymous slice/run
 * @vmf: revalidated fault with the PTE mapped and PTL held
 * @foliop: charged, uptodate preallocation; replaced on reuse
 * @nr_pages: process-PTE count, adjusted for a new compat run
 * @addr: run start, adjusted with vmf->pte for a new compat run
 *
 * Does not install PTEs, alter RSS/rmap/LRU, or release the PTL. On reuse,
 * clears the selected slice and puts the preallocation; the existing tuple's
 * reference/rmap/RSS continue to own the returned folio. Otherwise the caller
 * still owns its preallocation and must establish reference/rmap/RSS/LRU.
 * Both results continue to generic PTE installation. Native inputs unchanged.
 *
 * Return: true if an existing folio was reused, not "fault handled".
 */
bool ppps_anon_fault_anon_prepare(struct vm_fault *vmf, struct folio **foliop,
		int *nr_pages, unsigned long *addr)
{
	struct vm_area_struct *vma = vmf->vma;
	struct folio *folio;

	if (!ppps_mm_is_compat(vma->vm_mm))
		return false;
	lockdep_assert_held(vmf->ptl);
	VM_BUG_ON(pte_present(ptep_get(vmf->pte)));
	folio = ppps_anon_hole_fill_folio(vma, vmf->pte, *addr, NULL);
	if (folio) {
		ppps_anon_clear_slice(vma, folio, *addr);
		folio_put(*foliop);
		*foliop = folio;
		return true;
	}
	*nr_pages = ppps_anon_installable_run(vma, vmf->pte, *addr, addr);
	VM_BUG_ON(*nr_pages < 1 || *nr_pages > PPPS_SLICES_PER_PAGE);
	vmf->pte -= (vmf->address - *addr) >> PAGE_SHIFT_COMPAT;
	return false;
}

static bool ppps_anon_is_file_cow(struct vm_fault *vmf)
{
	return (vmf->flags & FAULT_FLAG_WRITE) &&
		!(vmf->vma->vm_flags & VM_SHARED) &&
		!vma_is_anonymous(vmf->vma) &&
		ppps_vma_address_shares_tuple(vmf->vma, vmf->address);
}

/**
 * ppps_anon_file_cow_no_prealloc - recognize a deferred COW allocation
 * @vmf: file fault, before taking the PTL
 *
 * No PTE, lock, reference or counter changes. This is not permission to reuse
 * a tuple; the event hook must recheck under the PTL.
 *
 * Return: true when this compat file COW skipped its preallocation.
 */
bool ppps_anon_file_cow_no_prealloc(struct vm_fault *vmf)
{
	return ppps_anon_is_file_cow(vmf) && !vmf->cow_page;
}

/**
 * ppps_anon_fault_file_cow - finish a compat private-file COW slice
 * @vmf: revalidated missing PTE with PTL held and source file folio locked
 * @foliop: preallocated COW folio, or source folio if allocation was deferred
 * @ret: fault result, written only when the event is handled
 *
 * A reused tuple keeps its rmap/ref/RSS. Fill the slice, install its PTE, put
 * any unused preallocation, clear cow_page and return the final tuple folio
 * through @foliop for the caller's unlock/mlock epilogue. If revalidation
 * cannot reuse and cow_page is NULL, end this fault with VM_FAULT_NOPAGE;
 * leave the source and temporary-reference cleanup to the caller.
 *
 * A supplied preallocation instead starts a new tuple through the existing
 * set_pte_range() primitive, followed by the original RSS/event accounting.
 * Its allocation reference becomes the mapping reference; cow_page remains
 * unchanged. No PTL or source-folio lock is released by any result.
 *
 * Return: false for an unhandled/native event, with all inputs unchanged;
 * true with *@ret == 0 after installation, or VM_FAULT_NOPAGE without it.
 */
bool ppps_anon_fault_file_cow(struct vm_fault *vmf, struct folio **foliop,
		vm_fault_t *ret)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = vmf->address;
	struct folio *tuple_folio;
	bool multi_folio;
	pte_t entry;

	if (!ppps_anon_is_file_cow(vmf))
		return false;
	lockdep_assert_held(vmf->ptl);
	VM_BUG_ON(pte_present(ptep_get(vmf->pte)));
	tuple_folio = ppps_anon_hole_fill_folio(vma, vmf->pte, addr,
						&multi_folio);
	if (tuple_folio) {
		ppps_anon_fill_slice_from(tuple_folio, vma, addr, vmf->page);
		flush_icache_pages(vma, &tuple_folio->page, 1);
		entry = mk_pte(&tuple_folio->page, vma->vm_page_prot);
		entry = vma_pte_mkslice(vma, entry, addr);
		entry = pte_sw_mkyoung(maybe_mkwrite(pte_mkdirty(entry), vma));
		if (unlikely(vmf_orig_pte_uffd_wp(vmf)))
			entry = pte_mkuffd_wp(entry);
		set_pte_at(vma->vm_mm, addr, vmf->pte, entry);
		update_mmu_cache_range(vmf, vma, addr, vmf->pte, 1);
		count_vm_event(PPPS_FILE_COW_FILL);
		if (vmf->cow_page)
			folio_put(*foliop);
		vmf->cow_page = NULL;
		*foliop = tuple_folio;
	} else if (!vmf->cow_page) {
		*ret = VM_FAULT_NOPAGE;
		return true;
	} else {
		set_pte_range(vmf, *foliop, vmf->cow_page, 1, addr);
		add_mm_counter(vma->vm_mm, MM_ANONPAGES, 1);
		count_vm_event(multi_folio ? PPPS_FILE_COW_MULTI_FOLIO :
					    PPPS_FILE_COW_ALLOC);
	}
	*ret = 0;
	return true;
}
