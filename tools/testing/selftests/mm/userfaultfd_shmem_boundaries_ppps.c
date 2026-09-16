// SPDX-License-Identifier: GPL-2.0
/* Byte-range holes must not turn partially retained UFFD slices into holes. */
#define _GNU_SOURCE
#include <linux/falloc.h>
#include <linux/memfd.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "kselftest_ppps.h"

static int new_uffd(void)
{
	struct uffdio_api api = { .api = UFFD_API };
	int fd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);

	if (fd < 0) {
		if (errno == EPERM || errno == ENOSYS)
			ksft_exit_skip("userfaultfd unavailable\n");
		ksft_exit_fail_msg("userfaultfd: %s\n", strerror(errno));
	}
	if (ioctl(fd, UFFDIO_API, &api))
		ksft_exit_fail_msg("UFFDIO_API: %s\n", strerror(errno));
	if (!(api.features & UFFD_FEATURE_MISSING_SHMEM))
		ksft_exit_skip("UFFD shmem missing mode unavailable\n");
	return fd;
}

static void check_range(const char *name, size_t start, size_t len, bool truncate)
{
	size_t page = getpagesize(), size = 4 * page;
	struct uffdio_register reg = { .mode = UFFDIO_REGISTER_MODE_MISSING };
	unsigned char *map, *source, *expected, *actual;
	bool masks_ok = true;
	int fd, uffd;
	size_t i;

	uffd = new_uffd();
	fd = memfd_create("ppps-uffd-boundaries", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size))
		ksft_exit_fail_msg("memfd setup: %s\n", strerror(errno));
	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	source = mmap(NULL, size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	expected = malloc(size);
	actual = malloc(size);
	if (map == MAP_FAILED || source == MAP_FAILED || !expected || !actual)
		ksft_exit_fail_msg("buffer allocation failed\n");
	for (i = 0; i < size; i++)
		source[i] = 1 + i % 251;
	memcpy(expected, source, size);
	reg.range.start = (unsigned long)map;
	reg.range.len = size;
	if (ioctl(uffd, UFFDIO_REGISTER, &reg))
		ksft_exit_fail_msg("UFFDIO_REGISTER: %s\n", strerror(errno));
	for (i = 0; i < size; i += page) {
		struct uffdio_copy copy = {
			.src = (unsigned long)source + i,
			.dst = (unsigned long)map + i,
			.len = page,
		};

		if (ioctl(uffd, UFFDIO_COPY, &copy) || copy.copy != page)
			ksft_exit_fail_msg("initial COPY failed\n");
	}
	if (truncate) {
		if (ftruncate(fd, start) || ftruncate(fd, size))
			ksft_exit_fail_msg("truncate/regrow: %s\n", strerror(errno));
	} else if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			     start, len)) {
		ksft_exit_fail_msg("hole punch: %s\n", strerror(errno));
	}
	/* Test slice presence, not EEXIST from an already installed PTE. */
	if (madvise(map, size, MADV_DONTNEED))
		ksft_exit_fail_msg("drop mapping PTEs: %s\n", strerror(errno));
	memset(expected + start, 0, len);
	/* pread checks file bytes without blocking on a registered missing PTE. */
	ksft_test_result(pread(fd, actual, size, 0) == size &&
			 !memcmp(actual, expected, size),
			 "%s: only the requested bytes become zero\n", name);
	for (i = 0; i < size; i += page) {
		struct uffdio_copy copy = {
			.src = (unsigned long)source + i,
			.dst = (unsigned long)map + i,
			.len = page,
		};
		bool whole = i >= start && i + page <= start + len;
		int ret = ioctl(uffd, UFFDIO_COPY, &copy);

		if (whole) {
			masks_ok &= !ret && copy.copy == page;
			memcpy(expected + i, source + i, page);
		} else {
			masks_ok &= ret == -1 && errno == EEXIST &&
				    copy.copy == -EEXIST;
		}
	}
	ksft_test_result(masks_ok, "%s: only whole pages become missing\n", name);
	ksft_test_result(pread(fd, actual, size, 0) == size &&
			 !memcmp(actual, expected, size),
			 "%s: COPY leaves retained bytes intact\n", name);
	if (ioctl(uffd, UFFDIO_UNREGISTER, &reg.range))
		ksft_exit_fail_msg("UFFDIO_UNREGISTER failed\n");
	free(actual);
	free(expected);
	munmap(source, size);
	munmap(map, size);
	close(fd);
	close(uffd);
}

static int run_test(void)
{
	size_t page = getpagesize();

	ksft_print_header();
	ksft_set_plan(24);
	check_range("partial middle", page / 4, page / 2, false);
	check_range("partial prefix", 0, page / 2, false);
	check_range("partial suffix", 4 * page - page / 2, page / 2, false);
	check_range("straddling boundary", page - 1, 2, false);
	check_range("whole page", page, page, false);
	check_range("whole and partial pages", page / 2, 2 * page, false);
	check_range("partial EOF/regrow", page + page / 2, 5 * page / 2, true);
	check_range("aligned EOF/regrow", 2 * page, 2 * page, true);
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
