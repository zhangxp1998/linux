// SPDX-License-Identifier: GPL-2.0-only
/* Private PTE arrays model migration across a split anonymous tuple. */
#include <kunit/test.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/swap.h>
#include <linux/swapops.h>

#include "ppps.h"

struct tuple_test {
	struct mm_struct *mm;
	struct folio *source;
	struct folio *destination;
	struct vm_area_struct vma[2];
	pte_t ptes[PPPS_SLICES_PER_PAGE];
};

static void tuple_test_destroy(void *data)
{
	struct tuple_test *ctx = data;
	struct folio *folios[] = { ctx->source, ctx->destination };
	int i;

	for (i = 0; i < ARRAY_SIZE(folios); i++) {
		if (!folios[i])
			continue;
		folios[i]->mapping = NULL;
		folio_clear_ppps_compat_anon(folios[i]);
		if (folio_test_locked(folios[i]))
			folio_unlock(folios[i]);
		folio_put(folios[i]);
	}
	if (ctx->mm) {
		/* Synthetic RSS and geometry never describe installed page tables. */
		percpu_counter_set(&ctx->mm->rss_stat[MM_ANONPAGES], 0);
		ctx->mm->page_shift = PAGE_SHIFT;
		mmput(ctx->mm);
	}
}

static int tuple_test_init(struct kunit *test)
{
	struct tuple_test *ctx;
	int i, ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	ret = kunit_add_action_or_reset(test, tuple_test_destroy, ctx);
	if (ret)
		return ret;
	ctx->mm = mm_alloc();
	ctx->source = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	ctx->destination = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	if (!ctx->mm || !ctx->source || !ctx->destination)
		return -ENOMEM;
	/* Migration-entry inspection requires the referenced folio locked. */
	folio_lock(ctx->source);
	folio_lock(ctx->destination);
	ctx->mm->page_shift = PAGE_SHIFT_COMPAT;
	/* Only the type predicates inspect mapping; no anon_vma is dereferenced. */
	ctx->source->mapping = (void *)PAGE_MAPPING_ANON;
	ctx->destination->mapping = (void *)PAGE_MAPPING_ANON;
	folio_set_ppps_compat_anon(ctx->source);
	folio_set_ppps_compat_anon(ctx->destination);
	for (i = 0; i < ARRAY_SIZE(ctx->vma); i++) {
		vma_init(&ctx->vma[i], ctx->mm);
		/* vma_init() installs dummy ops; model a real anonymous VMA. */
		ctx->vma[i].vm_ops = NULL;
		ctx->vma[i].vm_start = PAGE_SIZE + i * PAGE_SIZE / 2;
		ctx->vma[i].vm_end = ctx->vma[i].vm_start + PAGE_SIZE / 2;
		ctx->vma[i].vm_pgoff = ctx->vma[i].vm_start >> PAGE_SHIFT_COMPAT;
		KUNIT_ASSERT_TRUE(test, ppps_vma_address_shares_tuple(
				&ctx->vma[i], ctx->vma[i].vm_start));
	}
	return 0;
}

static void check_migration(struct kunit *test, bool same_folio, bool reverse)
{
	struct tuple_test *ctx = test->priv;
	struct folio *destination = same_folio ? ctx->source : ctx->destination;
	unsigned int owners = 0, step;
	pte_t migration = swp_entry_to_pte(
		make_readable_migration_entry(folio_pfn(ctx->source)));

	for (step = 0; step < ARRAY_SIZE(ctx->ptes); step++)
		ctx->ptes[step] = migration;
	percpu_counter_set(&ctx->mm->rss_stat[MM_ANONPAGES], 1);
	for (step = 0; step < ARRAY_SIZE(ctx->ptes); step++) {
		unsigned int slice = reverse ? ARRAY_SIZE(ctx->ptes) - 1 - step : step;
		unsigned long address = PAGE_SIZE + slice * PAGE_SIZE_COMPAT;
		struct vm_area_struct *vma = &ctx->vma[slice / 2];
		bool last = step == ARRAY_SIZE(ctx->ptes) - 1;
		s64 expected = same_folio || last ? 1 : 2;

		owners += ppps_anon_restore_migration(vma, ctx->source,
				destination, &ctx->ptes[slice], address);
		ctx->ptes[slice] = pte_mkslice(
			mk_pte(&destination->page, PAGE_READONLY), slice);
		KUNIT_EXPECT_EQ_MSG(test,
			percpu_counter_sum(&ctx->mm->rss_stat[MM_ANONPAGES]),
			expected, "step %u slice %u", step, slice);
	}
	KUNIT_EXPECT_EQ(test, owners, 1U);
}

static void migration_split_tuple_test(struct kunit *test)
{
	check_migration(test, false, false);
}

static void migration_reverse_tuple_test(struct kunit *test)
{
	check_migration(test, false, true);
}

static void migration_same_folio_test(struct kunit *test)
{
	check_migration(test, true, false);
}

static struct kunit_case ppps_test_cases[] = {
	KUNIT_CASE(migration_split_tuple_test),
	KUNIT_CASE(migration_reverse_tuple_test),
	KUNIT_CASE(migration_same_folio_test),
	{}
};

static struct kunit_suite ppps_test_suite = {
	.name = "ppps-tuples",
	.init = tuple_test_init,
	.test_cases = ppps_test_cases,
};

kunit_test_suite(ppps_test_suite);

MODULE_LICENSE("GPL");
