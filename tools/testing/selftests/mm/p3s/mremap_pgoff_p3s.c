// SPDX-License-Identifier: GPL-2.0
/* Preserve a 4K process file offset and unsigned driver cookies in mremap. */
#define _GNU_SOURCE

#include <limits.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define DEVICE_PATH "/dev/mremap_pgoff_p3s"
#define HIGH_COOKIE_OFFSET (1UL << 63)

struct move_result {
	bool prepared;
	bool mapped;
	bool moved;
	bool refault_matches;
	unsigned long file_offset;
};

FIXTURE(driver_cookie) {
	int fd;
};

FIXTURE_SETUP(driver_cookie)
{
	self->fd = p3s_open_fixture(DEVICE_PATH, O_RDONLY);
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "mremap_pgoff fixture is unavailable");
	ASSERT_GE(self->fd, 0);
}

FIXTURE_TEARDOWN(driver_cookie)
{
	close(self->fd);
}

static struct move_result driver_move(int fd, bool high_cookie, bool grow)
{
	size_t reserved = (high_cookie ? 3 : 2) * PROCESS_PAGE_SIZE;
	unsigned long cookie = high_cookie ? HIGH_COOKIE_OFFSET :
						 PROCESS_PAGE_SIZE;
	struct move_result result = {};
	void *reservation, *source, *moved;

	reservation = mmap(NULL, reserved, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return result;
	if (munmap(reservation, PROCESS_PAGE_SIZE)) {
		munmap(reservation + PROCESS_PAGE_SIZE,
		       reserved - PROCESS_PAGE_SIZE);
		return result;
	}
	result.prepared = true;
	/* mmap's off_t is signed; pass the driver's unsigned cookie as-is. */
	source = (void *)syscall(__NR_mmap, reservation, PROCESS_PAGE_SIZE,
				 PROT_READ, MAP_SHARED | MAP_FIXED_NOREPLACE,
				 fd, cookie);
	result.mapped = source != MAP_FAILED;
	if (result.mapped && grow) {
		/* Reserved guard pages force growth through get_unmapped_area(). */
		moved = mremap(source, PROCESS_PAGE_SIZE,
			       2 * PROCESS_PAGE_SIZE, MREMAP_MAYMOVE);
		result.moved = moved != MAP_FAILED;
		if (result.moved)
			munmap(moved, 2 * PROCESS_PAGE_SIZE);
		else
			munmap(source, PROCESS_PAGE_SIZE);
	} else if (result.mapped) {
		munmap(source, PROCESS_PAGE_SIZE);
	}
	munmap(reservation + PROCESS_PAGE_SIZE,
	       reserved - PROCESS_PAGE_SIZE);
	return result;
}

TEST_F(driver_cookie, map_file_at_4k_offset)
{
	struct move_result result = driver_move(self->fd, false, false);

	ASSERT_TRUE(result.prepared);
	EXPECT_TRUE(result.mapped);
}

TEST_F(driver_cookie, grow_move_preserves_4k_file_offset)
{
	struct move_result result = driver_move(self->fd, false, true);

	ASSERT_TRUE(result.prepared);
	ASSERT_TRUE(result.mapped);
	EXPECT_TRUE(result.moved);
}

TEST_F(driver_cookie, map_unsigned_high_bit_cookie)
{
	struct move_result result = driver_move(self->fd, true, false);

	ASSERT_TRUE(result.prepared);
	EXPECT_TRUE(result.mapped);
}

TEST_F(driver_cookie, grow_move_preserves_high_bit_cookie)
{
	struct move_result result = driver_move(self->fd, true, true);

	ASSERT_TRUE(result.prepared);
	ASSERT_TRUE(result.mapped);
	EXPECT_TRUE(result.moved);
}

static struct move_result fixed_file_move(void)
{
	unsigned char slice0[PROCESS_PAGE_SIZE];
	unsigned char slice2[PROCESS_PAGE_SIZE];
	struct move_result result = { .file_offset = ULONG_MAX };
	unsigned char before;
	uintptr_t aligned;
	void *reservation, *source, *target, *moved;
	int fd;

	memset(slice0, 0x11, sizeof(slice0));
	memset(slice2, 0x33, sizeof(slice2));
	fd = syscall(__NR_memfd_create, "p3s-mremap-offset", MFD_CLOEXEC);
	if (fd < 0)
		return result;
	if (ftruncate(fd, NATIVE_PAGE_SIZE) ||
	    pwrite(fd, slice0, sizeof(slice0), 0) != sizeof(slice0) ||
	    pwrite(fd, slice2, sizeof(slice2), 2 * PROCESS_PAGE_SIZE) !=
		    sizeof(slice2))
		goto out_fd;
	reservation = mmap(NULL, 4 * NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		goto out_fd;
	result.prepared = true;
	aligned = ((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		  ~(NATIVE_PAGE_SIZE - 1);
	source = (void *)aligned;
	target = (void *)(aligned + NATIVE_PAGE_SIZE +
			  2 * PROCESS_PAGE_SIZE);
	source = mmap(source, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_FIXED, fd, 0);
	result.mapped = source != MAP_FAILED;
	if (!result.mapped)
		goto out_map;
	before = *(unsigned char *)source;
	moved = mremap(source, PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED, target);
	result.moved = moved != MAP_FAILED;
	if (!result.moved)
		goto out_map;
	result.file_offset = p3s_maps_offset(moved);
	if (!madvise(moved, PROCESS_PAGE_SIZE, MADV_DONTNEED))
		result.refault_matches = before == 0x11 &&
			*(unsigned char *)moved == 0x11;

out_map:
	munmap(reservation, 4 * NATIVE_PAGE_SIZE);
out_fd:
	close(fd);
	return result;
}

TEST(fixed_remap_moves_to_different_virtual_slice)
{
	struct move_result result = fixed_file_move();

	ASSERT_TRUE(result.prepared);
	ASSERT_TRUE(result.mapped);
	EXPECT_TRUE(result.moved);
}

TEST(fixed_remap_keeps_file_offset)
{
	struct move_result result = fixed_file_move();

	ASSERT_TRUE(result.prepared);
	ASSERT_TRUE(result.moved);
	EXPECT_EQ(0, result.file_offset);
}

TEST(fixed_remap_refaults_original_file_slice)
{
	struct move_result result = fixed_file_move();

	ASSERT_TRUE(result.prepared);
	ASSERT_TRUE(result.moved);
	EXPECT_TRUE(result.refault_matches);
}

P3S_HARNESS_COMPAT_MAIN
