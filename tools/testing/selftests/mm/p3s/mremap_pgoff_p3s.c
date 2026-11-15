// SPDX-License-Identifier: GPL-2.0
/* Preserve 4KB file offsets and unsigned driver cookies in mremap. */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define DEVICE_PATH "/dev/mremap_pgoff_p3s"
#define HIGH_COOKIE_OFFSET (1UL << 63)

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

static void check_driver_move(struct __test_metadata *_metadata, int fd,
			      bool high_cookie, bool grow)
{
	size_t reserved = (high_cookie ? 3 : 2) * PROCESS_PAGE_SIZE;
	unsigned long cookie = high_cookie ? HIGH_COOKIE_OFFSET :
						PROCESS_PAGE_SIZE;
	void *reservation, *source, *moved;

	reservation = mmap(NULL, reserved, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(reservation, MAP_FAILED);
	ASSERT_EQ(0, munmap(reservation, PROCESS_PAGE_SIZE));
	/* mmap's off_t is signed; pass the driver's unsigned cookie as-is. */
	source = (void *)syscall(__NR_mmap, reservation, PROCESS_PAGE_SIZE,
				 PROT_READ, MAP_SHARED | MAP_FIXED_NOREPLACE,
				 fd, cookie);
	ASSERT_NE(source, MAP_FAILED);
	ASSERT_EQ(reservation, source);
	if (grow) {
		/* Reserved guard pages force growth through get_unmapped_area(). */
		moved = mremap(source, PROCESS_PAGE_SIZE,
			       2 * PROCESS_PAGE_SIZE, MREMAP_MAYMOVE);
		ASSERT_NE(moved, MAP_FAILED);
		ASSERT_EQ(0, munmap(moved, 2 * PROCESS_PAGE_SIZE));
	} else {
		ASSERT_EQ(0, munmap(source, PROCESS_PAGE_SIZE));
	}
	ASSERT_EQ(0, munmap(reservation + PROCESS_PAGE_SIZE,
			    reserved - PROCESS_PAGE_SIZE));
}

TEST_F(driver_cookie, map_file_at_4k_offset)
{
	check_driver_move(_metadata, self->fd, false, false);
}

TEST_F(driver_cookie, grow_move_preserves_4k_file_offset)
{
	check_driver_move(_metadata, self->fd, false, true);
}

TEST_F(driver_cookie, map_unsigned_high_bit_cookie)
{
	check_driver_move(_metadata, self->fd, true, false);
}

TEST_F(driver_cookie, grow_move_preserves_high_bit_cookie)
{
	check_driver_move(_metadata, self->fd, true, true);
}

P3S_HARNESS_4KB_MAIN
