// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/wait.h>
#include "p3s_test.h"
#include "p3s_harness.h"

static bool mapping_has_backing_bytes(const unsigned char *map,
				      size_t size, unsigned char first)
{
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return false;
	if (!pid) {
		size_t offset;

		for (offset = 0; offset < size; offset++)
			if (map[offset] != first + offset / PROCESS_PAGE_SIZE) {
				dprintf(STDERR_FILENO,
					"first mismatch at %#zx: mapped=%#x expected=%#lx\n",
					offset, map[offset],
					first + offset / PROCESS_PAGE_SIZE);
				_exit(1);
			}
		_exit(0);
	}
	if (waitpid(pid, &status, 0) != pid)
		return false;
	if (WIFSIGNALED(status))
		ksft_print_msg("mapping access killed child with signal %d\n",
			       WTERMSIG(status));
	return WIFEXITED(status) && !WEXITSTATUS(status);
}

FIXTURE(vmalloc_align) {
	int fd;
};

FIXTURE_SETUP(vmalloc_align)
{
	self->fd = p3s_open_fixture("/dev/vmalloc_align_p3s", O_RDWR);
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "vmalloc_align fixture unavailable");
	ASSERT_GE(self->fd, 0);
}

FIXTURE_TEARDOWN(vmalloc_align)
{
	close(self->fd);
}

static bool check_backing_case(int fd, off_t offset, unsigned char expected)
{
	size_t size = PROCESS_PAGE_SIZE;
	unsigned char *map, *reservation, *aligned;
	bool ok = false;
	size_t j;
	int error;

	reservation = mmap(NULL, 3 * NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return false;
	aligned = (unsigned char *)
		(((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
		 ~(uintptr_t)(NATIVE_PAGE_SIZE - 1));
	errno = 0;
	map = mmap(aligned + PROCESS_PAGE_SIZE, size, PROT_READ,
		   MAP_SHARED | MAP_FIXED, fd, offset);
	error = errno;
	if (map != MAP_FAILED) {
		for (j = 0; j < size; j++)
			if (map[j] != expected)
				break;
		ok = j == size;
		munmap(map, size);
	}
	if (!ok)
		ksft_print_msg("backing offset %lld errno=%d\n",
			       (long long)offset, error);
	munmap(reservation, 3 * NATIVE_PAGE_SIZE);
	return ok;
}

TEST_F(vmalloc_align, backing_offset_zero)
{
	EXPECT_TRUE(check_backing_case(self->fd, 0, 0x41));
}

TEST_F(vmalloc_align, backing_offset_three_native_pages)
{
	EXPECT_TRUE(check_backing_case(self->fd, 3 * NATIVE_PAGE_SIZE, 0x62));
}

TEST_F(vmalloc_align, complete_native_backing_maps)
{
	unsigned char *map = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ,
				  MAP_SHARED, self->fd, 0);

	ASSERT_NE(map, MAP_FAILED);
	EXPECT_TRUE(mapping_has_backing_bytes(map, NATIVE_PAGE_SIZE, 0x41));
	munmap(map, NATIVE_PAGE_SIZE);
}

static bool check_crossing_slice(int fd, unsigned int slice)
{
	unsigned char *map;
	bool ok = true;
	size_t j;

	map = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED,
		   fd, (off_t)slice * PROCESS_PAGE_SIZE);
	if (map == MAP_FAILED)
		return false;
	for (j = 0; j < NATIVE_PAGE_SIZE; j++) {
		size_t pos = slice * PROCESS_PAGE_SIZE + j;
		unsigned char expected =
			(pos < NATIVE_PAGE_SIZE ? 0x41 : 0x62) +
			(pos % NATIVE_PAGE_SIZE) / PROCESS_PAGE_SIZE;

		if (map[j] != expected) {
			ok = false;
			break;
		}
	}
	munmap(map, NATIVE_PAGE_SIZE);
	return ok;
}

TEST_F(vmalloc_align, crossing_slice_1_keeps_backing_bytes)
{
	EXPECT_TRUE(check_crossing_slice(self->fd, 1));
}

TEST_F(vmalloc_align, crossing_slice_2_keeps_backing_bytes)
{
	EXPECT_TRUE(check_crossing_slice(self->fd, 2));
}

TEST_F(vmalloc_align, crossing_slice_3_keeps_backing_bytes)
{
	EXPECT_TRUE(check_crossing_slice(self->fd, 3));
}

P3S_HARNESS_4KB_MAIN
