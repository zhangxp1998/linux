// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
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
#define MAPPING_SIZE (4 * USER_PAGE_SIZE)
#define DRM_IOCTL_BASE 'd'
#define DRM_COMMAND_BASE 0x40

struct drm_ppps_create {
	uint64_t offset;
	uint32_t handle;
	uint32_t pad;
};

#define DRM_IOCTL_PPPS_CREATE \
	_IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE, struct drm_ppps_create)

static int run_test(void)
{
	static const uint8_t values[] = { 0x11, 0x22, 0x33, 0x44 };
	struct drm_ppps_create create = {};
	uint8_t *mapping;
	int fd;
	int i;

	ksft_print_header();
	ksft_set_plan(8);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	fd = open("/dev/drm_gem_mmap_ppps", O_RDWR | O_CLOEXEC);
	ksft_test_result(fd >= 0, "open the DRM render node\n");
	if (fd < 0)
		ksft_exit_fail_msg("open DRM device failed: %s\n",
				   strerror(errno));

	ksft_test_result(ioctl(fd, DRM_IOCTL_PPPS_CREATE, &create) == 0,
			 "create a 16K GEM object\n");
	if (!create.handle)
		ksft_exit_fail_msg("GEM create ioctl failed: %s\n",
				   strerror(errno));

	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, create.offset);
	ksft_test_result(mapping != MAP_FAILED, "map the 16K GEM object\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("GEM mmap failed: %s\n", strerror(errno));

	for (i = 0; i < 4; i++) {
		mapping[i * USER_PAGE_SIZE] = values[i];
		ksft_test_result(mapping[i * USER_PAGE_SIZE] == values[i],
				 "4K GEM slice %d is writable\n", i);
	}

	munmap(mapping, MAPPING_SIZE);
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
	execl("/proc/self/exe", "drm_gem_mmap_ppps", "--run", NULL);
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
