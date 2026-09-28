// SPDX-License-Identifier: GPL-2.0
/*
 * Stress the 4KB-to-native geometry switch on exec (aosp/4367990): a 4KB
 * owner repeatedly execs a native child whose argv array lives in a cold,
 * not-yet-faulted 4KB file page. Every exec must complete with native pages.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define P3S_HARNESS_ROLE native_role
#include "p3s_harness.h"

#define TRANSITION_COUNT	32

static const char *const native_argv[] = {
	"/proc/self/exe",
	"--check-native",
	NULL,
};

static int exec_native_with_cold_argv(void)
{
	char **exec_argv;
	int memfd;
	int persona = personality(0xffffffffUL);

	if (persona < 0 ||
	    personality(persona & ~ADDR_PAGE_SIZE_4KB) < 0)
		return EXIT_FAILURE;
	memfd = memfd_create("exec-argv", 0);
	if (memfd < 0 || ftruncate(memfd, PROCESS_PAGE_SIZE) ||
	    pwrite(memfd, native_argv, sizeof(native_argv), 0) !=
	    sizeof(native_argv))
		return EXIT_FAILURE;
	exec_argv = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ, MAP_PRIVATE, memfd, 0);
	close(memfd);
	if (exec_argv == MAP_FAILED)
		return EXIT_FAILURE;
	if (madvise(exec_argv, PROCESS_PAGE_SIZE, MADV_DONTNEED))
		return EXIT_FAILURE;
	syscall(SYS_execve, native_argv[0], exec_argv, NULL);
	return EXIT_FAILURE;
}

static bool wait_for_exec(pid_t child)
{
	int status;

	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR)
			return false;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
}

TEST(cold_4k_argv_survives_repeated_native_exec)
{
	int i;

	for (i = 0; i < TRANSITION_COUNT; i++) {
		pid_t child = fork();

		ASSERT_GE(child, 0);
		if (!child)
			_exit(exec_native_with_cold_argv());
		EXPECT_TRUE(wait_for_exec(child)) TH_LOG("iteration %d", i + 1);
	}
}

static int native_role(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--check-native"))
		return sysconf(_SC_PAGESIZE) == NATIVE_PAGE_SIZE &&
		       p3s_has_native_pages() ?
			EXIT_SUCCESS : EXIT_FAILURE;
	return -1;
}

P3S_HARNESS_4KB_MAIN
