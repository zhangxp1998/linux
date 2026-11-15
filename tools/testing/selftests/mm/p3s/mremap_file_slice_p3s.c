// SPDX-License-Identifier: GPL-2.0
/* Preserve process-page file offsets across fixed and automatic mremap moves. */
#define _GNU_SOURCE

#include <limits.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define GROW_OLD_SIZE	(4 * NATIVE_PAGE_SIZE)
#define GROW_NEW_SIZE	(2 * GROW_OLD_SIZE)

struct remap_result {
	bool prepared;
	bool mapped;
	bool moved;
	bool offset_ok;
	bool bytes_ok;
};

static struct remap_result fixed_source_slice(void)
{
	unsigned char page[PROCESS_PAGE_SIZE];
	struct remap_result result = {};
	uintptr_t aligned;
	void *reservation, *source, *target, *moved;
	int fd;
	unsigned int i;

	fd = syscall(__NR_memfd_create, "p3s-mremap-source-slice", MFD_CLOEXEC);
	if (fd < 0)
		return result;
	if (ftruncate(fd, NATIVE_PAGE_SIZE))
		goto out_fd;
	for (i = 0; i < NATIVE_PAGE_SIZE / PROCESS_PAGE_SIZE; i++) {
		memset(page, 0x41 + i, sizeof(page));
		if (pwrite(fd, page, sizeof(page), i * PROCESS_PAGE_SIZE) !=
		    sizeof(page))
			goto out_fd;
	}
	reservation = mmap(NULL, 5 * NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		goto out_fd;
	result.prepared = true;
	aligned = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		  ~(NATIVE_PAGE_SIZE - 1);
	source = mmap((void *)aligned, NATIVE_PAGE_SIZE,
		      PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
	result.mapped = source != MAP_FAILED;
	if (!result.mapped)
		goto out_map;
	target = (void *)(aligned + 2 * NATIVE_PAGE_SIZE);
	/* Move an unfaulted source slice to a different virtual slice. */
	moved = mremap(source + PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
		       PROCESS_PAGE_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, target);
	result.moved = moved != MAP_FAILED;
	if (result.moved) {
		result.offset_ok = p3s_maps_offset(moved) == PROCESS_PAGE_SIZE;
		if (!madvise(moved, PROCESS_PAGE_SIZE, MADV_DONTNEED))
			result.bytes_ok = *(unsigned char *)moved == 0x42;
	}
out_map:
	munmap(reservation, 5 * NATIVE_PAGE_SIZE);
out_fd:
	close(fd);
	return result;
}

TEST(fixed_remap_moves_nonzero_file_slice)
{
	struct remap_result r = fixed_source_slice();

	ASSERT_TRUE(r.prepared);
	ASSERT_TRUE(r.mapped);
	EXPECT_TRUE(r.moved);
}

TEST(fixed_remap_preserves_source_file_offset)
{
	struct remap_result r = fixed_source_slice();

	ASSERT_TRUE(r.prepared);
	ASSERT_TRUE(r.moved);
	EXPECT_TRUE(r.offset_ok);
}

TEST(fixed_remap_refaults_source_slice_data)
{
	struct remap_result r = fixed_source_slice();

	ASSERT_TRUE(r.prepared);
	ASSERT_TRUE(r.moved);
	EXPECT_TRUE(r.bytes_ok);
}

static struct remap_result growing_file_mapping(void)
{
	const size_t old_len = GROW_OLD_SIZE;
	const size_t new_len = GROW_NEW_SIZE;
	struct remap_result result = {};
	uintptr_t aligned;
	void *reservation, *source, *guard, *moved;
	unsigned char head = 0x59, tail = 0x6d;
	int fd;

	fd = syscall(__NR_memfd_create, "p3s-mremap-gua", MFD_CLOEXEC);
	if (fd < 0)
		return result;
	if (ftruncate(fd, new_len + PROCESS_PAGE_SIZE) ||
	    pwrite(fd, &head, 1, PROCESS_PAGE_SIZE) != 1 ||
	    pwrite(fd, &tail, 1, new_len) != 1)
		goto out_fd;
	reservation = mmap(NULL, new_len + 2 * NATIVE_PAGE_SIZE,
			   PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		goto out_fd;
	result.prepared = true;
	aligned = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		  ~(NATIVE_PAGE_SIZE - 1);
	source = mmap((void *)aligned, old_len, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_FIXED, fd, PROCESS_PAGE_SIZE);
	guard = mmap((void *)(aligned + old_len), PROCESS_PAGE_SIZE,
		     PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	result.mapped = source != MAP_FAILED && guard != MAP_FAILED;
	if (!result.mapped)
		goto out_map;
	moved = mremap(source, old_len, new_len, MREMAP_MAYMOVE);
	result.moved = moved != MAP_FAILED;
	if (result.moved) {
		result.offset_ok = p3s_maps_offset(moved) == PROCESS_PAGE_SIZE;
		result.bytes_ok = *(unsigned char *)moved == head &&
			*((unsigned char *)moved + new_len - PROCESS_PAGE_SIZE) == tail;
		munmap(moved, new_len);
	}
out_map:
	munmap(reservation, new_len + 2 * NATIVE_PAGE_SIZE);
out_fd:
	close(fd);
	return result;
}

TEST(automatic_growth_preserves_4k_file_offset)
{
	struct remap_result r = growing_file_mapping();
	ASSERT_TRUE(r.prepared);
	ASSERT_TRUE(r.mapped);
	ASSERT_TRUE(r.moved);
	EXPECT_TRUE(r.offset_ok);
}

TEST(automatic_growth_keeps_and_extends_file_bytes)
{
	struct remap_result r = growing_file_mapping();
	ASSERT_TRUE(r.prepared);
	ASSERT_TRUE(r.mapped);
	ASSERT_TRUE(r.moved);
	EXPECT_TRUE(r.bytes_ok);
}

P3S_HARNESS_COMPAT_MAIN
