// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/vfio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define NATIVE_PAGE_SIZE 16384UL

static int open_group(void)
{
	struct dirent *entry;
	char path[PATH_MAX];
	DIR *dir;
	int fd = -1;

	dir = opendir("/dev/vfio");
	if (!dir)
		return -1;
	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.')
			continue;
		if ((size_t)snprintf(path, sizeof(path), "/dev/vfio/%s",
				      entry->d_name) >= sizeof(path))
			continue;
		fd = open(path, O_RDWR);
		if (fd >= 0)
			break;
	}
	closedir(dir);
	return fd;
}

static int run_test(void)
{
	char *name;
	int saved_errno;
	int group_fd;
	int ret;

	ksft_print_header();
	ksft_set_plan(3);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	group_fd = open_group();
	ksft_test_result(group_fd >= 0, "open the VFIO group fixture\n");
	if (group_fd < 0)
		ksft_exit_fail_msg("open VFIO group failed: %s\n",
				   strerror(errno));

	name = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (name == MAP_FAILED)
		ksft_exit_fail_msg("mmap failed: %s\n", strerror(errno));
	memset(name, 'x', NATIVE_PAGE_SIZE);
	name[USER_PAGE_SIZE] = '\0';

	errno = 0;
	ret = ioctl(group_fd, VFIO_GROUP_GET_DEVICE_FD, name);
	saved_errno = errno;
	ksft_print_msg("VFIO_GROUP_GET_DEVICE_FD returned %d, errno %d (%s)\n",
		       ret, saved_errno, strerror(saved_errno));
	ksft_test_result(ret == -1 && saved_errno == EINVAL,
			 "bound VFIO device names to the process page\n");

	munmap(name, NATIVE_PAGE_SIZE);
	close(group_fd);
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
	execl("/proc/self/exe", "vfio_group_name_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	return EXIT_FAILURE;
}
