// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/ppps.h>

#include <asm/pgtable.h>
#include <asm/tlb.h>

#define TEST_PTE_PROT	(PTE_TYPE_PAGE | PTE_AF | PTE_USER | PTE_RDONLY | \
			 PTE_NG | PTE_DBM | PTE_PXN | PTE_UXN | PTE_DIRTY | \
			 PTE_SPECIAL)

static pte_t pte_advance_test_entry(phys_addr_t phys)
{
	return __pte(__phys_to_pte_val(phys) | TEST_PTE_PROT);
}

static pteval_t pte_advance_test_non_phys(pte_t pte)
{
	phys_addr_t phys = __pte_to_phys(pte);

	return pte_val(pte) ^ __phys_to_pte_val(phys);
}

static void pte_advance_test_sequence(struct kunit *test,
				      unsigned long stride,
				      unsigned int nr_steps)
{
	const phys_addr_t base = SZ_1G;
	pte_t pte = pte_advance_test_entry(base);
	unsigned int i;

	for (i = 0; i < nr_steps; i++) {
		phys_addr_t expected = base + (phys_addr_t)i * stride;

		KUNIT_EXPECT_EQ_MSG(test, __pte_to_phys(pte), expected,
				    "step %u, stride %#lx", i, stride);
		KUNIT_EXPECT_EQ_MSG(test, pte_advance_test_non_phys(pte),
				    (pteval_t)TEST_PTE_PROT,
				    "attributes changed at step %u", i);
		pte = pte_advance_phys(pte, stride);
	}
}

static void pte_advance_phys_native_test(struct kunit *test)
{
	pte_advance_test_sequence(test, PAGE_SIZE, 8);
}

static void tlb_unmap_geometry_test(struct kunit *test)
{
	struct mm_struct *mm = kunit_kzalloc(test, sizeof(*mm), GFP_KERNEL);
	struct mmu_gather *tlb = kunit_kzalloc(test, sizeof(*tlb), GFP_KERNEL);
	unsigned long sizes[] = { PAGE_SIZE, PMD_SIZE, PUD_SIZE, P4D_SIZE };
	unsigned int mode, level;
	unsigned int modes = 1 + IS_ENABLED(CONFIG_ARM64_PER_PROCESS_PAGE_SIZE);

	KUNIT_ASSERT_NOT_NULL(test, mm);
	KUNIT_ASSERT_NOT_NULL(test, tlb);
	for (mode = 0; mode < modes; mode++) {
#ifdef CONFIG_ARM64_PER_PROCESS_PAGE_SIZE
		mm->page_shift = mode ? PAGE_SHIFT_COMPAT : PAGE_SHIFT;
		if (mode) {
			sizes[0] = SZ_4K;
			sizes[1] = SZ_2M;
			sizes[2] = SZ_1G;
			sizes[3] = SZ_1G; /* P4D is folded in the three-level build. */
		}
#endif
		for (level = 0; level < ARRAY_SIZE(sizes); level++) {
			memset(tlb, 0, sizeof(*tlb));
			tlb->mm = mm;
			tlb->freed_tables = true;
			tlb->cleared_ptes = level == 0;
			tlb->cleared_pmds = level == 1;
			tlb->cleared_puds = level == 2;
			tlb->cleared_p4ds = level == 3;
			KUNIT_EXPECT_EQ_MSG(test, tlb_get_unmap_size_mm(tlb),
					sizes[level], "mode %u level %u", mode, level);
			KUNIT_EXPECT_EQ(test, tlb_get_level(tlb), TLBI_TTL_UNKNOWN);
			/* Mixed-level teardown uses the smallest cleared level. */
			tlb->cleared_ptes = true;
			KUNIT_EXPECT_EQ(test, tlb_get_unmap_size_mm(tlb), sizes[0]);
		}
	}
}

#if IS_ENABLED(CONFIG_ARM64_PER_PROCESS_PAGE_SIZE)
static void pte_advance_phys_compat_test(struct kunit *test)
{
	KUNIT_ASSERT_LT(test, PAGE_SIZE_COMPAT, PAGE_SIZE);
	pte_advance_test_sequence(test, PAGE_SIZE_COMPAT, 8);
}
#endif

static struct kunit_case pte_test_cases[] = {
	KUNIT_CASE(pte_advance_phys_native_test),
	KUNIT_CASE(tlb_unmap_geometry_test),
#if IS_ENABLED(CONFIG_ARM64_PER_PROCESS_PAGE_SIZE)
	KUNIT_CASE(pte_advance_phys_compat_test),
#endif
	{}
};

static struct kunit_suite pte_test_suite = {
	.name = "arm64-pte",
	.test_cases = pte_test_cases,
};

kunit_test_suite(pte_test_suite);

MODULE_DESCRIPTION("KUnit tests for arm64 PTE helpers");
MODULE_LICENSE("GPL");
