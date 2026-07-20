// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define TEST_PLANE_SIZE (6 * 1024UL)
#define VALID_MAP_SIZE (2 * USER_PAGE_SIZE)
#define OVERSIZED_MAP_SIZE (3 * USER_PAGE_SIZE)
#define VB2_MMAP_PPPS_EXPBUF _IO('v', 0x70)

static int run_native_test(void)
{
	long page_size = sysconf(_SC_PAGESIZE);
	size_t valid_map_size;
	void *mapping;
	struct stat st = {};
	int saved_errno;
	int dmabuf_fd;
	int fd;

	ksft_print_header();
	ksft_set_plan(6);
	ksft_test_result(page_size == USER_PAGE_SIZE || page_size == 16384,
			 "native process uses a supported page size (%ld)\n",
			 page_size);
	if (page_size != USER_PAGE_SIZE && page_size != 16384)
		ksft_exit_fail_msg("unsupported native page size: %ld\n",
				   page_size);
	valid_map_size = (TEST_PLANE_SIZE + page_size - 1) & ~(page_size - 1);

	fd = open("/dev/vb2_mmap_ppps", O_RDWR | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the videobuf2 mmap test device\n");
	if (fd < 0)
		ksft_exit_fail_msg("open test device failed: %s\n",
				   strerror(errno));
	dmabuf_fd = ioctl(fd, VB2_MMAP_PPPS_EXPBUF);
	ksft_test_result(dmabuf_fd >= 0, "export the plane as a DMA-BUF\n");
	if (dmabuf_fd < 0)
		ksft_exit_fail_msg("export DMA-BUF failed: %s\n",
				   strerror(errno));
	ksft_test_result(fstat(dmabuf_fd, &st) == 0 &&
			 st.st_size >= (off_t)TEST_PLANE_SIZE &&
			 st.st_size <= (off_t)valid_map_size,
			 "DMA-BUF fits its native process-page range (%lld)\n",
			 (long long)st.st_size);
	mapping = mmap(NULL, valid_map_size, PROT_READ, MAP_SHARED,
		       dmabuf_fd, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map the DMA-BUF through its final native process page\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, valid_map_size);

	errno = 0;
	mapping = mmap(NULL, valid_map_size + page_size, PROT_READ, MAP_SHARED,
		       dmabuf_fd, 0);
	saved_errno = errno;
	ksft_test_result(mapping == MAP_FAILED && saved_errno == EINVAL,
			 "reject a mapping one native process page too long (errno=%d)\n",
			 saved_errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, valid_map_size + page_size);
	close(dmabuf_fd);
	close(fd);
	ksft_finished();
}

static int run_test(void)
{
	void *mapping;
	struct stat st = {};
	int saved_errno;
	int dmabuf_fd;
	int fd;

	ksft_print_header();
	ksft_set_plan(9);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fd = open("/dev/vb2_mmap_ppps", O_RDWR | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the videobuf2 mmap test device\n");
	if (fd < 0)
		ksft_exit_fail_msg("open test device failed: %s\n",
				   strerror(errno));

	mapping = mmap(NULL, VALID_MAP_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map the 6K plane rounded to two process pages\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, VALID_MAP_SIZE);

	errno = 0;
	mapping = mmap(NULL, USER_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
		       USER_PAGE_SIZE);
	saved_errno = errno;
	ksft_test_result(mapping == MAP_FAILED && saved_errno == EINVAL,
			 "reject a cookie selecting a nonexistent plane (errno=%d)\n",
			 saved_errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, USER_PAGE_SIZE);

	errno = 0;
	mapping = mmap(NULL, OVERSIZED_MAP_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	saved_errno = errno;
	ksft_test_result(mapping == MAP_FAILED && saved_errno == EINVAL,
			 "reject a 12K mapping of the 6K plane (errno=%d)\n",
			 saved_errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, OVERSIZED_MAP_SIZE);

	dmabuf_fd = ioctl(fd, VB2_MMAP_PPPS_EXPBUF);
	ksft_test_result(dmabuf_fd >= 0, "export the plane as a DMA-BUF\n");
	if (dmabuf_fd < 0)
		ksft_exit_fail_msg("export DMA-BUF failed: %s\n",
				   strerror(errno));
	ksft_test_result(fstat(dmabuf_fd, &st) == 0 &&
			 st.st_size >= (off_t)TEST_PLANE_SIZE &&
			 st.st_size <= (off_t)VALID_MAP_SIZE,
			 "DMA-BUF size does not expose native-page padding (%lld)\n",
			 (long long)st.st_size);
	mapping = mmap(NULL, VALID_MAP_SIZE, PROT_READ, MAP_SHARED,
		       dmabuf_fd, 0);
	ksft_test_result(mapping != MAP_FAILED,
			 "map the DMA-BUF through its final process page\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, VALID_MAP_SIZE);

	errno = 0;
	mapping = mmap(NULL, OVERSIZED_MAP_SIZE, PROT_READ, MAP_SHARED,
		       dmabuf_fd, 0);
	saved_errno = errno;
	ksft_test_result(mapping == MAP_FAILED && saved_errno == EINVAL,
			 "reject a 12K DMA-BUF mapping (errno=%d)\n",
			 saved_errno);
	if (mapping != MAP_FAILED)
		munmap(mapping, OVERSIZED_MAP_SIZE);
	close(dmabuf_fd);
	close(fd);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "vb2_mmap_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

static int exec_native(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona & ~ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality clear failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "vb2_mmap_ppps", "--native-run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	if (argc == 2 && !strcmp(argv[1], "--native"))
		return exec_native();
	if (argc == 2 && !strcmp(argv[1], "--native-run"))
		return run_native_test();
	return EXIT_FAILURE;
}
