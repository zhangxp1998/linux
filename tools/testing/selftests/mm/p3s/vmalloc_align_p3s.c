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

static bool check_backing_case(int fd, unsigned int index)
{
	const unsigned char values[] = { 0x41, 0, 0, 0x62 };
	size_t size = getpagesize();
	unsigned char *map, *reservation = MAP_FAILED;
	bool ok;
	size_t j;
	int error;

	errno = 0;
	if (size == PROCESS_PAGE_SIZE) {
		unsigned char *aligned;

		reservation = mmap(NULL, 3 * NATIVE_PAGE_SIZE, PROT_NONE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (reservation == MAP_FAILED)
			return false;
		aligned = (unsigned char *)
			(((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
			 ~(uintptr_t)(NATIVE_PAGE_SIZE - 1));
		map = mmap(aligned + PROCESS_PAGE_SIZE, size, PROT_READ,
			   MAP_SHARED | MAP_FIXED, fd,
			   (off_t)index * NATIVE_PAGE_SIZE);
	} else {
		map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd,
			   (off_t)index * NATIVE_PAGE_SIZE);
	}
	error = errno;
	if (index == 1 || index == 2) {
		ok = map == MAP_FAILED && error == EINVAL;
	} else if (size == PROCESS_PAGE_SIZE) {
		ok = map != MAP_FAILED;
		if (ok) {
			unsigned char expected = index == 0 ? 0x41 : 0x62;

			for (j = 0; j < size; j++)
				if (map[j] != expected)
					break;
			ok = j == size;
		}
	} else {
		ok = map != MAP_FAILED &&
		     mapping_has_backing_bytes(map, size, values[index]);
	}
	if (!ok)
		ksft_print_msg("backing case %u page=%zu errno=%d\n",
			       index, size, error);
	if (map != MAP_FAILED)
		munmap(map, size);
	if (reservation != MAP_FAILED)
		munmap(reservation, 3 * NATIVE_PAGE_SIZE);
	return ok;
}

TEST_F(vmalloc_align, backing_offset_zero)
{
	EXPECT_TRUE(check_backing_case(self->fd, 0));
}

TEST_F(vmalloc_align, backing_offset_one_native_page_rejected)
{
	EXPECT_TRUE(check_backing_case(self->fd, 1));
}

TEST_F(vmalloc_align, backing_offset_two_native_pages_rejected)
{
	EXPECT_TRUE(check_backing_case(self->fd, 2));
}

TEST_F(vmalloc_align, backing_offset_three_native_pages)
{
	EXPECT_TRUE(check_backing_case(self->fd, 3));
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

#define TEST_CROSSING_SLICE(n) \
TEST_F(vmalloc_align, crossing_slice_##n##_keeps_backing_bytes) \
{ \
	if (getpagesize() != PROCESS_PAGE_SIZE) \
		SKIP(return, "requires a 4KB process"); \
	EXPECT_TRUE(check_crossing_slice(self->fd, n)); \
}

TEST_CROSSING_SLICE(1)
TEST_CROSSING_SLICE(2)
TEST_CROSSING_SLICE(3)

int main(int argc, char **argv)
{
	p3s_argv0 = argv[0];
	if (argc == 2 && !strcmp(argv[1], "--native")) {
		if (getpagesize() != NATIVE_PAGE_SIZE) {
			ksft_print_header();
			ksft_exit_skip("requires native 16K pages\n");
		}
		return p3s_harness_run();
	}
	return p3s_compat_main(argc, argv, p3s_harness_run);
}
