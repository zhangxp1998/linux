// SPDX-License-Identifier: GPL-2.0-only
/* Private PTE arrays model migration across a split anonymous tuple. */
#include <kunit/test.h>
#include <linux/mm.h>
#include <linux/page_table_check.h>
#include <linux/rmap.h>
#include <linux/sched/mm.h>
#include <linux/shmem_fs.h>
#include <linux/swap.h>
#include <linux/swapops.h>

#include <asm/pgalloc.h>

#include "ppps.h"
#include "vma.h"

struct tuple_test {
	struct mm_struct *mm;
	struct folio *source;
	struct folio *destination;
	struct folio *singleton;
	struct file *file;
	struct vm_area_struct vma[2];
	pte_t ptes[PPPS_SLICES_PER_PAGE];
};

static const struct vm_operations_struct tuple_file_vm_ops;

static void tuple_test_destroy(void *data)
{
	struct tuple_test *ctx = data;
	struct folio *folios[] = {
		ctx->source, ctx->destination, ctx->singleton,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(folios); i++) {
		if (!folios[i])
			continue;
		if (folios[i] != ctx->singleton) {
			folios[i]->mapping = NULL;
			folio_clear_ppps_compat_anon(folios[i]);
		}
		if (folio_test_locked(folios[i]))
			folio_unlock(folios[i]);
		folio_put(folios[i]);
	}
	if (ctx->file)
		fput(ctx->file);
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
	ctx->singleton = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	if (!ctx->mm || !ctx->source || !ctx->destination || !ctx->singleton)
		return -ENOMEM;
	ctx->file = shmem_file_setup("ppps-kunit", PAGE_SIZE, 0);
	if (IS_ERR(ctx->file)) {
		ret = PTR_ERR(ctx->file);
		ctx->file = NULL;
		return ret;
	}
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

static void tuple_test_full_vma(struct tuple_test *ctx,
				struct vm_area_struct *vma)
{
	vma_init(vma, ctx->mm);
	vma->vm_ops = NULL;
	vma->vm_start = PAGE_SIZE;
	vma->vm_end = 2 * PAGE_SIZE;
	vma->vm_pgoff = vma->vm_start >> PAGE_SHIFT_COMPAT;
}

static void tuple_test_file_vma(struct tuple_test *ctx,
				struct vm_area_struct *vma)
{
	tuple_test_full_vma(ctx, vma);
	vma->vm_file = ctx->file;
	vma->vm_ops = &tuple_file_vm_ops;
	vm_flags_init(vma, VM_READ | VM_WRITE | VM_MAYREAD | VM_MAYWRITE);
}

static void tuple_test_set_present(struct tuple_test *ctx,
				   struct folio *folio)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ctx->ptes); i++)
		ctx->ptes[i] = pte_mkslice(
			mk_pte(&folio->page, PAGE_READONLY), i);
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

static void tuple_geometry_test(struct kunit *test)
{
	struct tuple_test *ctx = test->priv;
	struct vm_area_struct vma;
	struct page_vma_mapped_walk pvmw = {
		.nr_pages = 1,
	};
	unsigned long address = PAGE_SIZE + PAGE_SIZE_COMPAT;

	tuple_test_full_vma(ctx, &vma);
	KUNIT_EXPECT_TRUE(test, ppps_vma_shares_tuple(&vma));
	KUNIT_EXPECT_TRUE(test, ppps_vma_address_shares_tuple(&vma, address));
	KUNIT_EXPECT_EQ(test, ppps_tuple_index(&vma, address),
			vma.vm_pgoff);

	pvmw.vma = &vma;
	pvmw.pfn = folio_pfn(ctx->source);
	pvmw.address = address;
	KUNIT_EXPECT_EQ(test, ppps_pvmw_walk_end(&pvmw), 2 * PAGE_SIZE);
	pvmw.pfn = folio_pfn(ctx->singleton);
	KUNIT_EXPECT_EQ(test, ppps_pvmw_walk_end(&pvmw),
			address + PAGE_SIZE_COMPAT);
	pvmw.nr_pages = 2;
	KUNIT_EXPECT_EQ(test, ppps_pvmw_walk_end(&pvmw), 0UL);

	ctx->mm->page_shift = PAGE_SHIFT;
	KUNIT_EXPECT_FALSE(test, ppps_vma_shares_tuple(&vma));
	pvmw.nr_pages = 1;
	KUNIT_EXPECT_EQ(test, ppps_pvmw_walk_end(&pvmw), 0UL);
	ctx->mm->page_shift = PAGE_SHIFT_COMPAT;

	vm_flags_init(&vma, vma.vm_flags | VM_DROPPABLE);
	KUNIT_EXPECT_FALSE(test, ppps_vma_shares_tuple(&vma));
	KUNIT_EXPECT_FALSE(test,
			ppps_vma_address_shares_tuple(&vma, address));
	KUNIT_EXPECT_EQ(test, ppps_anon_reslice_range(ctx->mm, 0, 0, 0), 0);
	mmap_write_lock(ctx->mm);
	KUNIT_EXPECT_EQ(test, ppps_anon_reslice_range(ctx->mm, 0, PAGE_SIZE,
			PAGE_SIZE_COMPAT), -EFAULT);
	mmap_write_unlock(ctx->mm);
}

static void tuple_state_test(struct kunit *test)
{
	struct tuple_test *ctx = test->priv;
	struct vm_area_struct vma;
	unsigned long address = PAGE_SIZE + PAGE_SIZE_COMPAT;
	bool multi_folio = true;

	tuple_test_full_vma(ctx, &vma);
	tuple_test_set_present(ctx, ctx->source);
	KUNIT_EXPECT_TRUE(test, ppps_anon_tuple_is_complete(&vma, ctx->source,
			address, &ctx->ptes[1]));
	KUNIT_EXPECT_TRUE(test, ppps_anon_tuple_within(&vma, ctx->source,
			&ctx->ptes[1], address, PAGE_SIZE, 2 * PAGE_SIZE));
	KUNIT_EXPECT_FALSE(test, ppps_anon_tuple_within(&vma, ctx->source,
			&ctx->ptes[1], address, address,
			address + PAGE_SIZE_COMPAT));

	ctx->ptes[3] = __pte(0);
	KUNIT_EXPECT_FALSE(test, ppps_anon_tuple_is_complete(&vma, ctx->source,
			address, &ctx->ptes[1]));
	KUNIT_EXPECT_FALSE(test, ppps_anon_tuple_is_complete(&ctx->vma[0],
			ctx->source, PAGE_SIZE, &ctx->ptes[0]));
	KUNIT_EXPECT_FALSE(test, ppps_anon_tuple_is_complete(&vma,
			ctx->singleton, address, &ctx->ptes[1]));

	vm_flags_init(&vma, vma.vm_flags | VM_DROPPABLE);
	KUNIT_EXPECT_TRUE(test, ppps_anon_tuple_within(&vma, ctx->source,
			&ctx->ptes[1], address, address,
			address + PAGE_SIZE_COMPAT));
	KUNIT_EXPECT_TRUE(test, ppps_anon_slice_takes_ownership(&vma,
			ctx->source, &ctx->ptes[1], address));
	KUNIT_EXPECT_FALSE(test, ppps_anon_folio_has_other_entries(&vma,
			ctx->source, &ctx->ptes[1], address));
	KUNIT_EXPECT_PTR_EQ(test, ppps_anon_hole_fill_folio(&vma,
			&ctx->ptes[1], address, &multi_folio), NULL);
	KUNIT_EXPECT_FALSE(test, multi_folio);

	tuple_test_full_vma(ctx, &vma);
	tuple_test_set_present(ctx, ctx->source);
	ctx->ptes[1] = __pte(0);
	ctx->source->index = ppps_tuple_index(&vma, address);
	KUNIT_EXPECT_PTR_EQ(test, ppps_anon_hole_fill_folio(&vma,
			&ctx->ptes[1], address, NULL), NULL);
}

static void unmap_state_test(struct kunit *test)
{
	struct tuple_test *ctx = test->priv;
	struct ppps_anon_unmap_ctx unmap;
	struct vm_area_struct vma;
	struct page_vma_mapped_walk pvmw;
	unsigned long address = PAGE_SIZE + PAGE_SIZE_COMPAT;
	unsigned long start = address;
	unsigned long end = address + PAGE_SIZE_COMPAT;
	pte_t readable = swp_entry_to_pte(
		make_readable_migration_entry(folio_pfn(ctx->source)));
	pte_t foreign = swp_entry_to_pte(
		make_readable_migration_entry(folio_pfn(ctx->destination)));
	pte_t writable = swp_entry_to_pte(
		make_writable_migration_entry(folio_pfn(ctx->source)));

	tuple_test_full_vma(ctx, &vma);
	ppps_anon_unmap_begin(&unmap, &vma, ctx->source, &start, &end);
	KUNIT_EXPECT_TRUE(test, unmap.compat);
	KUNIT_EXPECT_EQ(test, start, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, end, 2 * PAGE_SIZE);
	KUNIT_EXPECT_FALSE(test, ppps_anon_unmap_can_batch(&unmap));
	KUNIT_EXPECT_TRUE(test, ppps_anon_unmap_needs_share(&unmap));
	ppps_anon_unmap_commit_share(&unmap);
	KUNIT_EXPECT_FALSE(test, ppps_anon_unmap_needs_share(&unmap));
	unmap.exclusive = true;
	pvmw = (struct page_vma_mapped_walk) {
		.vma = &vma,
		.pte = &ctx->ptes[1],
		.address = address,
	};
	KUNIT_EXPECT_TRUE(test, ppps_anon_unmap_sample(&unmap, ctx->source,
			&ctx->source->page, &pvmw));

	unmap.shared = false;
	ctx->ptes[0] = foreign;
	ctx->ptes[1] = readable;
	KUNIT_EXPECT_FALSE(test, ppps_anon_unmap_sample(&unmap, ctx->source,
			&ctx->source->page, &pvmw));
	KUNIT_EXPECT_TRUE(test, unmap.shared);

	unmap.shared = false;
	ctx->ptes[0] = readable;
	ctx->ptes[1] = __pte(0);
	KUNIT_EXPECT_FALSE(test, ppps_anon_unmap_sample(&unmap, ctx->source,
			&ctx->source->page, &pvmw));
	KUNIT_EXPECT_TRUE(test, unmap.shared);
	unmap.shared = false;
	ctx->ptes[0] = writable;
	KUNIT_EXPECT_TRUE(test, ppps_anon_unmap_sample(&unmap, ctx->source,
			&ctx->source->page, &pvmw));

	tuple_test_set_present(ctx, ctx->source);
	ctx->ptes[1] = __pte(0);
	ppps_anon_unmap_clear(&unmap, &vma, ctx->source, &ctx->ptes[1], address);
	KUNIT_EXPECT_FALSE(test, ppps_anon_unmap_last(&unmap));
	memset(ctx->ptes, 0, sizeof(ctx->ptes));
	ppps_anon_unmap_clear(&unmap, &vma, ctx->source, &ctx->ptes[1], address);
	KUNIT_EXPECT_TRUE(test, ppps_anon_unmap_last(&unmap));
}

static void swapin_state_test(struct kunit *test)
{
	struct tuple_test *ctx = test->priv;
	struct ppps_anon_swapin_ctx swapin;
	struct ppps_anon_swapin_ctx native = {};
	struct vm_area_struct vma;
	unsigned long address = PAGE_SIZE + PAGE_SIZE_COMPAT;
	swp_entry_t entry = swp_entry(0, 1);
	pte_t swap_pte = swp_entry_to_pte(entry);
	pte_t *ptep;
	bool exclusive = true;

	tuple_test_full_vma(ctx, &vma);
	memset(ctx->ptes, 0, sizeof(ctx->ptes));
	ctx->ptes[0] = swap_pte;
	ctx->ptes[1] = swap_pte;
	ctx->ptes[2] = swap_pte;
	ppps_anon_swapin_begin(&swapin, &vma, ctx->source, &ctx->ptes[1],
			address, entry);
	KUNIT_EXPECT_TRUE(test, swapin.compat);
	KUNIT_EXPECT_TRUE(test, ppps_anon_swapin_takes_ownership(&swapin));
	KUNIT_EXPECT_EQ(test, ppps_anon_swapin_swap_delta(&swapin), 2U);
	KUNIT_EXPECT_EQ(test, swapin.swap_extra, 2U);

	ctx->ptes[3] = swap_pte;
	ptep = &ctx->ptes[1];
	ppps_anon_swapin_begin(&swapin, &ctx->vma[0], ctx->source, ptep, address, entry);
	KUNIT_EXPECT_EQ(test, ppps_anon_swapin_swap_delta(&swapin), 1U);
	KUNIT_EXPECT_EQ(test, swapin.swap_extra, 2U);

	ctx->ptes[0] = pte_mkslice(
		mk_pte(&ctx->source->page, PAGE_READONLY), 0);
	ctx->ptes[3] = __pte(0);
	ppps_anon_swapin_begin(&swapin, &vma, ctx->source, &ctx->ptes[1],
			address, entry);
	KUNIT_EXPECT_FALSE(test, ppps_anon_swapin_takes_ownership(&swapin));
	KUNIT_EXPECT_EQ(test, ppps_anon_swapin_swap_delta(&swapin), 1U);

	ppps_anon_swapin_release(&native, entry, &exclusive);
	KUNIT_EXPECT_TRUE(test, exclusive);
	ctx->mm->page_shift = PAGE_SHIFT;
	ppps_swap_pte_remove(&vma, &ctx->ptes[1], address, 1, entry);
	ctx->mm->page_shift = PAGE_SHIFT_COMPAT;
	vm_flags_init(&vma, vma.vm_flags | VM_DROPPABLE);
	ppps_swap_pte_remove(&vma, &ctx->ptes[1], address, 1, entry);
}

static void vm_insert_pages_input_test(struct kunit *test)
{
	struct tuple_test *ctx = test->priv;
	struct vm_area_struct vma;
	unsigned long nr;

	tuple_test_full_vma(ctx, &vma);
	nr = 0;
	KUNIT_EXPECT_EQ(test, ppps_vm_insert_pages(&vma, vma.vm_start, NULL, &nr),
			0);
	nr = 1;
	KUNIT_EXPECT_EQ(test, ppps_vm_insert_pages(&vma, vma.vm_start - 1,
			NULL, &nr), -EFAULT);
	KUNIT_EXPECT_EQ(test, ppps_vm_insert_pages(&vma, vma.vm_start + 1,
			NULL, &nr), -EFAULT);
	nr = 2;
	KUNIT_EXPECT_EQ(test, ppps_vm_insert_pages(&vma, vma.vm_start, NULL, &nr),
			-EFAULT);
}

static void vma_merge_slice_offset_test(struct kunit *test)
{
	static const struct vm_operations_struct sliced_ops;
	struct tuple_test *ctx = test->priv;
	struct vm_area_struct sliced, next;
	struct vma_merge_struct vmg = {
		.mm = ctx->mm,
		.start = PAGE_SIZE,
		.end = PAGE_SIZE + PAGE_SIZE_COMPAT,
		.pgoff = 0,
		.slice_off = 0,
		.next = &next,
	};

	vma_init(&sliced, ctx->mm);
	sliced.vm_ops = &sliced_ops;
	vmg.ppps_sliced = ppps_vma_has_slices(&sliced);
	vma_init(&next, ctx->mm);
	next.vm_pgoff = 1;
	vma_set_slice_off(&next, 0);
	KUNIT_EXPECT_FALSE(test, vmg_can_merge_offsets(&vmg, true));

	next.vm_pgoff = 0;
	vma_set_slice_off(&next, 1);
	KUNIT_EXPECT_TRUE(test, vmg_can_merge_offsets(&vmg, true));

	vmg.ppps_sliced = false;
	vmg.flags = VM_SHARED;
	KUNIT_EXPECT_TRUE(test, vmg_can_merge_offsets(&vmg, true));
}

static void fault_input_test(struct kunit *test)
{
	struct tuple_test *ctx = test->priv;
	struct vm_area_struct anon_vma, file_vma;
	unsigned long address = PAGE_SIZE + 2 * PAGE_SIZE_COMPAT;
	struct vm_fault anon_vmf = {
		.vma = &anon_vma,
		.address = address,
	};
	struct vm_fault file_vmf = {
		.vma = &file_vma,
		.address = address,
		.flags = FAULT_FLAG_WRITE,
	};
	struct folio *folio = ctx->singleton;
	unsigned long addr = address;
	vm_fault_t ret = 0;
	spinlock_t ptl; /* Protects the synthetic PTE array. */
	enum ppps_anon_wp_type wp_type;
	swp_entry_t entry;
	bool prepared;
	int nr_pages = 1;
	pte_t zero;

	tuple_test_full_vma(ctx, &anon_vma);
	ctx->mm->page_shift = PAGE_SHIFT;
	prepared = ppps_anon_fault_anon_prepare(&anon_vmf, &folio, &nr_pages, &addr);
	KUNIT_EXPECT_FALSE(test, prepared);
	KUNIT_EXPECT_EQ(test, nr_pages, 1);
	KUNIT_EXPECT_EQ(test, addr, address);
	ctx->mm->page_shift = PAGE_SHIFT_COMPAT;
	mmap_write_lock(ctx->mm);

	memset(ctx->ptes, 0, sizeof(ctx->ptes));
	spin_lock_init(&ptl);
	anon_vmf.pte = &ctx->ptes[2];
	anon_vmf.ptl = &ptl;
	spin_lock(&ptl);
	prepared = ppps_anon_fault_anon_prepare(&anon_vmf, &folio, &nr_pages, &addr);
	KUNIT_EXPECT_FALSE(test, prepared);
	spin_unlock(&ptl);
	KUNIT_EXPECT_EQ(test, nr_pages, PPPS_SLICES_PER_PAGE);
	KUNIT_EXPECT_EQ(test, addr, PAGE_SIZE);
	KUNIT_EXPECT_PTR_EQ(test, anon_vmf.pte, &ctx->ptes[0]);

	tuple_test_file_vma(ctx, &file_vma);
	file_vmf.pte = &ctx->ptes[2];
	file_vmf.ptl = &ptl;
	KUNIT_EXPECT_TRUE(test, ppps_anon_file_cow_no_prealloc(&file_vmf));
	spin_lock(&ptl);
	prepared = ppps_anon_fault_file_cow(&file_vmf, &folio, &ret);
	KUNIT_EXPECT_TRUE(test, prepared);
	spin_unlock(&ptl);
	KUNIT_EXPECT_EQ(test, ret, (vm_fault_t)VM_FAULT_NOPAGE);

	file_vmf.cow_page = &ctx->singleton->page;
	KUNIT_EXPECT_FALSE(test, ppps_anon_file_cow_no_prealloc(&file_vmf));
	file_vmf.cow_page = NULL;
	vm_flags_set(&file_vma, VM_SHARED);
	KUNIT_EXPECT_FALSE(test, ppps_anon_file_cow_no_prealloc(&file_vmf));
	vm_flags_clear(&file_vma, VM_SHARED);

	wp_type = ppps_anon_wp_type(&file_vma, ctx->singleton, address, __pte(0));
	KUNIT_EXPECT_EQ(test, wp_type, PPPS_ANON_WP_FILE_COW);
	zero = pfn_pte(my_zero_pfn(address), PAGE_READONLY);
	wp_type = ppps_anon_wp_type(&anon_vma, NULL, address, zero);
	KUNIT_EXPECT_EQ(test, wp_type, PPPS_ANON_WP_ZERO);
	mmap_write_unlock(ctx->mm);

	entry = make_readable_migration_entry(folio_pfn(ctx->source));
	ppps_swap_entry_reset(entry);
}

#if IS_ENABLED(CONFIG_USERFAULTFD)
static void uffd_copy_input_test(struct kunit *test)
{
	struct tuple_test *ctx = test->priv;
	struct vm_area_struct vma;
	unsigned long address = PAGE_SIZE;
	uffd_flags_t copy = uffd_flags_set_mode(0, MFILL_ATOMIC_COPY);
	struct ppps_uffd_copy_state state = {};
	struct folio *folio = NULL;
	struct folio *pending;
	unsigned long size;
	pmd_t pmd = __pmd(0);
	long copied;

	tuple_test_full_vma(ctx, &vma);
	pending = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, pending);
	mmap_write_lock(ctx->mm);
	ctx->mm->page_shift = PAGE_SHIFT;
	copied = ppps_uffd_copy(&pmd, &vma, address, 0, PAGE_SIZE, copy,
				&folio, &state);
	KUNIT_EXPECT_EQ(test, copied, 0L);
	ctx->mm->page_shift = PAGE_SHIFT_COMPAT;

	copied = ppps_uffd_copy(&pmd, &vma, address, 0, PAGE_SIZE,
				copy | MFILL_ATOMIC_WP, &folio, &state);
	KUNIT_EXPECT_EQ(test, copied, 0L);
	vm_flags_set(&vma, VM_LOCKED);
	copied = ppps_uffd_copy(&pmd, &vma, address, 0, PAGE_SIZE, copy,
				&folio, &state);
	KUNIT_EXPECT_EQ(test, copied, 0L);
	vm_flags_clear(&vma, VM_LOCKED);
	copied = ppps_uffd_copy(&pmd, &vma, vma.vm_start - PAGE_SIZE, 0,
				PAGE_SIZE, copy, &folio, &state);
	KUNIT_EXPECT_EQ(test, copied, 0L);

	folio = pending;
	state.offset = 0;
	state.tuple = false;
	copied = ppps_uffd_copy(&pmd, &vma, address + PAGE_SIZE_COMPAT, 0,
				PAGE_SIZE, copy, &folio, &state);
	KUNIT_EXPECT_EQ(test, copied, 0L);
	KUNIT_EXPECT_PTR_EQ(test, folio, NULL);

	state.tuple = true;
	size = PAGE_SIZE_COMPAT;
	KUNIT_EXPECT_EQ(test, ppps_uffd_copy_retry(&state, &vma, address, &size),
			0UL);
	KUNIT_EXPECT_EQ(test, size, PAGE_SIZE);
	mmap_write_unlock(ctx->mm);
}
#endif

#if IS_ENABLED(CONFIG_PAGE_TABLE_CHECK)
static void page_table_check_nonleaf_input_test(struct kunit *test)
{
	struct tuple_test *ctx = test->priv;
	struct folio *pmd_folio;
	pmd_t pmds[2] = {};
	pud_t puds[2] = {};
	pgtable_t pgtable;
	pmd_t table_pmd = __pmd(0);
	pmd_t huge_pmd;
	unsigned int order;

	if (static_branch_likely(&page_table_check_disabled)) {
		kunit_skip(test, "page_table_check is disabled");
		return;
	}

	__page_table_check_pmd_set(ctx->mm, &pmds[0], __pmd(0));
	__page_table_check_pmd_set(ctx->mm, &pmds[1], __pmd(0));
	__page_table_check_pud_set(ctx->mm, &puds[0], __pud(0));
	__page_table_check_pud_set(ctx->mm, &puds[1], __pud(0));

	order = get_order(MM_PMD_SIZE(ctx->mm));
	pmd_folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, order);
	KUNIT_ASSERT_NOT_NULL(test, pmd_folio);
	huge_pmd = pmd_mkhuge(pfn_pmd(folio_pfn(pmd_folio), PAGE_READONLY));
	KUNIT_ASSERT_EQ(test,
		(__pmd_to_phys(huge_pmd) & MM_PMD_MASK(ctx->mm)) >> PAGE_SHIFT,
		folio_pfn(pmd_folio));
	__page_table_check_pmd_set(ctx->mm, &pmds[0], huge_pmd);
	__page_table_check_pmd_clear(ctx->mm, huge_pmd);
	folio_put(pmd_folio);

	pgtable = pte_alloc_one(ctx->mm);
	KUNIT_ASSERT_NOT_NULL(test, pgtable);
	pmd_populate(ctx->mm, &table_pmd, pgtable);
	__page_table_check_pte_clear_range(ctx->mm, PAGE_SIZE, table_pmd);
	pmd_clear(&table_pmd);
	pte_free(ctx->mm, pgtable);
}
#endif

static struct kunit_case ppps_test_cases[] = {
	KUNIT_CASE(migration_split_tuple_test),
	KUNIT_CASE(migration_reverse_tuple_test),
	KUNIT_CASE(migration_same_folio_test),
	KUNIT_CASE(tuple_geometry_test),
	KUNIT_CASE(tuple_state_test),
	KUNIT_CASE(unmap_state_test),
	KUNIT_CASE(swapin_state_test),
	KUNIT_CASE(vm_insert_pages_input_test),
	KUNIT_CASE(vma_merge_slice_offset_test),
	KUNIT_CASE(fault_input_test),
#if IS_ENABLED(CONFIG_USERFAULTFD)
	KUNIT_CASE(uffd_copy_input_test),
#endif
#if IS_ENABLED(CONFIG_PAGE_TABLE_CHECK)
	KUNIT_CASE(page_table_check_nonleaf_input_test),
#endif
	{}
};

static struct kunit_suite ppps_test_suite = {
	.name = "ppps-tuples",
	.init = tuple_test_init,
	.test_cases = ppps_test_cases,
};

kunit_test_suite(ppps_test_suite);

MODULE_LICENSE("GPL");
