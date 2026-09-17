// SPDX-License-Identifier: GPL-2.0-only
/* PPPS file-page sharing snapshots. No proc formatting or accounting policy. */
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>

#include "internal.h"
#include "ppps.h"

/* Statistics remain snapshots, as with the ordinary native mapcount. */
bool ppps_file_fast_counts(struct page *page,
				  struct ppps_file_counts *counts)
{
	int compat = ppps_file_pte_refs(page);
	int mapcount;

	if (compat < 0 || compat > 1)
		return false;
	mapcount = folio_precise_page_mapcount(page_folio(page), page);
	/* Pair with compat additions before, and removals after, rmap updates. */
	smp_rmb();
	if (ppps_file_pte_refs(page) != compat || mapcount < compat)
		return false;
	*counts = (struct ppps_file_counts) {
		.mapcount = mapcount,
		.compat = compat,
	};
	return true;
}

/* Called under the PTL: batch pages with an unambiguous sharing count. */
bool ppps_file_pte_needs_rmap(struct vm_area_struct *vma,
		unsigned long addr, pte_t pte, struct ppps_file_counts *counts)
{
	struct page *page;

	*counts = (struct ppps_file_counts) { .mapcount = -1 };
	if (!IS_ENABLED(CONFIG_ARM64_PER_PROCESS_PAGE_SIZE) || !vma->vm_file ||
	    !pte_present(pte))
		return false;
	page = vm_normal_page(vma, addr, pte);
	if (!page || folio_test_anon(page_folio(page)))
		return false;
	if (!ppps_file_fast_counts(page, counts))
		return true;
	/* Our present PTE must be included in the corresponding count. */
	if (ppps_mm_is_compat(vma->vm_mm))
		return counts->compat != 1;
	return counts->compat && counts->mapcount <= counts->compat;
}

/*
 * File PTEs for different PPPS slices increment the same native page
 * mapcount.  Build per-slice counts from the file reverse mappings so PSS
 * does not treat disjoint slices as aliases.  The caller drops its PTL before
 * this walk; page_vma_mapped_walk() takes the PTLs of every mapped VMA.
 */
struct ppps_file_mapcounts {
	struct page *page;
	int count[PPPS_SLICES_PER_PAGE];
};

static bool ppps_file_mapcount_one(struct folio *folio,
				   struct vm_area_struct *vma,
				   unsigned long address, void *arg)
{
	struct ppps_file_mapcounts *mapcounts = arg;
	pgoff_t pgoff = folio->index +
			folio_page_idx(folio, mapcounts->page);
	struct page_vma_mapped_walk pvmw = {
		.pfn = page_to_pfn(mapcounts->page),
		.nr_pages = 1,
		.pgoff = pgoff,
		.vma = vma,
		.address = page_address_in_vma(folio, mapcounts->page, vma),
	};
	unsigned int slice;

	if (pvmw.address == -EFAULT)
		return true;

	while (page_vma_mapped_walk(&pvmw)) {
		if (ppps_mm_is_compat(vma->vm_mm)) {
			slice = vma_address_to_slice(vma, pvmw.address);
			mapcounts->count[slice]++;
		} else {
			for (slice = 0; slice < PPPS_SLICES_PER_PAGE; slice++)
				mapcounts->count[slice]++;
		}
	}

	return true;
}

void ppps_file_page_mapcounts(struct folio *folio, struct page *page,
				     int count[PPPS_SLICES_PER_PAGE])
{
	struct ppps_file_mapcounts mapcounts = {
		.page = page,
	};
	struct rmap_walk_control rwc = {
		.arg = &mapcounts,
		.rmap_one = ppps_file_mapcount_one,
	};

	if (folio_mapcount(folio) == 1) {
		unsigned int slice;

		for (slice = 0; slice < PPPS_SLICES_PER_PAGE; slice++)
			count[slice] = 1;
		return;
	}
	folio_lock(folio);
	if (folio_mapping(folio))
		rmap_walk(folio, &rwc);
	else {
		int mapcount = max(folio_precise_page_mapcount(folio, page), 1);
		unsigned int slice;

		/*
		 * Driver RAM has no page-cache reverse mapping.  Its native-page
		 * count is conservative for slices, not an exact slice count, but
		 * must not turn shared backing into an exclusive page.  Do not use
		 * the aggregate mapcount of an entire large folio here.
		 */
		for (slice = 0; slice < PPPS_SLICES_PER_PAGE; slice++)
			mapcounts.count[slice] = mapcount;
	}
	folio_unlock(folio);

	memcpy(count, mapcounts.count, sizeof(mapcounts.count));
}

void ppps_file_pte_cache_put(struct ppps_file_pte_cache *cache)
{
	if (cache->folio)
		folio_put(cache->folio);
}

enum ppps_file_snapshot_result
ppps_file_pte_snapshot(struct vm_area_struct *vma, pmd_t *pmd,
		unsigned long addr, struct ppps_file_pte_cache *cache,
		struct ppps_file_snapshot *snapshot)
{
	struct folio *folio;
	struct page *page;
	unsigned int slice;

	snapshot->ptep = pte_offset_map_lock(vma->vm_mm, pmd, addr,
					     &snapshot->ptl);
	if (!snapshot->ptep)
		return PPPS_FILE_RETRY;
	snapshot->entry = ptep_get(snapshot->ptep);
	snapshot->mapcount = -1;
	page = pte_present(snapshot->entry) ?
		vm_normal_page(vma, addr, snapshot->entry) : NULL;
	if (!page || folio_test_anon(page_folio(page)))
		return PPPS_FILE_GENERIC;

	folio = page_folio(page);
	if (page != cache->page) {
		if (!folio_try_get(folio))
			return PPPS_FILE_GENERIC;
		ppps_file_pte_cache_put(cache);
		cache->folio = folio;
		cache->page = page;
		pte_unmap_unlock(snapshot->ptep, snapshot->ptl);
		ppps_file_page_mapcounts(folio, page, cache->mapcounts);
	} else {
		pte_unmap_unlock(snapshot->ptep, snapshot->ptl);
	}

	snapshot->ptep = NULL;
	snapshot->page = page;
	if (ppps_mm_is_compat(vma->vm_mm)) {
		snapshot->mapcount = max(cache->mapcounts[
					vma_address_to_slice(vma, addr)], 1);
	} else {
		/* A native pagemap entry is exclusive only if all slices are. */
		snapshot->mapcount = 1;
		for (slice = 0; slice < PPPS_SLICES_PER_PAGE; slice++)
			snapshot->mapcount = max(snapshot->mapcount,
						cache->mapcounts[slice]);
	}
	return PPPS_FILE_READY;
}
