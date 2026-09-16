// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include "kselftest_ppps.h"
#include "drm_gem_dma_import_ppps.h"

static int run_test(void)
{
	static const char * const names[] = {
		"accept native-aligned size and DMA address",
		"reject sub-native buffer size",
		"reject sub-native DMA address alignment",
		"reject an SG extent shorter than the buffer",
		"accept contiguous DMA segments",
		"reject a gap within the required DMA extent",
	};
	unsigned int i;
	int fd;

	ksft_print_header();
	ksft_set_plan(DRM_DMA_CASES);
	fd = ppps_open_fixture_or_skip("/dev/drm_dma_import_ppps", O_RDWR);
	for (i = 0; i < DRM_DMA_CASES; i++) {
		int ret = ioctl(fd, DRM_DMA_IMPORT_PPPS_CHECK, (unsigned long)i);

		ksft_test_result(!ret, "%s (errno=%d)\n", names[i], ret ? errno : 0);
	}
	close(fd);
	ksft_finished();
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--native")) {
		if (getpagesize() != NATIVE_PAGE_SIZE)
			exec_native(argv[0], "--native", NULL);
		return run_test();
	}
	return ppps_compat_main(argc, argv, run_test);
}
