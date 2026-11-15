// SPDX-License-Identifier: GPL-2.0
/*
 * COW of one 4K slice can replace every mapped PTE of its 16K folio.
 * Start/end MMU notifier invalidations must cover the whole replaced range.
 * The fixture module is loaded by cow_notifier_p3s.sh before this test.
 */
#define _GNU_SOURCE

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_test.h"
#include "p3s_harness.h"
#include "mmu_notifier_p3s.h"

TEST(p3s_cow_notifier)
{
	struct mmu_notifier_p3s_range range;
	struct mmu_notifier_p3s_stats stats = {};
	unsigned char *raw, *base;
	uint64_t before, after;
	int fd, ready[2], status;
	pid_t child;
	char done = 1;
	bool cow_ok, range_ok;
	unsigned int i;

	fd = p3s_open_fixture("/dev/mmu_notifier_p3s", O_RDWR);
	if (fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "MMU notifier fixture unavailable");
	ASSERT_GE(fd, 0);

	base = map_aligned(NATIVE_PAGE_SIZE, &raw);
	ASSERT_NE(MAP_FAILED, base);
	for (i = 0; i < P3S_SLICES; i++)
		base[i * PROCESS_PAGE_SIZE] = 0x31 + i;
	if (!p3s_pfn(base, &before)) {
		close(fd);
		munmap(raw, 2 * NATIVE_PAGE_SIZE);
		SKIP(return, "pagemap PFNs are unavailable");
	}
	ASSERT_EQ(0, pipe(ready));
	child = fork();
	ASSERT_GE(child, 0);
	if (!child) {
		close(fd);
		close(ready[1]);
		if (!read_full(ready[0], &done, 1))
			_exit(1);
		_exit(base[PROCESS_PAGE_SIZE] == 0x32 ? 0 : 1);
	}
	close(ready[0]);
	range.start = (uintptr_t)base;
	range.end = range.start + NATIVE_PAGE_SIZE;
	range.page_size = PROCESS_PAGE_SIZE;
	ASSERT_EQ(0, ioctl(fd, MMU_NOTIFIER_P3S_SET_RANGE, &range));
	base[PROCESS_PAGE_SIZE] = 0x77;
	ASSERT_TRUE(p3s_pfn(base, &after));
	ASSERT_EQ(0, ioctl(fd, MMU_NOTIFIER_P3S_GET_STATS, &stats));
	ASSERT_TRUE(write_full(ready[1], &done, 1));
	close(ready[1]);
	ASSERT_EQ(child, waitpid(child, &status, 0));
	cow_ok = before != after && WIFEXITED(status) && !WEXITSTATUS(status);
	range_ok = stats.invalidate_starts == 1 &&
		   stats.invalidate_ends == 1 &&
		   stats.last_invalidate_start == (uintptr_t)base &&
		   stats.last_invalidate_end == (uintptr_t)base +
						NATIVE_PAGE_SIZE &&
		   !stats.bad_ranges;
	ksft_print_msg("COW PFNs %#llx/%#llx; notifier %llu/%llu [%#llx,%#llx) bad=%llu\n",
		       (unsigned long long)before, (unsigned long long)after,
		       (unsigned long long)stats.invalidate_starts,
		       (unsigned long long)stats.invalidate_ends,
		       (unsigned long long)stats.last_invalidate_start,
		       (unsigned long long)stats.last_invalidate_end,
		       (unsigned long long)stats.bad_ranges);

	EXPECT_TRUE(cow_ok);
	EXPECT_TRUE(range_ok);
	close(fd);
	munmap(raw, 2 * NATIVE_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
