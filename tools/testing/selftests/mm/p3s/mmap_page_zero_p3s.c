// SPDX-License-Identifier: GPL-2.0
/*
 * With MMAP_PAGE_ZERO in its personality, a 4K compat process gets a
 * page-zero mapping that spans exactly one process page.
 */
#define _GNU_SOURCE

#include "p3s_harness.h"

static int read_zero_mapping(unsigned long *endp, char perms[5])
{
	char line[256];
	FILE *maps;

	maps = fopen("/proc/self/maps", "re");
	if (!maps)
		return -1;
	while (fgets(line, sizeof(line), maps)) {
		unsigned long start, end;

		if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) == 3 &&
		    start == 0) {
			*endp = end;
			fclose(maps);
			return 0;
		}
	}
	fclose(maps);
	return -1;
}

TEST(page_zero_maps_one_compat_page)
{
	unsigned long page_size = sysconf(_SC_PAGESIZE);
	unsigned long end = 0;
	char perms[5] = {};
	bool found;

	p3s_require_compat();
	found = read_zero_mapping(&end, perms) == 0;
	if (!found) {
		void *zero;

		errno = 0;
		zero = mmap(NULL, page_size, PROT_READ,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			    -1, 0);
		if (zero == MAP_FAILED && (errno == EPERM || errno == EACCES))
			SKIP(return, "address-zero mapping denied: %s",
			     strerror(errno));
		if (zero != MAP_FAILED)
			munmap(zero, page_size);
	}

	EXPECT_TRUE(found && end == page_size);
	ksft_print_msg("process page size %lu, page-zero mapping 0-%lx %s\n",
		       page_size, end, found ? perms : "missing");
}

/*
 * Hand-written main: the compat re-exec also needs MMAP_PAGE_ZERO in the
 * personality, which the standard main does not set.
 */
int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);
	int persona;

	if (mode && argc == 2 && !strcmp(mode, P3S_RUN_FLAG))
		return p3s_harness_run();
	if (argc != 1)
		ksft_exit_fail_msg("unexpected arguments\n");
	persona = personality(0xffffffffUL);
	if (persona < 0)
		ksft_exit_fail_msg("failed to read personality\n");
	if (personality((unsigned int)(persona | MMAP_PAGE_ZERO)) < 0) {
		if (errno == EPERM || errno == EACCES || errno == EINVAL)
			ksft_exit_skip("MMAP_PAGE_ZERO unavailable: %s\n",
				       strerror(errno));
		ksft_exit_fail_msg("failed to set personality: %s\n",
				   strerror(errno));
	}
	exec_compat(argv[0], P3S_RUN_FLAG, NULL);
}
