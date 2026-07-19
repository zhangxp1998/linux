// SPDX-License-Identifier: GPL-2.0
/*
 * A 16K GEM object created through the DRM fixture maps into a 4K compat
 * process and every one of its four 4K slices is writable.
 */
#define _GNU_SOURCE

#include <sys/ioctl.h>
#include <sys/mman.h>

#include "kselftest_ppps.h"

#define MAPPING_SIZE (4 * PROCESS_PAGE_SIZE)
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
	ksft_set_plan(7);

	fd = ppps_open_fixture_or_skip("/dev/drm_gem_mmap_ppps", O_RDWR);
	ksft_test_result(fd >= 0, "open the DRM render node\n");

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
		mapping[i * PROCESS_PAGE_SIZE] = values[i];
		ksft_test_result(mapping[i * PROCESS_PAGE_SIZE] == values[i],
				 "4K GEM slice %d is writable\n", i);
	}

	munmap(mapping, MAPPING_SIZE);
	close(fd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
