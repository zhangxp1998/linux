// SPDX-License-Identifier: GPL-2.0
/* MOVE must preserve physical pages; unsupported slices fall back to COPY. */
#define _GNU_SOURCE
#include "p3s_slice_test.h"
#include "p3s_harness.h"

struct move_case {
	const char *name;
	unsigned int present;
	unsigned int first;
	unsigned int dst_first;
	unsigned int count;
	bool reject;
};

FIXTURE(uffd_policy) {
};

FIXTURE_SETUP(uffd_policy)
{
	struct uffdio_api api = { .api = UFFD_API };
	unsigned char *page;
	uint64_t entry;
	int fd;

	fd = syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
	if (fd < 0)
		SKIP(return, "userfaultfd: %s", strerror(errno));
	if (ioctl(fd, UFFDIO_API, &api)) {
		close(fd);
		ASSERT_TRUE(false) TH_LOG("UFFDIO_API: %s", strerror(errno));
	}
	close(fd);
	if (!(api.features & UFFD_FEATURE_MOVE))
		SKIP(return, "UFFD_FEATURE_MOVE unavailable");
	page = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, page);
	page[0] = 1;
	if (!p3s_pagemap_entry(page, &entry) || !PAGEMAP_PFN(entry)) {
		munmap(page, NATIVE_PAGE_SIZE);
		SKIP(return, "visible pagemap PFNs required");
	}
	munmap(page, NATIVE_PAGE_SIZE);
}

FIXTURE_TEARDOWN(uffd_policy)
{
}

static int run_case(const struct move_case *test)
{
	unsigned char *src_res = MAP_FAILED, *dst_res = MAP_FAILED;
	unsigned char *src, *dst;
	uint64_t before[P3S_SLICES] = {}, after;
	struct uffdio_api api = { .api = UFFD_API };
	struct uffdio_register reg = { .mode = UFFDIO_REGISTER_MODE_MISSING };
	struct uffdio_move move = {};
	struct uffdio_copy copy = {};
	int status = KSFT_FAIL;
	unsigned int i;
	int fd = -1, ret = -1, error = 0;

	src = map_exact_edge(NATIVE_PAGE_SIZE, &src_res);
	dst = map_exact_edge(NATIVE_PAGE_SIZE, &dst_res);
	if (src == MAP_FAILED || dst == MAP_FAILED)
		goto out;
	for (i = 0; i < P3S_SLICES; i++) {
		if (!(test->present & (1U << i)))
			continue;
		memset(src + i * PROCESS_PAGE_SIZE, 0x31 + i, PROCESS_PAGE_SIZE);
		if (!p3s_pagemap_entry(src + i * PROCESS_PAGE_SIZE, &before[i]) ||
		    !PAGEMAP_PFN(before[i])) {
			status = KSFT_SKIP;
			goto out;
		}
	}
	fd = syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
	if (fd < 0)
		goto out;
	if (ioctl(fd, UFFDIO_API, &api))
		goto out;
	if (!(api.features & UFFD_FEATURE_MOVE)) {
		status = KSFT_SKIP;
		goto out;
	}
	reg.range.start = (uintptr_t)dst;
	reg.range.len = NATIVE_PAGE_SIZE;
	if (ioctl(fd, UFFDIO_REGISTER, &reg))
		goto out;
	move.src = (uintptr_t)(src + test->first * PROCESS_PAGE_SIZE);
	move.dst = (uintptr_t)(dst + test->dst_first * PROCESS_PAGE_SIZE);
	move.len = test->count * PROCESS_PAGE_SIZE;
	errno = 0;
	ret = ioctl(fd, UFFDIO_MOVE, &move);
	error = errno;
	if (test->reject) {
		if (ret != -1 || error != EBUSY || move.move != -EBUSY)
			goto out;
		for (i = 0; i < P3S_SLICES; i++) {
			if (p3s_page_present(dst + i * PROCESS_PAGE_SIZE))
				goto out;
			if (!(test->present & (1U << i)))
				continue;
			if (!p3s_pagemap_entry(src + i * PROCESS_PAGE_SIZE, &after) ||
			    !(after & PM_PRESENT) ||
			    PAGEMAP_PFN(after) != PAGEMAP_PFN(before[i]) ||
			    !all_bytes_are(src + i * PROCESS_PAGE_SIZE,
					   PROCESS_PAGE_SIZE, 0x31 + i))
				goto out;
		}
		/* ART handles EBUSY by explicitly requesting a COPY. */
		copy.src = move.src;
		copy.dst = move.dst;
		copy.len = PROCESS_PAGE_SIZE;
		if (!ioctl(fd, UFFDIO_COPY, &copy) &&
		    copy.copy == PROCESS_PAGE_SIZE &&
		    all_bytes_are((void *)(uintptr_t)copy.dst,
				  PROCESS_PAGE_SIZE, 0x31 + test->first))
			status = KSFT_PASS;
	} else {
		if (ret || move.move != (long long)move.len)
			goto out;
		for (i = 0; i < test->count; i++) {
			unsigned int s = test->first + i;
			unsigned int d = test->dst_first + i;

			if (p3s_page_present(src + s * PROCESS_PAGE_SIZE) ||
			    !p3s_pagemap_entry(dst + d * PROCESS_PAGE_SIZE, &after) ||
			    !(after & PM_PRESENT) ||
			    PAGEMAP_PFN(after) != PAGEMAP_PFN(before[s]) ||
			    !all_bytes_are(dst + d * PROCESS_PAGE_SIZE,
					   PROCESS_PAGE_SIZE, 0x31 + s))
				goto out;
		}
		status = KSFT_PASS;
	}
out:
	if (status == KSFT_FAIL)
		ksft_print_msg("%s: ret=%d errno=%d moved=%lld\n", test->name,
			       ret, error, (long long)move.move);
	if (fd >= 0)
		close(fd);
	if (src_res != MAP_FAILED)
		munmap(src_res, 3 * NATIVE_PAGE_SIZE);
	if (dst_res != MAP_FAILED)
		munmap(dst_res, 3 * NATIVE_PAGE_SIZE);
	return status;
}

static int invalid_area_returns_einval(bool registered, bool cross_phase)
{
	unsigned char *src_res = MAP_FAILED, *dst_res = MAP_FAILED;
	unsigned char *src, *dst;
	struct uffdio_api api = { .api = UFFD_API };
	struct uffdio_register reg = { .mode = UFFDIO_REGISTER_MODE_MISSING };
	struct uffdio_move move = {};
	int status = KSFT_FAIL;
	int fd = -1, ret = -1, error = 0;

	src = map_exact_edge(NATIVE_PAGE_SIZE, &src_res);
	dst = map_exact_edge(NATIVE_PAGE_SIZE, &dst_res);
	if (src == MAP_FAILED || dst == MAP_FAILED)
		goto out;
	memset(src, 0x5a, NATIVE_PAGE_SIZE);
	fd = syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
	if (fd < 0)
		goto out;
	if (ioctl(fd, UFFDIO_API, &api))
		goto out;
	if (!(api.features & UFFD_FEATURE_MOVE)) {
		status = KSFT_SKIP;
		goto out;
	}
	if (registered) {
		/* A registered destination with protections unlike the source. */
		if (mprotect(dst, NATIVE_PAGE_SIZE, PROT_READ))
			goto out;
		reg.range.start = (uintptr_t)dst;
		reg.range.len = NATIVE_PAGE_SIZE;
		if (ioctl(fd, UFFDIO_REGISTER, &reg))
			goto out;
	}
	move.src = (uintptr_t)src;
	move.dst = (uintptr_t)(dst + (cross_phase ? PROCESS_PAGE_SIZE : 0));
	move.len = PROCESS_PAGE_SIZE;
	errno = 0;
	ret = ioctl(fd, UFFDIO_MOVE, &move);
	error = errno;
	if (ret == -1 && error == EINVAL && move.move == -EINVAL &&
	    !p3s_page_present((void *)(uintptr_t)move.dst) &&
	    all_bytes_are(src, NATIVE_PAGE_SIZE, 0x5a))
		status = KSFT_PASS;
out:
	if (status == KSFT_FAIL)
		ksft_print_msg("registered=%d cross_phase=%d: ret=%d errno=%d moved=%lld\n",
			       registered, cross_phase, ret, error,
			       (long long)move.move);
	if (fd >= 0)
		close(fd);
	if (src_res != MAP_FAILED)
		munmap(src_res, 3 * NATIVE_PAGE_SIZE);
	if (dst_res != MAP_FAILED)
		munmap(dst_res, 3 * NATIVE_PAGE_SIZE);
	return status;
}

#define CHECK_POLICY(expr) do { \
	int _rc = (expr); \
	if (_rc == KSFT_SKIP) \
		SKIP(return, "UFFDIO_MOVE precondition unavailable"); \
	EXPECT_EQ(KSFT_PASS, _rc); \
} while (0)

TEST_F(uffd_policy, whole_folio_preserves_physical_pages)
{
	const struct move_case test = { "whole folio", 15, 0, 0, 4, false };

	CHECK_POLICY(run_case(&test));
}

TEST_F(uffd_policy, sole_slice_preserves_physical_page)
{
	const struct move_case test = { "sole slice", 2, 1, 1, 1, false };

	CHECK_POLICY(run_case(&test));
}

TEST_F(uffd_policy, three_siblings_move_without_copy)
{
	const struct move_case test = { "three siblings", 14, 1, 1, 3, false };

	CHECK_POLICY(run_case(&test));
}

TEST_F(uffd_policy, partial_folio_falls_back_to_copy)
{
	const struct move_case test = { "partial folio", 15, 0, 0, 1, true };

	CHECK_POLICY(run_case(&test));
}

TEST_F(uffd_policy, cross_phase_move_falls_back_to_copy)
{
	const struct move_case test = { "cross-phase MOVE", 15, 0, 1, 1, true };

	CHECK_POLICY(run_case(&test));
}

TEST_F(uffd_policy, holey_folio_falls_back_to_copy)
{
	const struct move_case test = { "holey folio", 13, 0, 0, 4, true };

	CHECK_POLICY(run_case(&test));
}

TEST_F(uffd_policy, unregistered_same_phase_destination_rejected)
{
	CHECK_POLICY(invalid_area_returns_einval(false, false));
}

TEST_F(uffd_policy, same_phase_protection_mismatch_rejected)
{
	CHECK_POLICY(invalid_area_returns_einval(true, false));
}

TEST_F(uffd_policy, unregistered_cross_phase_destination_rejected)
{
	CHECK_POLICY(invalid_area_returns_einval(false, true));
}

TEST_F(uffd_policy, cross_phase_protection_mismatch_rejected)
{
	CHECK_POLICY(invalid_area_returns_einval(true, true));
}

P3S_HARNESS_4KB_MAIN
