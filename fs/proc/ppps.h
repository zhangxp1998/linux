/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _PROC_PPPS_H
#define _PROC_PPPS_H

#include <linux/mm.h>
#include <linux/ppps.h>

/* Private proc snapshot interface, not a module or userspace ABI. */
struct ppps_file_counts {
	int mapcount;
	int compat;
};

struct ppps_file_pte_cache {
	struct folio *folio; /* Owns a reference until cache_put(). */
	struct page *page;
	int mapcounts[PPPS_SLICES_PER_PAGE];
};

struct ppps_file_snapshot {
	pte_t entry;
	struct page *page;
	int mapcount;
	pte_t *ptep;
	spinlock_t *ptl;
};

/*
 * GENERIC: ptep remains mapped/locked; caller performs native handling and
 *          unmaps/unlocks it. mapcount is -1 (no override).
 * READY:   PTL dropped, entry/counts are a snapshot, page is kept alive by
 *          cache's reference. Counts are clamped to the observed mapping.
 * RETRY:   no PTE mapping or PTL; caller discards this walk's partial stats.
 * Caller holds mmap/VMA protection, and starts with an empty cache. Never
 * enter the slow snapshot or page_mapcounts() while holding a PTL.
 */
enum ppps_file_snapshot_result {
	PPPS_FILE_GENERIC,
	PPPS_FILE_READY,
	PPPS_FILE_RETRY,
};

#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
int ppps_swap_mapcount(swp_entry_t entry);
bool ppps_file_fast_counts(struct page *page, struct ppps_file_counts *counts);
bool ppps_file_pte_needs_rmap(struct vm_area_struct *vma,
		unsigned long addr, pte_t pte, struct ppps_file_counts *counts);
void ppps_file_page_mapcounts(struct folio *folio, struct page *page,
		int count[PPPS_SLICES_PER_PAGE]);
void ppps_file_pte_cache_put(struct ppps_file_pte_cache *cache);
enum ppps_file_snapshot_result
ppps_file_pte_snapshot(struct vm_area_struct *vma, pmd_t *pmd,
		unsigned long addr, struct ppps_file_pte_cache *cache,
		struct ppps_file_snapshot *snapshot);
#else
static inline int ppps_swap_mapcount(swp_entry_t entry)
{
	return swp_swapcount(entry);
}

static inline bool ppps_file_pte_needs_rmap(struct vm_area_struct *vma,
		unsigned long addr, pte_t pte, struct ppps_file_counts *counts)
{
	*counts = (struct ppps_file_counts) { .mapcount = -1 };
	return false;
}
#endif
#endif /* _PROC_PPPS_H */
