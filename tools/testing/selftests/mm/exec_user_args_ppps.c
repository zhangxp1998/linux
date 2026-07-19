// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE		4096UL
#define TRANSITION_COUNT	32
#define EXEC_TIMEOUT_US		500000
#define WAIT_STEP_US		1000

static const char *const native_argv[] = {
	"/proc/self/exe",
	"--native",
	NULL,
};

static int reexec_with_personality(unsigned long set, unsigned long clear,
				   const char *mode)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0 || personality((persona | set) & ~clear) < 0)
		return EXIT_FAILURE;
	execl("/proc/self/exe", "exec_user_args_ppps", mode, NULL);
	return EXIT_FAILURE;
}

static int exec_native_with_cold_argv(void)
{
	char **exec_argv;
	int memfd;
	int persona = personality(0xffffffffUL);

	if (persona < 0 ||
	    personality(persona & ~ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		return EXIT_FAILURE;
	memfd = memfd_create("exec-argv", 0);
	if (memfd < 0 || ftruncate(memfd, USER_PAGE_SIZE) ||
	    pwrite(memfd, native_argv, sizeof(native_argv), 0) !=
	    sizeof(native_argv))
		return EXIT_FAILURE;
	exec_argv = mmap(NULL, USER_PAGE_SIZE, PROT_READ, MAP_PRIVATE, memfd, 0);
	close(memfd);
	if (exec_argv == MAP_FAILED)
		return EXIT_FAILURE;
	if (madvise(exec_argv, USER_PAGE_SIZE, MADV_DONTNEED))
		return EXIT_FAILURE;
	syscall(SYS_execve, native_argv[0], exec_argv, NULL);
	return EXIT_FAILURE;
}

static bool wait_for_exec(pid_t child)
{
	int elapsed;
	int status;

	for (elapsed = 0; elapsed < EXEC_TIMEOUT_US; elapsed += WAIT_STEP_US) {
		pid_t waited = waitpid(child, &status, WNOHANG);

		if (waited == child)
			return WIFEXITED(status) &&
			       WEXITSTATUS(status) == EXIT_SUCCESS;
		if (waited < 0)
			return false;
		usleep(WAIT_STEP_US);
	}
	kill(child, SIGKILL);
	ksft_exit_fail_msg("4K-to-native exec timed out\n");
}

static int run_compat_owner(void)
{
	int i;

	ksft_print_header();
	ksft_set_plan(TRANSITION_COUNT + 1);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "owner process uses 4K pages\n");

	for (i = 0; i < TRANSITION_COUNT; i++) {
		pid_t child = fork();

		if (child < 0)
			ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
		if (!child)
			_exit(exec_native_with_cold_argv());
		ksft_test_result(wait_for_exec(child),
				 "4K-to-native exec iteration %d\n", i + 1);
	}
	ksft_finished();
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return reexec_with_personality(ADDR_4KB_COMPAT_PAGE_SIZE, 0,
					       "--owner");
	if (argc == 2 && !strcmp(argv[1], "--owner"))
		return run_compat_owner();
	if (argc == 2 && !strcmp(argv[1], "--native"))
		return sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE ||
		       sysconf(_SC_PAGESIZE) == 4 * USER_PAGE_SIZE ?
			EXIT_SUCCESS : EXIT_FAILURE;
	return EXIT_FAILURE;
}
