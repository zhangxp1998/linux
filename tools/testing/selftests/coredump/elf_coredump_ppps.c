// SPDX-License-Identifier: GPL-2.0
/* Check that a 4K PPPS process uses a 4K ELF core-dump threshold. */
#define _GNU_SOURCE

#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "kselftest_ppps.h"

#define CORE_LIMIT (2 * PROCESS_PAGE_SIZE)
#define CORE_PATTERN_PATH "/proc/sys/kernel/core_pattern"

static bool set_core_pattern(const char *pattern, size_t length)
{
	int fd = open(CORE_PATTERN_PATH, O_WRONLY | O_CLOEXEC);
	bool ok;

	if (fd < 0)
		return false;
	ok = write_full(fd, pattern, length);
	if (close(fd))
		ok = false;
	return ok;
}

static ssize_t save_core_pattern(char *pattern, size_t size)
{
	int fd = open(CORE_PATTERN_PATH, O_RDONLY | O_CLOEXEC);
	ssize_t length;

	if (fd < 0)
		return -1;
	length = read(fd, pattern, size);
	if (close(fd) && length >= 0)
		return -1;
	return length;
}

static int run_test(void)
{
	char original_pattern[256];
	char temporary[] = "/tmp/elf-coredump-ppps.XXXXXX";
	struct rlimit limit = { CORE_LIMIT, CORE_LIMIT };
	struct stat statbuf = {};
	ssize_t pattern_length;
	bool core_dumped;
	int cwd_fd = -1;
	int status = 0;
	pid_t child;

	ksft_print_header();
	ksft_set_plan(1);

	pattern_length = save_core_pattern(original_pattern,
					   sizeof(original_pattern));
	if (pattern_length < 0 ||
	    !set_core_pattern("core", strlen("core"))) {
		ksft_test_result_skip("core_pattern is not writable\n");
		return KSFT_SKIP;
	}

	cwd_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (cwd_fd < 0) {
		ksft_test_result_fail("prepare private core directory\n");
		goto out_restore;
	}
	if (!mkdtemp(temporary)) {
		ksft_test_result_fail("prepare private core directory\n");
		goto out_restore;
	}
	if (chdir(temporary)) {
		ksft_test_result_fail("prepare private core directory\n");
		rmdir(temporary);
		goto out_restore;
	}
	if (setrlimit(RLIMIT_CORE, &limit)) {
		ksft_test_result_skip("8K RLIMIT_CORE is unavailable\n");
		goto out_directory;
	}

	child = fork();
	if (child < 0) {
		ksft_test_result_fail("fork a crashing child\n");
		goto out_directory;
	}
	if (!child) {
		prctl(PR_SET_DUMPABLE, 1);
		raise(SIGSEGV);
		_exit(127);
	}
	if (waitpid(child, &status, 0) != child) {
		ksft_test_result_fail("wait for the crashing child\n");
		goto out_directory;
	}

	core_dumped = WIFSIGNALED(status) && WCOREDUMP(status) &&
		!stat("core", &statbuf) && statbuf.st_size > 0 &&
		statbuf.st_size <= (off_t)CORE_LIMIT;
	ksft_print_msg("status=%#x core_size=%lld\n", status,
		       (long long)statbuf.st_size);
	ksft_test_result(core_dumped,
			 "an 8K limit permits a 4K-process ELF core\n");

out_directory:
	unlink("core");
	if (cwd_fd >= 0 && fchdir(cwd_fd))
		ksft_print_msg("failed to restore working directory: %s\n",
			       strerror(errno));
	rmdir(temporary);
out_restore:
	if (cwd_fd >= 0)
		close(cwd_fd);
	if (!set_core_pattern(original_pattern, pattern_length))
		ksft_print_msg("failed to restore core_pattern: %s\n",
			       strerror(errno));
	return ksft_get_fail_cnt() ? KSFT_FAIL : KSFT_PASS;
}

int main(int argc, char **argv)
{
	if (access("/proc/self", F_OK) &&
	    mount("proc", "/proc", "proc", 0, NULL)) {
		perror("mount proc");
		return KSFT_FAIL;
	}
	return ppps_compat_main(argc, argv, run_test);
}
