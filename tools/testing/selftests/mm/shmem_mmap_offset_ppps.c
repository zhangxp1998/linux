// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define THP_SIZE_PATH "/sys/kernel/mm/transparent_hugepage/hpage_pmd_size"

static unsigned long read_thp_size(void)
{
	char buffer[64];
	char *end;
	ssize_t bytes;
	unsigned long size;
	int fd;

	fd = open(THP_SIZE_PATH, O_RDONLY);
	if (fd < 0)
		return 0;
	bytes = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (bytes <= 0)
		return 0;
	buffer[bytes] = '\0';
	errno = 0;
	size = strtoul(buffer, &end, 10);
	if (errno || end == buffer || size < USER_PAGE_SIZE ||
	    (size & (size - 1)))
		return 0;
	return size;
}

static int run_test(int fd, const char *mount_path)
{
	unsigned long thp_size = read_thp_size();
	const off_t offset = USER_PAGE_SIZE;
	size_t length;
	void *mapping;
	bool aligned;

	if (!thp_size)
		ksft_exit_skip("PMD THP size is unavailable\n");
	length = 2 * thp_size;
	ksft_print_header();
	ksft_set_plan(3);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, offset);
	ksft_test_result(mapping != MAP_FAILED,
			 "map a huge-enabled tmpfs file from a 4K offset\n");
	aligned = mapping != MAP_FAILED &&
		  (((uintptr_t)mapping - offset) & (thp_size - 1)) == 0;
	ksft_test_result(aligned,
			 "align the shmem mapping to its file offset at PMD size\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, length);
	close(fd);
	if (umount(mount_path))
		ksft_print_msg("umount %s failed: %s\n", mount_path,
			       strerror(errno));
	if (rmdir(mount_path))
		ksft_print_msg("rmdir %s failed: %s\n", mount_path,
			       strerror(errno));
	ksft_finished();
}

static int exec_compat(int fd, const char *mount_path)
{
	char fd_string[32];
	int persona;

	persona = personality(0xffffffffUL);
	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	snprintf(fd_string, sizeof(fd_string), "%d", fd);
	execl("/proc/self/exe", "shmem_mmap_offset_ppps", "--run",
	      fd_string, mount_path, NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	char mount_path[] = "./shmem-mmap-offset-ppps-XXXXXX";
	char file_path[sizeof(mount_path) + 16];
	unsigned long thp_size;
	int fd;

	if (argc == 4 && !strcmp(argv[1], "--run"))
		return run_test(atoi(argv[2]), argv[3]);
	if (argc != 1)
		return EXIT_FAILURE;

	thp_size = read_thp_size();
	if (!thp_size)
		ksft_exit_skip("PMD THP size is unavailable\n");
	if (!mkdtemp(mount_path))
		ksft_exit_fail_msg("mkdtemp failed: %s\n", strerror(errno));
	if (mount("tmpfs", mount_path, "tmpfs", 0,
		  "size=128m,huge=always")) {
		rmdir(mount_path);
		ksft_exit_skip("huge=always tmpfs mount failed: %s\n",
			       strerror(errno));
	}
	snprintf(file_path, sizeof(file_path), "%s/test", mount_path);
	fd = open(file_path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		ksft_exit_fail_msg("open %s failed: %s\n", file_path,
				   strerror(errno));
	unlink(file_path);
	if (ftruncate(fd, USER_PAGE_SIZE + 2 * thp_size))
		ksft_exit_fail_msg("ftruncate failed: %s\n", strerror(errno));
	return exec_compat(fd, mount_path);
}
