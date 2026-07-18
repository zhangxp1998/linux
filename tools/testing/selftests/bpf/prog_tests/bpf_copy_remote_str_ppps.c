// SPDX-License-Identifier: GPL-2.0
/*
 * A native (16K) BPF loader copies strings that straddle a 4K slice boundary
 * out of a 4K compat target's anonymous and file-backed mappings through the
 * remote task string-copy helpers; both copies must see the whole string.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <test_progs.h>

#include "kselftest_ppps.h"
#include "bpf_copy_remote_str_ppps.skel.h"

#define FILE_OFFSET (3 * PROCESS_PAGE_SIZE)
#define FILE_SIZE (4 * NATIVE_PAGE_SIZE)
#define OUTPUT_SIZE 32
#define ANON_ADDRESS ((void *)0x40000000UL)
#define FILE_ADDRESS ((void *)0x50000000UL)
#define MODE_ENV "BPF_COPY_REMOTE_STR_PPPS_MODE"
#define INFO_FD_ENV "BPF_COPY_REMOTE_STR_PPPS_INFO_FD"
#define FINISH_FD_ENV "BPF_COPY_REMOTE_STR_PPPS_FINISH_FD"

static const char anon_expected[] = "abcdEFGHIJKLMNO";
static const char file_expected[] = "WXYZabcdefghijk";

struct child_info {
	uintptr_t anon_ptr;
	uintptr_t file_ptr;
	long page_size;
};

struct bpf_copy_state {
	uint32_t target_pid;
	uint32_t seen;
	uint64_t anon_ptr;
	uint64_t file_ptr;
	int32_t anon_ret;
	int32_t file_ret;
	char anon_output[OUTPUT_SIZE];
	char file_output[OUTPUT_SIZE];
};

static int run_target(int info_fd, int finish_fd)
{
	unsigned char file_data[FILE_SIZE];
	struct child_info info;
	char *anon_mapping;
	char *file_mapping;
	char finish;
	int memfd;

	anon_mapping = mmap(ANON_ADDRESS, 2 * PROCESS_PAGE_SIZE,
			    PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			    -1, 0);
	if (anon_mapping == MAP_FAILED)
		return EXIT_FAILURE;
	memset(anon_mapping, 'A', 2 * PROCESS_PAGE_SIZE);
	memcpy(anon_mapping + PROCESS_PAGE_SIZE - 4, anon_expected,
	       sizeof(anon_expected));

	memfd = memfd_create("bpf-copy-remote-str-ppps", 0);
	if (memfd < 0 || ftruncate(memfd, FILE_SIZE))
		return EXIT_FAILURE;
	memset(file_data, 'Q', sizeof(file_data));
	memcpy(file_data + FILE_OFFSET + PROCESS_PAGE_SIZE - 4, file_expected,
	       sizeof(file_expected));
	if (!pwrite_full(memfd, file_data, sizeof(file_data), 0))
		return EXIT_FAILURE;
	file_mapping = mmap(FILE_ADDRESS, 2 * PROCESS_PAGE_SIZE,
			    PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_FIXED_NOREPLACE, memfd,
			    FILE_OFFSET);
	if (file_mapping == MAP_FAILED)
		return EXIT_FAILURE;

	info.anon_ptr = (uintptr_t)(anon_mapping + PROCESS_PAGE_SIZE - 4);
	info.file_ptr = (uintptr_t)(file_mapping + PROCESS_PAGE_SIZE - 4);
	info.page_size = sysconf(_SC_PAGESIZE);
	if (!write_full(info_fd, &info, sizeof(info)) ||
	    !read_full(finish_fd, &finish, sizeof(finish)))
		return EXIT_FAILURE;

	munmap(file_mapping, 2 * PROCESS_PAGE_SIZE);
	close(memfd);
	munmap(anon_mapping, 2 * PROCESS_PAGE_SIZE);
	return EXIT_SUCCESS;
}

/*
 * Re-exec test_progs restricted to this test in @mode, as a compat or native
 * process.  Only ever called in a forked child, which fails quietly.
 */
static void exec_mode(const char *mode, bool compat, int info_fd, int finish_fd)
{
	char info_fd_string[16];
	char finish_fd_string[16];

	if (setenv(MODE_ENV, mode, 1))
		_exit(EXIT_FAILURE);
	if (info_fd >= 0) {
		snprintf(info_fd_string, sizeof(info_fd_string), "%d", info_fd);
		snprintf(finish_fd_string, sizeof(finish_fd_string), "%d",
			 finish_fd);
		if (setenv(INFO_FD_ENV, info_fd_string, 1) ||
		    setenv(FINISH_FD_ENV, finish_fd_string, 1))
			_exit(EXIT_FAILURE);
	}
	ppps_execl(compat, "test_progs", "-t", "bpf_copy_remote_str_ppps",
		   NULL);
	_exit(EXIT_FAILURE);
}

static int trigger_iterator(struct bpf_program *program)
{
	struct bpf_link *link;
	char buffer[64];
	int iterator_fd;
	ssize_t bytes;
	int error = -1;

	link = bpf_program__attach_iter(program, NULL);
	if (!ASSERT_OK_PTR(link, "attach iterator"))
		return -1;
	iterator_fd = bpf_iter_create(bpf_link__fd(link));
	if (!ASSERT_GE(iterator_fd, 0, "create iterator"))
		goto out;
	do {
		bytes = read(iterator_fd, buffer, sizeof(buffer));
	} while (bytes > 0 || (bytes < 0 && errno == EINTR));
	error = ASSERT_EQ(bytes, 0, "read iterator") ? 0 : -1;
	close(iterator_fd);
out:
	bpf_link__destroy(link);
	return error;
}

static void run_loader(void)
{
	struct bpf_copy_remote_str_ppps *skeleton = NULL;
	struct bpf_copy_state state = {};
	struct child_info info;
	uint32_t map_key = 0;
	int info_pipe[2];
	int finish_pipe[2];
	int child_status;
	int error;
	char finish = 1;
	pid_t child;

	if (!ASSERT_OK(pipe(info_pipe), "create info pipe") ||
	    !ASSERT_OK(pipe(finish_pipe), "create finish pipe"))
		return;
	child = fork();
	if (!ASSERT_GE(child, 0, "fork target"))
		return;
	if (!child) {
		close(info_pipe[0]);
		close(finish_pipe[1]);
		exec_mode("target", true, info_pipe[1], finish_pipe[0]);
	}

	close(info_pipe[1]);
	close(finish_pipe[0]);
	if (!ASSERT_TRUE(read_full(info_pipe[0], &info, sizeof(info)),
			 "read target info"))
		goto release_child;
	ASSERT_EQ(info.page_size, PROCESS_PAGE_SIZE, "target page size");

	skeleton = bpf_copy_remote_str_ppps__open_and_load();
	if (!ASSERT_OK_PTR(skeleton, "open and load BPF skeleton"))
		goto release_child;
	state.target_pid = child;
	state.anon_ptr = info.anon_ptr;
	state.file_ptr = info.file_ptr;
	error = bpf_map_update_elem(bpf_map__fd(skeleton->maps.state_map),
				    &map_key, &state, BPF_ANY);
	if (!ASSERT_OK(error, "initialize BPF state"))
		goto release_child;
	if (trigger_iterator(skeleton->progs.copy_remote_strings))
		goto release_child;
	error = bpf_map_lookup_elem(bpf_map__fd(skeleton->maps.state_map),
				    &map_key, &state);
	if (!ASSERT_OK(error, "read BPF state"))
		goto release_child;

	ASSERT_EQ(state.seen, 1, "iterator saw target");
	ASSERT_EQ(state.anon_ret, (int)sizeof(anon_expected), "anonymous return");
	ASSERT_STREQ(state.anon_output, anon_expected, "anonymous contents");
	ASSERT_EQ(state.file_ret, (int)sizeof(file_expected), "file return");
	ASSERT_STREQ(state.file_output, file_expected, "file contents");

release_child:
	bpf_copy_remote_str_ppps__destroy(skeleton);
	write_full(finish_pipe[1], &finish, sizeof(finish));
	close(finish_pipe[1]);
	close(info_pipe[0]);
	if (!ASSERT_EQ(waitpid(child, &child_status, 0), child, "wait target"))
		return;
	ASSERT_TRUE(WIFEXITED(child_status) && !WEXITSTATUS(child_status),
		    "target exited successfully");
}

void test_bpf_copy_remote_str_ppps(void)
{
	const char *mode = ppps_run_mode(0, NULL, MODE_ENV);
	int status;
	pid_t child;

	if (mode && !strcmp(mode, "target"))
		_exit(run_target(atoi(getenv(INFO_FD_ENV)),
				 atoi(getenv(FINISH_FD_ENV))));
	if (mode && !strcmp(mode, "loader")) {
		run_loader();
		return;
	}

	child = fork();
	if (!ASSERT_GE(child, 0, "fork native loader"))
		return;
	if (!child)
		exec_mode("loader", false, -1, -1);
	if (!ASSERT_EQ(waitpid(child, &status, 0), child, "wait native loader"))
		return;
	ASSERT_TRUE(WIFEXITED(status) && !WEXITSTATUS(status),
		    "native loader passed");
}
