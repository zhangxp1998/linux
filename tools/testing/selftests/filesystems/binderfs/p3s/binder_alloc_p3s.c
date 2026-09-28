// SPDX-License-Identifier: GPL-2.0
/*
 * Check Binder backing-page alignment policy and partial-page tail handling in
 * a 4KB process inside a private binderfs mount.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/android/binder.h>
#include <linux/android/binderfs.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest_p3s.h"
#define P3S_HARNESS_ROLE binder_role
#define P3S_HARNESS_SETUP setup_harness
#include "p3s_harness.h"

#define USER_PAGE_SIZE 4096UL
#define BINDER_VM_SIZE ((1UL * 1024 * 1024) - 2 * USER_PAGE_SIZE)
#define BINDER_VM_HINT ((void *)0x100000000ULL)
#define TEST_CODE 0x50505053U
#define TRANSACTION_PAYLOAD_SIZE (BINDER_VM_SIZE - USER_PAGE_SIZE)

struct server_ready {
	int error;
	uintptr_t mapping;
};

struct transaction_result {
	bool received;
	int active_pages;
	int lru_pages;
};

static unsigned char transaction_payload[TRANSACTION_PAYLOAD_SIZE];

static void init_transaction_payload(void)
{
	size_t i;

	for (i = 0; i < sizeof(transaction_payload); i++)
		transaction_payload[i] = (i * 131 + 17) & 0xff;
}

static unsigned long mapping_kernel_page_size(pid_t pid, uintptr_t address)
{
	char path[64];
	char *line = NULL;
	size_t line_size = 0;
	unsigned long page_size = 0;
	bool in_mapping = false;
	FILE *file;

	if (snprintf(path, sizeof(path), "/proc/%d/smaps", pid) >=
	    (int)sizeof(path))
		return 0;
	file = fopen(path, "re");
	if (!file)
		return 0;
	while (getline(&line, &line_size, file) >= 0) {
		unsigned long start, end, size_kb;

		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in_mapping = address >= start && address < end;
			continue;
		}
		if (in_mapping &&
		    sscanf(line, "KernelPageSize: %lu kB", &size_kb) == 1) {
			page_size = size_kb * 1024;
			break;
		}
	}
	free(line);
	fclose(file);
	return page_size;
}

static int binder_page_slots(const char *binderfs_dir, pid_t pid,
			     int *active_pages, int *lru_pages)
{
	char path[PATH_MAX];
	char *line = NULL;
	size_t line_size = 0;
	int slots = -1;
	bool in_proc = false;
	FILE *file;

	if (snprintf(path, sizeof(path), "%s/binder_logs/stats",
		     binderfs_dir) >= (int)sizeof(path))
		return -1;
	file = fopen(path, "re");
	if (!file) {
		ksft_print_msg("open %s failed: %s\n", path, strerror(errno));
		return -1;
	}
	while (getline(&line, &line_size, file) >= 0) {
		int active, lru, free_pages;
		int current_pid;

		if (sscanf(line, "proc %d", &current_pid) == 1) {
			in_proc = current_pid == pid;
			continue;
		}
		if (!in_proc)
			continue;
		if (sscanf(line, "  pages: %d:%d:%d", &active, &lru, &free_pages) == 3) {
			slots = active + lru + free_pages;
			if (active_pages)
				*active_pages = active;
			if (lru_pages)
				*lru_pages = lru;
			break;
		}
	}
	free(line);
	fclose(file);
	if (slots < 0)
		ksft_print_msg("no Binder page statistics in %s\n", path);
	return slots;
}

static int binder_write_commands(int fd, const void *commands, size_t size)
{
	struct binder_write_read bwr = {
		.write_size = size,
		.write_buffer = (binder_uintptr_t)(uintptr_t)commands,
	};

	return ioctl(fd, BINDER_WRITE_READ, &bwr);
}

static int binder_enter_looper(int fd)
{
	uint32_t command = BC_ENTER_LOOPER;

	return binder_write_commands(fd, &command, sizeof(command));
}

static int binder_free_transaction(int fd, binder_uintptr_t buffer)
{
	unsigned char request[sizeof(uint32_t) + sizeof(buffer)];
	uint32_t command = BC_FREE_BUFFER;

	memcpy(request, &command, sizeof(command));
	memcpy(request + sizeof(command), &buffer, sizeof(buffer));
	return binder_write_commands(fd, request, sizeof(request));
}

static int binder_reply_empty(int fd)
{
	struct binder_transaction_data transaction = {};
	unsigned char commands[sizeof(uint32_t) + sizeof(transaction)];
	uint32_t command = BC_REPLY;

	memcpy(commands, &command, sizeof(command));
	memcpy(commands + sizeof(command), &transaction, sizeof(transaction));
	return binder_write_commands(fd, commands, sizeof(commands));
}

static struct transaction_result read_transaction(int fd,
					  const char *binderfs_dir,
					  bool check_lru)
{
	struct transaction_result result = {
		.active_pages = -1,
		.lru_pages = -1,
	};
	unsigned char read_buffer[4096];
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN,
	};
	unsigned int attempt;

	for (attempt = 0; attempt < 8; attempt++) {
		struct binder_write_read bwr = {
			.read_size = sizeof(read_buffer),
			.read_buffer = (binder_uintptr_t)(uintptr_t)read_buffer,
		};
		size_t offset = 0;

		if (poll(&pfd, 1, 500) <= 0)
			continue;
		if (ioctl(fd, BINDER_WRITE_READ, &bwr))
			return result;
		while (offset + sizeof(uint32_t) <= bwr.read_consumed) {
			struct binder_transaction_data transaction;
			uint32_t command;

			memcpy(&command, read_buffer + offset, sizeof(command));
			offset += sizeof(command);
			switch (command) {
			case BR_TRANSACTION:
				if (offset + sizeof(transaction) > bwr.read_consumed)
					return result;
				memcpy(&transaction, read_buffer + offset,
				       sizeof(transaction));
				if (transaction.code != TEST_CODE ||
				    transaction.data_size != sizeof(transaction_payload) ||
				    memcmp((void *)(uintptr_t)transaction.data.ptr.buffer,
					   transaction_payload,
					   sizeof(transaction_payload)))
					return result;
				if (check_lru)
					binder_page_slots(binderfs_dir, getpid(),
							  &result.active_pages,
							  &result.lru_pages);
				result.received = !binder_free_transaction(fd,
							 transaction.data.ptr.buffer) &&
						  !binder_reply_empty(fd);
				return result;
			case BR_NOOP:
			case BR_TRANSACTION_COMPLETE:
			case BR_SPAWN_LOOPER:
			case BR_FINISHED:
				break;
			case BR_ERROR:
				offset += sizeof(int32_t);
				break;
			default:
				return result;
			}
		}
	}
	return result;
}

static void binder_server(const char *device_path, const char *binderfs_dir,
			  int ready_fd, int result_fd)
{
	struct server_ready ready = {};
	struct transaction_result result = {
		.active_pages = -1,
		.lru_pages = -1,
	};
	void *mapping = MAP_FAILED;
	int manager = 0;
	int fd;

	fd = open(device_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ready.error = errno;
		goto report_ready;
	}
	mapping = mmap(BINDER_VM_HINT, BINDER_VM_SIZE, PROT_READ, MAP_SHARED,
		       fd, 0);
	if (mapping == MAP_FAILED) {
		ready.error = errno;
		goto report_ready;
	}
	ready.mapping = (uintptr_t)mapping;
	if (ioctl(fd, BINDER_SET_CONTEXT_MGR, &manager) ||
	    binder_enter_looper(fd)) {
		ready.error = errno;
		goto report_ready;
	}

report_ready:
	write_full(ready_fd, &ready, sizeof(ready));
	close(ready_fd);
	for (unsigned int round = 0; !ready.error && round < 2; round++) {
		result = read_transaction(fd, binderfs_dir, round == 1);
		write_full(result_fd, &result, sizeof(result));
		if (!result.received)
			break;
	}
	close(result_fd);
	if (mapping != MAP_FAILED)
		munmap(mapping, BINDER_VM_SIZE);
	if (fd >= 0)
		close(fd);
	_exit(result.received ? EXIT_SUCCESS : EXIT_FAILURE);
}

static int send_transaction(const char *device_path, void **mapping_out)
{
	struct binder_transaction_data transaction = {
		.target.handle = 0,
		.code = TEST_CODE,
		.data_size = sizeof(transaction_payload),
		.data.ptr.buffer = (binder_uintptr_t)(uintptr_t)transaction_payload,
	};
	unsigned char commands[sizeof(uint32_t) + sizeof(transaction)];
	uint32_t command = BC_TRANSACTION;
	int fd;
	int ret;
	void *mapping;

	memcpy(commands, &command, sizeof(command));
	memcpy(commands + sizeof(command), &transaction, sizeof(transaction));
	fd = open(device_path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	mapping = mmap(NULL, BINDER_VM_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		close(fd);
		return -1;
	}
	ret = binder_write_commands(fd, commands, sizeof(commands));
	if (ret) {
		munmap(mapping, BINDER_VM_SIZE);
		close(fd);
		return -1;
	}
	*mapping_out = mapping;
	return fd;
}

static bool wait_readable(int fd, int timeout_ms)
{
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN,
	};

	return poll(&pfd, 1, timeout_ms) > 0;
}

static const char *binderfs_dir;

static bool create_device(char *device_path, size_t size, bool *created)
{
	char control_path[PATH_MAX];
	struct binderfs_device device = {};
	int fd;

	if (snprintf(control_path, sizeof(control_path), "%s/binder-control",
		     binderfs_dir) >= sizeof(control_path))
		return false;
	if (snprintf(device.name, sizeof(device.name), "p3s-binder-%d",
		     getpid()) >= sizeof(device.name))
		return false;
	if (snprintf(device_path, size, "%s/%s", binderfs_dir,
		     device.name) >= size)
		return false;
	fd = open(control_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	if (ioctl(fd, BINDER_CTL_ADD, &device)) {
		close(fd);
		return false;
	}
	close(fd);
	*created = true;
	return !access(device_path, R_OK | W_OK);
}

static bool mapping_policy(const char *device, void *hint,
			   unsigned long offset, bool reject)
{
	int fd = open(device, O_RDWR | O_CLOEXEC);
	void *mapping;
	int error;
	bool ok;

	if (fd < 0)
		return false;
	errno = 0;
	mapping = mmap(hint, BINDER_VM_SIZE, PROT_READ, MAP_SHARED, fd, offset);
	error = errno;
	ok = reject ? mapping == MAP_FAILED && error == EINVAL :
		mapping != MAP_FAILED &&
		!((uintptr_t)mapping & (NATIVE_PAGE_SIZE - 1));
	if (mapping != MAP_FAILED)
		munmap(mapping, BINDER_VM_SIZE);
	close(fd);
	return ok;
}

FIXTURE(binder_policy) {
	char device_path[PATH_MAX];
	bool created;
};

FIXTURE_VARIANT(binder_policy) {
	void *hint;
	unsigned long offset;
	bool reject;
};

FIXTURE_VARIANT_ADD(binder_policy, unaligned_hint) {
	.hint = BINDER_VM_HINT + USER_PAGE_SIZE, .reject = true,
};
FIXTURE_VARIANT_ADD(binder_policy, offset_4k) {
	.offset = USER_PAGE_SIZE, .reject = true,
};
FIXTURE_VARIANT_ADD(binder_policy, offset_12k) {
	.offset = 3 * USER_PAGE_SIZE, .reject = true,
};
FIXTURE_VARIANT_ADD(binder_policy, offset_16k) {
	.offset = NATIVE_PAGE_SIZE,
};

FIXTURE_SETUP(binder_policy)
{
}

FIXTURE_TEARDOWN_PARENT(binder_policy)
{
	if (self->created)
		EXPECT_EQ(0, unlink(self->device_path));
}

TEST_F(binder_policy, mapping_obeys_native_alignment)
{
	ASSERT_TRUE(create_device(self->device_path,
				  sizeof(self->device_path), &self->created));
	EXPECT_TRUE(mapping_policy(self->device_path, variant->hint,
				   variant->offset, variant->reject));
}

struct tail_result {
	struct server_ready ready;
	struct transaction_result first;
	struct transaction_result second;
	unsigned long kernel_page_size;
	unsigned long expected_slots;
	int page_slots;
};

static struct transaction_result send_and_receive(const char *device_path,
						   int result_fd)
{
	struct transaction_result result = {
		.active_pages = -1,
		.lru_pages = -1,
	};
	void *sender_mapping = NULL;
	int sender_fd = send_transaction(device_path, &sender_mapping);

	if (sender_fd < 0)
		return result;
	if (wait_readable(result_fd, 5000))
		read_full(result_fd, &result, sizeof(result));
	munmap(sender_mapping, BINDER_VM_SIZE);
	close(sender_fd);
	return result;
}

static struct tail_result run_tail(const char *device_path,
				  unsigned int rounds)
{
	struct tail_result result = {
		.first.active_pages = -1,
		.first.lru_pages = -1,
		.second.active_pages = -1,
		.second.lru_pages = -1,
		.page_slots = -1,
	};
	int ready_pipe[2], result_pipe[2], status;
	pid_t server;

	if (pipe(ready_pipe))
		return result;
	if (pipe(result_pipe)) {
		close(ready_pipe[0]);
		close(ready_pipe[1]);
		return result;
	}
	server = fork();
	if (server < 0) {
		close(ready_pipe[0]);
		close(ready_pipe[1]);
		close(result_pipe[0]);
		close(result_pipe[1]);
		return result;
	}
	if (!server) {
		close(ready_pipe[0]);
		close(result_pipe[0]);
		binder_server(device_path, binderfs_dir,
			      ready_pipe[1], result_pipe[1]);
	}
	close(ready_pipe[1]);
	close(result_pipe[1]);
	if (wait_readable(ready_pipe[0], 2000))
		read_full(ready_pipe[0], &result.ready, sizeof(result.ready));
	close(ready_pipe[0]);
	if (!result.ready.error && result.ready.mapping) {
		result.kernel_page_size =
			mapping_kernel_page_size(server, result.ready.mapping);
		result.page_slots = binder_page_slots(binderfs_dir, server,
						      NULL, NULL);
		if (result.kernel_page_size)
			result.expected_slots =
				(BINDER_VM_SIZE + result.kernel_page_size - 1) /
				result.kernel_page_size;
		if (rounds)
			result.first = send_and_receive(device_path,
							result_pipe[0]);
		if (rounds > 1 && result.first.received)
			result.second = send_and_receive(device_path,
							 result_pipe[0]);
	}
	close(result_pipe[0]);
	if (rounds < 2 || !result.second.received)
		kill(server, SIGKILL);
	waitpid(server, &status, 0);
	return result;
}

FIXTURE(binder_tail) {
	char device_path[PATH_MAX];
	bool created;
};

FIXTURE_SETUP(binder_tail)
{
}

FIXTURE_TEARDOWN_PARENT(binder_tail)
{
	if (self->created)
		EXPECT_EQ(0, unlink(self->device_path));
}

static struct tail_result tail_case(struct _test_data_binder_tail *self,
				    unsigned int rounds)
{
	if (!create_device(self->device_path, sizeof(self->device_path),
			   &self->created))
		return (struct tail_result) {};
	return run_tail(self->device_path, rounds);
}

TEST_F(binder_tail, maps_at_aligned_hint_with_partial_tail)
{
	struct tail_result result;

	result = tail_case(self, 0);
	EXPECT_TRUE(!result.ready.error &&
		    result.ready.mapping == (uintptr_t)BINDER_VM_HINT &&
		    !(result.ready.mapping & (NATIVE_PAGE_SIZE - 1)));
}

TEST_F(binder_tail, tracks_partial_backing_page)
{
	struct tail_result result;

	result = tail_case(self, 0);
	ASSERT_TRUE(result.ready.mapping && !result.ready.error);
	if (result.page_slots < 0)
		SKIP(return, "Binder page statistics unavailable");
	ASSERT_GT(result.kernel_page_size, 0);
	EXPECT_EQ(result.expected_slots, result.page_slots);
}

TEST_F(binder_tail, delivers_large_transaction)
{
	struct tail_result result;

	result = tail_case(self, 1);
	EXPECT_TRUE(result.first.received);
}

TEST_F(binder_tail, repeats_transaction_in_same_mapping)
{
	struct tail_result result;

	result = tail_case(self, 2);
	ASSERT_TRUE(result.first.received);
	EXPECT_TRUE(result.second.received);
}

TEST_F(binder_tail, no_in_use_tail_page_remains_on_lru)
{
	struct tail_result result;

	result = tail_case(self, 2);
	ASSERT_TRUE(result.second.received);
	if (result.second.lru_pages < 0)
		SKIP(return, "Binder page statistics unavailable");
	EXPECT_EQ(result.expected_slots, result.second.active_pages);
	EXPECT_EQ(0, result.second.lru_pages);
}

static int run_in_private_binderfs(int argc, char **argv)
{
	const char *tmpdir = getenv("TMPDIR");
	char mountpoint[PATH_MAX];
	int status;
	pid_t child;
	int i;

	if (!tmpdir || !tmpdir[0])
		tmpdir = "/tmp";
	if (snprintf(mountpoint, sizeof(mountpoint),
		     "%s/binder-alloc-p3s.XXXXXX", tmpdir) >=
	    (int)sizeof(mountpoint))
		ksft_exit_fail_msg("binderfs mountpoint path too long\n");
	if (unshare(CLONE_NEWNS) ||
	    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL))
		ksft_exit_skip("private mount namespace unavailable: %s\n",
			       strerror(errno));
	if (!mkdtemp(mountpoint))
		ksft_exit_fail_msg("create binderfs mountpoint failed: %s\n",
				   strerror(errno));
	if (mount(NULL, mountpoint, "binder", 0, "stats=global")) {
		int saved_errno = errno;

		rmdir(mountpoint);
		if (saved_errno == ENODEV)
			ksft_exit_skip("binderfs is unavailable\n");
		errno = saved_errno;
		ksft_exit_fail_msg("mount binderfs failed: %s\n",
				   strerror(errno));
	}

	child = fork();
	if (child < 0)
		ksft_exit_fail_msg("fork 4KB process failed: %s\n",
				   strerror(errno));
	if (!child) {
		char *next_argv[argc + 2];

		next_argv[0] = argv[0];
		next_argv[1] = "-f";
		next_argv[2] = !strcmp(argv[1], "policy") ? "binder_policy" :
			       "binder_tail";
		for (i = 2; i < argc; i++)
			next_argv[i + 1] = argv[i];
		next_argv[argc + 1] = NULL;
		if (setenv("P3S_BINDERFS_DIR", mountpoint, 1))
			_exit(KSFT_FAIL);
		p3s_execv(true, next_argv);
		_exit(KSFT_FAIL);
	}
	if (waitpid(child, &status, 0) < 0)
		ksft_exit_fail_msg("wait for 4KB process failed: %s\n",
				   strerror(errno));
	if (umount2(mountpoint, MNT_DETACH) || rmdir(mountpoint))
		ksft_exit_fail_msg("clean up binderfs failed: %s\n",
				   strerror(errno));
	if (!WIFEXITED(status))
		return EXIT_FAILURE;
	return WEXITSTATUS(status);
}

static int binder_role(int argc, char **argv)
{
	if (argc < 2 || (strcmp(argv[1], "policy") &&
			 strcmp(argv[1], "tail")))
		return -1;
	p3s_harness_require_native_4kb();
	return run_in_private_binderfs(argc, argv);
}

static void setup_harness(int *argc, char ***argv)
{
	binderfs_dir = getenv("P3S_BINDERFS_DIR");
	if (!binderfs_dir && p3s_harness_listing(*argc, *argv))
		return;
	if (!binderfs_dir)
		ksft_exit_fail_msg("run with policy or tail fixture\n");
	init_transaction_payload();
}

P3S_HARNESS_4KB_MAIN
