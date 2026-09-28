// SPDX-License-Identifier: GPL-2.0
/*
 * With MMAP_PAGE_ZERO in its personality, a 4KB process gets a
 * page-zero mapping that spans exactly one process page.
 */
#define _GNU_SOURCE

#include <sys/mman.h>

#define P3S_HARNESS_ROLE check_page_zero_role
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

static int check_page_zero_role(int argc, char **argv)
{
	unsigned long end = 0;
	char perms[5] = {};
	void *zero;

	if (argc != 2 || strcmp(argv[1], "--check-page-zero"))
		return -1;
	if (!p3s_is_4kb_process())
		return EXIT_FAILURE;
	if (!read_zero_mapping(&end, perms))
		return end == PROCESS_PAGE_SIZE ? EXIT_SUCCESS : EXIT_FAILURE;

	errno = 0;
	zero = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_EXEC,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (zero == MAP_FAILED && (errno == EPERM || errno == EACCES))
		return KSFT_SKIP;
	if (zero != MAP_FAILED)
		munmap(zero, PROCESS_PAGE_SIZE);
	return EXIT_FAILURE;
}

TEST(page_zero_maps_one_4kb_page)
{
	pid_t child = fork();
	int persona;
	int status;

	ASSERT_GE(child, 0);
	if (!child) {
		persona = personality(0xffffffffUL);
		if (persona < 0)
			_exit(EXIT_FAILURE);
		if (personality((unsigned int)(persona | MMAP_PAGE_ZERO)) < 0)
			_exit(errno == EPERM || errno == EACCES || errno == EINVAL ?
			      KSFT_SKIP : EXIT_FAILURE);
		p3s_execl(true, NULL, "--check-page-zero", NULL);
		_exit(errno == EPERM || errno == EACCES || errno == EINVAL ?
		      KSFT_SKIP : EXIT_FAILURE);
	}
	ASSERT_EQ(child, waitpid(child, &status, 0));
	ASSERT_TRUE(WIFEXITED(status));
	if (WEXITSTATUS(status) == KSFT_SKIP)
		SKIP(return, "address-zero mapping is unavailable");
	EXPECT_EQ(EXIT_SUCCESS, WEXITSTATUS(status));
}

P3S_HARNESS_4KB_MAIN
