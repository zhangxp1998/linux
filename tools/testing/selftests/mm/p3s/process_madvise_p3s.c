// SPDX-License-Identifier: GPL-2.0
/*
 * process_madvise() from a native (16K) caller on a 4K compat target accepts
 * a start that is only 4KB-aligned in the target. WILLNEED on anonymous
 * memory checks acceptance of that address, not rounding or residency.
 */
#define _GNU_SOURCE

#include <asm/unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>

#define P3S_HARNESS_ROLE child_role
#define P3S_HARNESS_DEFAULT_NATIVE 1
#include "p3s_harness.h"

#define MAPPING_SIZE	(16 * PROCESS_PAGE_SIZE)

struct child_info {
	uintptr_t address;
	long page_size;
};

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

static int exec_compat_child(int info_fd, int done_fd)
{
	char done_fd_arg[32];
	char info_fd_arg[32];

	snprintf(info_fd_arg, sizeof(info_fd_arg), "%d", info_fd);
	snprintf(done_fd_arg, sizeof(done_fd_arg), "%d", done_fd);
	p3s_execl(true, NULL, "--child", info_fd_arg, done_fd_arg, NULL);
	perror("exec child");
	return EXIT_FAILURE;
}

static int run_child(int info_fd, int done_fd)
{
	unsigned char *large_page_aligned;
	unsigned char *mapping;
	struct child_info info;
	char done;

	info.page_size = sysconf(_SC_PAGESIZE);
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		perror("child mmap");
		return EXIT_FAILURE;
	}
	large_page_aligned = (unsigned char *)align_up((uintptr_t)mapping,
							NATIVE_PAGE_SIZE);
	if (large_page_aligned + 2 * PROCESS_PAGE_SIZE > mapping + MAPPING_SIZE)
		return EXIT_FAILURE;
	info.address = (uintptr_t)(large_page_aligned + PROCESS_PAGE_SIZE);

	if (!write_full(info_fd, &info, sizeof(info)))
		return EXIT_FAILURE;
	if (!read_full(done_fd, &done, sizeof(done)))
		return EXIT_FAILURE;
	munmap(mapping, MAPPING_SIZE);
	return EXIT_SUCCESS;
}

TEST(native_process_madvise_accepts_target_4kb_start)
{
	struct child_info info = {};
	struct iovec iov;
	ssize_t advised;
	int done_pipe[2] = { -1, -1 };
	int info_pipe[2] = { -1, -1 };
	int child_status = 0;
	int pidfd = -1;
	char done = 1;
	pid_t child = -1;
	bool ready = false, permission_denied = false;

	if (pipe(info_pipe) || pipe(done_pipe)) {
		TH_LOG("pipe failed: %s", strerror(errno));
		EXPECT_TRUE(false);
		goto out;
	}
	child = fork();
	if (child < 0) {
		TH_LOG("fork failed: %s", strerror(errno));
		EXPECT_TRUE(false);
		goto out;
	}
	if (!child) {
		close(info_pipe[0]);
		close(done_pipe[1]);
		_exit(exec_compat_child(info_pipe[1], done_pipe[0]));
	}
	close(info_pipe[1]);
	info_pipe[1] = -1;
	close(done_pipe[0]);
	done_pipe[0] = -1;
	ready = read_full(info_pipe[0], &info, sizeof(info));
	close(info_pipe[0]);
	info_pipe[0] = -1;
	EXPECT_TRUE(ready);
	if (!ready)
		goto out;
	TH_LOG("caller page size=%ld, target page size=%ld, address=%#lx",
	       sysconf(_SC_PAGESIZE), info.page_size,
	       (unsigned long)info.address);
	EXPECT_EQ(PROCESS_PAGE_SIZE, info.page_size);
	EXPECT_EQ(0, info.address & (PROCESS_PAGE_SIZE - 1));
	EXPECT_NE(0, info.address & (NATIVE_PAGE_SIZE - 1));
	pidfd = syscall(__NR_pidfd_open, child, 0);
	EXPECT_GE(pidfd, 0);
	if (pidfd < 0)
		goto out;
	iov.iov_base = (void *)info.address;
	iov.iov_len = PROCESS_PAGE_SIZE;
	errno = 0;
	advised = syscall(__NR_process_madvise, pidfd, &iov, 1,
			  MADV_WILLNEED, 0);
	if (advised == -1 && errno == EPERM) {
		permission_denied = true;
		goto out;
	}
	if (advised != PROCESS_PAGE_SIZE)
		TH_LOG("process_madvise returned %zd: %s", advised,
		       advised < 0 ? strerror(errno) : "short operation");
	EXPECT_EQ(PROCESS_PAGE_SIZE, advised);

out:
	if (pidfd >= 0)
		close(pidfd);
	if (info_pipe[0] >= 0)
		close(info_pipe[0]);
	if (info_pipe[1] >= 0)
		close(info_pipe[1]);
	if (done_pipe[0] >= 0)
		close(done_pipe[0]);
	if (done_pipe[1] >= 0) {
		if (child > 0 && !write_full(done_pipe[1], &done, sizeof(done)))
			TH_LOG("failed to release child: %s", strerror(errno));
		close(done_pipe[1]);
	}
	if (child > 0) {
		EXPECT_EQ(child, waitpid(child, &child_status, 0));
		EXPECT_TRUE(WIFEXITED(child_status));
		if (WIFEXITED(child_status))
			EXPECT_EQ(EXIT_SUCCESS, WEXITSTATUS(child_status));
	}
	if (permission_denied)
		SKIP(return, "process_madvise needs CAP_SYS_NICE");
}

static int child_role(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "--child"))
		return run_child(atoi(argv[2]), atoi(argv[3]));
	return -1;
}

P3S_HARNESS_4KB_MAIN
