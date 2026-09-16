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

#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#include "internal.h"
#include "ppps.h"
#include "swap.h"

/*
 * Tuple geometry
 *
 * vma_address_to_slice() selects a compat anonymous slice by virtual address.
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
	return linear_page_index(vma, address) -
		vma_address_to_slice(vma, address);
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
	/* Enable only with the complete generic-MM lifecycle. */
	return false;
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
		 * subset of the tuple.  A candidate must match the physical
		 * slice, tuple index and this VMA's anon_vma chain.
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
	if (!ppps_vma_shares_tuple(vma) || !pte_none(ptep_get(ptep)))
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
	return false;
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
 * or NULL when a new folio is needed.
 */
struct folio *ppps_anon_hole_fill_folio(struct vm_area_struct *vma,
					pte_t *ptep, unsigned long address, bool *multi_folio)
{
	struct folio *folio = ppps_anon_tuple_folio(vma, ptep, address);

	/* Private-file COW starts using this output at activation. */
	if (multi_folio)
		*multi_folio = !!folio;

	if (!folio || !ppps_anon_try_reuse_folio(folio, vma))
		return NULL;
	return folio;
}

void ppps_anon_clear_slice(struct vm_area_struct *vma, struct folio *folio,
			   unsigned long address)
{
	unsigned int start = vma_address_to_slice(vma, address) * PAGE_SIZE_COMPAT;
	void *kaddr = kmap_local_page(&folio->page);

	memset(kaddr + start, 0, PAGE_SIZE_COMPAT);
	kunmap_local(kaddr);
	flush_dcache_page(&folio->page);
	/* Make the cleared slice visible before installing its PTE. */
	smp_wmb();
}

/*
 * Copy one process-sized slice.  Callers install a PTE for the slice right
 * after this, so the data is made visible first, just as
 * __folio_mark_uptodate() does for a brand-new folio.
 */
void ppps_anon_copy_slice(struct folio *dst, unsigned int dst_slice,
			  struct folio *src, unsigned int src_slice)
{
	void *src_addr = kmap_local_folio(src, 0);
	void *dst_addr = kmap_local_folio(dst, 0);
	void *from = src_addr + src_slice * PAGE_SIZE_COMPAT;
	void *to = dst_addr + dst_slice * PAGE_SIZE_COMPAT;

	memcpy(to, from, PAGE_SIZE_COMPAT);
	kunmap_local(dst_addr);
	kunmap_local(src_addr);
	flush_dcache_folio(dst);
	/* Make the copied slice visible before installing its PTE. */
	smp_wmb();
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
	unsigned long first;
	bool normal;
	bool zero;
};

/* The source folios drained by one destination tuple. */
struct ppps_reslice_sources {
	struct folio *folios[PPPS_SLICES_PER_PAGE];
	unsigned long bases[PPPS_SLICES_PER_PAGE];
	int nr;
};
