// SPDX-License-Identifier: GPL-2.0
/*
 * A privileged native caller must not operate zram process-range ioctls on a
 * 4K compat target.  The policy is based on the target mm, not current->mm.
 */
#define _GNU_SOURCE

#include <asm/unistd.h>
#include <linux/zram_ioctl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "kselftest_ppps.h"

struct child_info {
	long page_size;
};

static int run_child(int info_fd, int command_fd)
{
	struct child_info info = { .page_size = sysconf(_SC_PAGESIZE) };
	char command;

	if (!write_full(info_fd, &info, sizeof(info)) ||
	    !read_full(command_fd, &command, sizeof(command)))
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}

static int exec_compat_child(int info_fd, int command_fd)
{
	char command_fd_arg[16];
	char info_fd_arg[16];

	snprintf(info_fd_arg, sizeof(info_fd_arg), "%d", info_fd);
	snprintf(command_fd_arg, sizeof(command_fd_arg), "%d", command_fd);
	ppps_execl(true, NULL, "--child", info_fd_arg, command_fd_arg, NULL);
	return EXIT_FAILURE;
}

static int run_parent(const char *device)
{
	struct zram_android_ioc_process_range_writeback request = {};
	struct child_info info;
	int command_pipe[2], info_pipe[2];
	int child_status, pidfd, result, saved_errno, zram_fd;
	char command = 1;
	pid_t child;

	ksft_print_header();
	ksft_set_plan(5);
	ksft_test_result(sysconf(_SC_PAGESIZE) == NATIVE_PAGE_SIZE,
			 "caller uses native 16K pages\n");
	if (pipe(info_pipe) || pipe(command_pipe))
		ksft_exit_fail_msg("pipe failed: %s\n", strerror(errno));
	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork failed: %s\n", strerror(errno));
	if (!child) {
		close(info_pipe[0]);
		close(command_pipe[1]);
		_exit(exec_compat_child(info_pipe[1], command_pipe[0]));
	}
	close(info_pipe[1]);
	close(command_pipe[0]);
	if (!read_full(info_pipe[0], &info, sizeof(info)))
		ksft_exit_fail_msg("failed to read child information\n");
	close(info_pipe[0]);

	ksft_test_result(info.page_size == PROCESS_PAGE_SIZE,
			 "target uses 4K compat pages\n");
	zram_fd = open(device, O_RDWR | O_CLOEXEC);
	ksft_test_result(zram_fd >= 0, "open %s\n", device);
	if (zram_fd < 0)
		ksft_exit_fail_msg("open %s failed: %s\n", device,
				   strerror(errno));
	pidfd = syscall(__NR_pidfd_open, child, 0);
	ksft_test_result(pidfd >= 0, "open target pidfd\n");
	if (pidfd < 0)
		ksft_exit_fail_msg("pidfd_open failed: %s\n", strerror(errno));

	request.pidfd = pidfd;
	request.start_addr = 0;
	request.size = NATIVE_PAGE_SIZE;
	errno = 0;
	result = ioctl(zram_fd, ZRAM_ANDROID_IOC_PROCESS_RANGE_WRITEBACK,
		       &request);
	saved_errno = errno;
	ksft_test_result(result == -1 && saved_errno == EOPNOTSUPP,
			 "native caller rejects compat target mm (%s)\n",
			 result == -1 ? strerror(saved_errno) :
			 "unexpected success");

	close(pidfd);
	close(zram_fd);
	write_full(command_pipe[1], &command, sizeof(command));
	close(command_pipe[1]);
	waitpid(child, &child_status, 0);
	if (!WIFEXITED(child_status) ||
	    WEXITSTATUS(child_status) != EXIT_SUCCESS)
		ksft_exit_fail_msg("compat child failed\n");
	ksft_finished();
}

int main(int argc, char **argv)
{
	ppps_argv0 = argv[0];
	if (argc == 4 && !strcmp(argv[1], "--child"))
		return run_child(atoi(argv[2]), atoi(argv[3]));
	if (argc != 2)
		ksft_exit_fail_msg("usage: %s /dev/block/zramN\n", argv[0]);
	if (sysconf(_SC_PAGESIZE) != NATIVE_PAGE_SIZE)
		exec_native(argv[0], argv[1], NULL);
	return run_parent(argv[1]);
}
