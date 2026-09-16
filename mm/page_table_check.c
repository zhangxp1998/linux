// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2021, Google LLC.
 * Pasha Tatashin <pasha.tatashin@soleen.com>
 */
#include <linux/kstrtox.h>
#include <linux/mm.h>
#include <linux/page_table_check.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/ppps.h>

#undef pr_fmt
#define pr_fmt(fmt)	"page_table_check: " fmt

struct page_table_check {
	/* PPPS PTEs can map disjoint process-page slices of one native page. */
	atomic_t anon_map_count[PPPS_SLICES_PER_PAGE];
	atomic_t file_map_count[PPPS_SLICES_PER_PAGE];
};

static bool __page_table_check_enabled __initdata =
				IS_ENABLED(CONFIG_PAGE_TABLE_CHECK_ENFORCED);

DEFINE_STATIC_KEY_TRUE(page_table_check_disabled);
EXPORT_SYMBOL(page_table_check_disabled);

static int __init early_page_table_check_param(char *buf)
{
	return kstrtobool(buf, &__page_table_check_enabled);
}

early_param("page_table_check", early_page_table_check_param);

static bool __init need_page_table_check(void)
{
	return __page_table_check_enabled;
}

static void __init init_page_table_check(void)
{
	if (!__page_table_check_enabled)
		return;
	static_branch_disable(&page_table_check_disabled);
}

struct page_ext_operations page_table_check_ops = {
	.size = sizeof(struct page_table_check),
	.need = need_page_table_check,
	.init = init_page_table_check,
	.need_shared_flags = false,
};

static struct page_table_check *get_page_table_check(struct page_ext *page_ext)
{
	BUG_ON(!page_ext);
	return page_ext_data(page_ext, &page_table_check_ops);
}

static unsigned int page_table_check_pte_slice(pte_t pte)
{
	return pte_page_offset(pte) >> PAGE_SHIFT_COMPAT;
}

/*
 * An entry is removed from the page table, decrement the counters for that page
 * verify that it is of correct type and counters do not become negative.
 */
static void page_table_check_clear(unsigned long pfn, unsigned long pgcnt,
				   unsigned int slice)
{
	struct page_ext_iter iter;
	struct page_ext *page_ext;
	struct page *page;
	bool anon;

	if (!pfn_valid(pfn))
		return;
	BUG_ON(slice >= PPPS_SLICES_PER_PAGE);

	page = pfn_to_page(pfn);
	BUG_ON(PageSlab(page));
	anon = PageAnon(page);

	rcu_read_lock();
	for_each_page_ext(page, pgcnt, page_ext, iter) {
		struct page_table_check *ptc = get_page_table_check(page_ext);

		if (anon) {
			BUG_ON(atomic_read(&ptc->file_map_count[slice]));
			BUG_ON(atomic_dec_return(&ptc->anon_map_count[slice]) < 0);
		} else {
			BUG_ON(atomic_read(&ptc->anon_map_count[slice]));
			BUG_ON(atomic_dec_return(&ptc->file_map_count[slice]) < 0);
		}
	}
	rcu_read_unlock();
}

/*
 * A new entry is added to the page table, increment the counters for that page
 * verify that it is of correct type and is not being mapped with a different
 * type to a different process.
 */
static void page_table_check_set(unsigned long pfn, unsigned long pgcnt,
				 unsigned int slice, bool rw)
{
	struct page_ext_iter iter;
	struct page_ext *page_ext;
	struct page *page;
	bool anon;

	if (!pfn_valid(pfn))
		return;
	BUG_ON(slice >= PPPS_SLICES_PER_PAGE);

	page = pfn_to_page(pfn);
	BUG_ON(PageSlab(page));
	anon = PageAnon(page);

	rcu_read_lock();
	for_each_page_ext(page, pgcnt, page_ext, iter) {
		struct page_table_check *ptc = get_page_table_check(page_ext);

		if (anon) {
			BUG_ON(atomic_read(&ptc->file_map_count[slice]));
			BUG_ON(atomic_inc_return(&ptc->anon_map_count[slice]) > 1 && rw);
		} else {
			BUG_ON(atomic_read(&ptc->anon_map_count[slice]));
			BUG_ON(atomic_inc_return(&ptc->file_map_count[slice]) < 0);
		}
	}
	rcu_read_unlock();
}

/*
 * page is on free list, or is being allocated, verify that counters are zeroes
 * crash if they are not.
 */
void __page_table_check_zero(struct page *page, unsigned int order)
{
	struct page_ext_iter iter;
	struct page_ext *page_ext;

	BUG_ON(PageSlab(page));

	rcu_read_lock();
	for_each_page_ext(page, 1 << order, page_ext, iter) {
		struct page_table_check *ptc = get_page_table_check(page_ext);
		unsigned int slice;

		for (slice = 0; slice < PPPS_SLICES_PER_PAGE; slice++) {
			BUG_ON(atomic_read(&ptc->anon_map_count[slice]));
			BUG_ON(atomic_read(&ptc->file_map_count[slice]));
		}
	}
	rcu_read_unlock();
}

void __page_table_check_pte_clear(struct mm_struct *mm, pte_t pte)
{
	if (&init_mm == mm)
		return;

	if (pte_user_accessible_page(pte)) {
		page_table_check_clear(pte_pfn(pte), 1,
				       page_table_check_pte_slice(pte));
	}
}
EXPORT_SYMBOL(__page_table_check_pte_clear);

void __page_table_check_pmd_clear(struct mm_struct *mm, pmd_t pmd)
{
	if (&init_mm == mm)
		return;

	if (pmd_user_accessible_page(pmd)) {
		page_table_check_clear(pmd_pfn(pmd),
				       MM_PMD_SIZE(mm) >> PAGE_SHIFT, 0);
	}
}
EXPORT_SYMBOL(__page_table_check_pmd_clear);

void __page_table_check_pud_clear(struct mm_struct *mm, pud_t pud)
{
	if (&init_mm == mm)
		return;

	if (pud_user_accessible_page(pud)) {
		page_table_check_clear(pud_pfn(pud),
				       MM_PUD_SIZE(mm) >> PAGE_SHIFT, 0);
	}
}
EXPORT_SYMBOL(__page_table_check_pud_clear);

/* Whether the swap entry cached writable information */
static inline bool swap_cached_writable(swp_entry_t entry)
{
	return is_writable_device_private_entry(entry) ||
	       is_writable_migration_entry(entry);
}

static inline void page_table_check_pte_flags(pte_t pte)
{
	if (pte_present(pte) && pte_uffd_wp(pte))
		WARN_ON_ONCE(pte_write(pte));
	else if (is_swap_pte(pte) && pte_swp_uffd_wp(pte))
		WARN_ON_ONCE(swap_cached_writable(pte_to_swp_entry(pte)));
}

void __page_table_check_ptes_set(struct mm_struct *mm, pte_t *ptep, pte_t pte,
		unsigned int nr)
{
	unsigned int i;

	unsigned long pgsize = MM_PAGE_SIZE(mm);
	unsigned long nr_pages = max(1UL, pgsize >> PAGE_SHIFT);

	if (&init_mm == mm)
		return;

	page_table_check_pte_flags(pte);

	for (i = 0; i < nr; i++) {
		pte_t old_pte = ptep_get(ptep + i);
		pte_t new_pte = pte_advance_phys(pte, pgsize * i);

		__page_table_check_pte_clear(mm, old_pte);

		if (pte_user_accessible_page(new_pte)) {
			page_table_check_set(pte_pfn(new_pte), nr_pages,
					     page_table_check_pte_slice(new_pte),
					     pte_write(new_pte));
		}
	}
}
EXPORT_SYMBOL(__page_table_check_ptes_set);

static inline void page_table_check_pmd_flags(pmd_t pmd)
{
	if (pmd_present(pmd) && pmd_uffd_wp(pmd))
		WARN_ON_ONCE(pmd_write(pmd));
	else if (is_swap_pmd(pmd) && pmd_swp_uffd_wp(pmd))
		WARN_ON_ONCE(swap_cached_writable(pmd_to_swp_entry(pmd)));
}

void __page_table_check_pmds_set(struct mm_struct *mm, pmd_t *pmdp, pmd_t pmd,
		unsigned int nr)
{
	unsigned long stride = MM_PMD_SIZE(mm) >> PAGE_SHIFT;
	unsigned int i;

	if (&init_mm == mm)
		return;

	page_table_check_pmd_flags(pmd);

	for (i = 0; i < nr; i++)
		__page_table_check_pmd_clear(mm, *(pmdp + i));
	if (pmd_user_accessible_page(pmd))
		page_table_check_set(pmd_pfn(pmd), stride * nr, 0,
				     pmd_write(pmd));
}
EXPORT_SYMBOL(__page_table_check_pmds_set);

void __page_table_check_puds_set(struct mm_struct *mm, pud_t *pudp, pud_t pud,
		unsigned int nr)
{
	unsigned long stride = MM_PUD_SIZE(mm) >> PAGE_SHIFT;
	unsigned int i;

	if (&init_mm == mm)
		return;

	for (i = 0; i < nr; i++)
		__page_table_check_pud_clear(mm, *(pudp + i));
	if (pud_user_accessible_page(pud))
		page_table_check_set(pud_pfn(pud), stride * nr, 0,
				     pud_write(pud));
}
EXPORT_SYMBOL(__page_table_check_puds_set);

void __page_table_check_pte_clear_range(struct mm_struct *mm,
					unsigned long addr,
					pmd_t pmd)
{
	if (&init_mm == mm)
		return;

	if (!pmd_bad(pmd) && !pmd_leaf(pmd)) {
		pte_t *ptep = pte_offset_map_mm(mm, &pmd, addr);
		unsigned long i;
		unsigned long ptrs = MM_PTRS_PER_PTE(mm);

		if (WARN_ON(!ptep))
			return;
		for (i = 0; i < ptrs; i++) {
			__page_table_check_pte_clear(mm, ptep_get(ptep));
			addr += MM_PAGE_SIZE(mm);
			ptep++;
		}
		pte_unmap(ptep - ptrs);
	}
}
