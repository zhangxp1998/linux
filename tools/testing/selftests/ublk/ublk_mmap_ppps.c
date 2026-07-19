// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <linux/ublk_cmd.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define NATIVE_TEST_PAGE_SIZE 16384UL
#define USER_DATA 0x75626c6b2d707070ULL
#define IO_USER_DATA 0x75626c6b2d696f00ULL
#define SQE_SIZE_128 128UL
#define SHARED_MAP_ADDR ((void *)0x40001000UL)
#define FALLBACK_MAP_ADDR ((void *)0x50000000UL)
#define CHILD_STACK_SIZE (64 * 1024UL)

struct async_write {
	void *buffer;
	int fd;
};

static int direct_write_child(void *data)
{
	struct async_write *write = data;
	ssize_t written;

	written = pwrite(write->fd, write->buffer, USER_PAGE_SIZE, 0);
	return written == USER_PAGE_SIZE ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct ring_mapping {
	void *sq_ring;
	void *cq_ring;
	void *sqes;
	size_t sq_ring_size;
	size_t cq_ring_size;
	size_t sqes_size;
	bool single_mmap;
};

static int setup_ring(unsigned int entries, struct io_uring_params *params)
{
	return syscall(__NR_io_uring_setup, entries, params);
}

static int enter_ring(int fd, unsigned int submit, unsigned int wait_nr,
		      unsigned int flags)
{
	return syscall(__NR_io_uring_enter, fd, submit, wait_nr, flags, NULL, 0);
}

static void *mmap_ring_region(int fd, size_t size, off_t offset)
{
	return mmap(NULL, size, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_POPULATE, fd, offset);
}

static void unmap_ring(struct ring_mapping *map)
{
	if (map->sqes != MAP_FAILED)
		munmap(map->sqes, map->sqes_size);
	if (map->cq_ring != MAP_FAILED && !map->single_mmap)
		munmap(map->cq_ring, map->cq_ring_size);
	if (map->sq_ring != MAP_FAILED)
		munmap(map->sq_ring, map->sq_ring_size);
}

static bool map_ring(int fd, const struct io_uring_params *params,
		     struct ring_mapping *map)
{
	size_t shared_size;

	map->sq_ring = MAP_FAILED;
	map->cq_ring = MAP_FAILED;
	map->sqes = MAP_FAILED;
	map->sq_ring_size = params->sq_off.array +
		params->sq_entries * sizeof(unsigned int);
	map->cq_ring_size = params->cq_off.cqes +
		params->cq_entries * sizeof(struct io_uring_cqe);
	map->sqes_size = params->sq_entries * SQE_SIZE_128;
	map->single_mmap = params->features & IORING_FEAT_SINGLE_MMAP;
	if (map->single_mmap) {
		shared_size = map->sq_ring_size > map->cq_ring_size ?
			map->sq_ring_size : map->cq_ring_size;
		map->sq_ring_size = shared_size;
		map->cq_ring_size = shared_size;
	}

	map->sq_ring = mmap_ring_region(fd, map->sq_ring_size,
					IORING_OFF_SQ_RING);
	if (map->sq_ring == MAP_FAILED)
		return false;
	if (map->single_mmap) {
		map->cq_ring = map->sq_ring;
	} else {
		map->cq_ring = mmap_ring_region(fd, map->cq_ring_size,
						IORING_OFF_CQ_RING);
		if (map->cq_ring == MAP_FAILED)
			return false;
	}
	map->sqes = mmap_ring_region(fd, map->sqes_size, IORING_OFF_SQES);
	return map->sqes != MAP_FAILED;
}

static int submit_ctrl_cmd(int ring_fd, const struct io_uring_params *params,
			   struct ring_mapping *map, int ctrl_fd,
			   unsigned int cmd_op,
			   struct ublksrv_ctrl_dev_info *info,
			   void *payload, size_t payload_len,
			   uint64_t data)
{
	unsigned int *sq_head = map->sq_ring + params->sq_off.head;
	unsigned int *sq_tail = map->sq_ring + params->sq_off.tail;
	unsigned int *sq_mask = map->sq_ring + params->sq_off.ring_mask;
	unsigned int *sq_array = map->sq_ring + params->sq_off.array;
	unsigned int *cq_head = map->cq_ring + params->cq_off.head;
	unsigned int *cq_tail = map->cq_ring + params->cq_off.tail;
	unsigned int *cq_mask = map->cq_ring + params->cq_off.ring_mask;
	struct io_uring_cqe *cqes = map->cq_ring + params->cq_off.cqes;
	struct ublksrv_ctrl_cmd *cmd;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	unsigned int index;
	unsigned int tail;
	int result;

	if (*sq_tail - *sq_head >= params->sq_entries)
		return -EBUSY;
	tail = *sq_tail;
	index = tail & *sq_mask;
	sqe = map->sqes + index * SQE_SIZE_128;
	memset(sqe, 0, SQE_SIZE_128);
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->fd = ctrl_fd;
	sqe->cmd_op = cmd_op;
	sqe->user_data = USER_DATA;
	cmd = (struct ublksrv_ctrl_cmd *)&sqe->cmd;
	cmd->dev_id = info->dev_id;
	cmd->queue_id = (uint16_t)-1;
	if (payload) {
		cmd->addr = (uintptr_t)payload;
		cmd->len = payload_len;
	}
	cmd->data[0] = data;
	sq_array[index] = index;
	__atomic_store_n(sq_tail, tail + 1, __ATOMIC_RELEASE);
	if (enter_ring(ring_fd, 1, 1, IORING_ENTER_GETEVENTS) != 1)
		return -errno;
	if (__atomic_load_n(cq_tail, __ATOMIC_ACQUIRE) == *cq_head)
		return -EIO;
	cqe = &cqes[*cq_head & *cq_mask];
	result = cqe->user_data == USER_DATA ? cqe->res : -EIO;
	__atomic_store_n(cq_head, *cq_head + 1, __ATOMIC_RELEASE);
	return result;
}

static int submit_io_cmd(int ring_fd, const struct io_uring_params *params,
			 struct ring_mapping *map, int char_fd,
			 unsigned int cmd_op, unsigned int queue_id,
			 int result, void *addr)
{
	unsigned int *sq_head = map->sq_ring + params->sq_off.head;
	unsigned int *sq_tail = map->sq_ring + params->sq_off.tail;
	unsigned int *sq_mask = map->sq_ring + params->sq_off.ring_mask;
	unsigned int *sq_array = map->sq_ring + params->sq_off.array;
	struct ublksrv_io_cmd *cmd;
	struct io_uring_sqe *sqe;
	unsigned int index;
	unsigned int tail;

	if (*sq_tail - *sq_head >= params->sq_entries)
		return -EBUSY;
	tail = *sq_tail;
	index = tail & *sq_mask;
	sqe = map->sqes + index * SQE_SIZE_128;
	memset(sqe, 0, SQE_SIZE_128);
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->fd = char_fd;
	sqe->cmd_op = cmd_op;
	sqe->user_data = IO_USER_DATA + queue_id;
	cmd = (struct ublksrv_io_cmd *)&sqe->cmd;
	cmd->q_id = queue_id;
	cmd->tag = 0;
	cmd->result = result;
	cmd->addr = (uintptr_t)addr;
	sq_array[index] = index;
	__atomic_store_n(sq_tail, tail + 1, __ATOMIC_RELEASE);
	return enter_ring(ring_fd, 1, 0, 0) == 1 ? 0 : -errno;
}

static int reap_io_cqe(const struct io_uring_params *params,
			struct ring_mapping *map, unsigned int *queue_id)
{
	unsigned int *cq_head = map->cq_ring + params->cq_off.head;
	unsigned int *cq_tail = map->cq_ring + params->cq_off.tail;
	unsigned int *cq_mask = map->cq_ring + params->cq_off.ring_mask;
	struct io_uring_cqe *cqes = map->cq_ring + params->cq_off.cqes;
	struct io_uring_cqe *cqe;
	uint64_t user_data;
	int result;

	if (__atomic_load_n(cq_tail, __ATOMIC_ACQUIRE) == *cq_head)
		return -EAGAIN;
	cqe = &cqes[*cq_head & *cq_mask];
	user_data = cqe->user_data;
	result = cqe->res;
	__atomic_store_n(cq_head, *cq_head + 1, __ATOMIC_RELEASE);
	if (user_data < IO_USER_DATA || user_data >= IO_USER_DATA + 2)
		return -EIO;
	*queue_id = user_data - IO_USER_DATA;
	return result;
}

static int open_ublk_char(unsigned int dev_id)
{
	char line[128];
	char path[64];
	char name[64];
	unsigned int major;
	FILE *devices;
	int retries;
	int fd;

	snprintf(path, sizeof(path), "/dev/ublkc%u", dev_id);
	devices = fopen("/proc/devices", "r");
	if (devices) {
		while (fgets(line, sizeof(line), devices)) {
			if (sscanf(line, "%u %63s", &major, name) == 2 &&
			    !strcmp(name, "ublk-char")) {
				mknod(path, S_IFCHR | 0600, makedev(major, dev_id));
				break;
			}
		}
		fclose(devices);
	}
	for (retries = 0; retries < 50; retries++) {
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0)
			return fd;
		usleep(10000);
	}
	ksft_print_msg("open %s failed: %s\n", path, strerror(errno));
	return -1;
}

static int open_ublk_block(unsigned int dev_id)
{
	char sysfs_path[64];
	char dev_path[64];
	unsigned int major;
	unsigned int minor;
	FILE *dev_file;
	int retries;
	int fd;

	snprintf(sysfs_path, sizeof(sysfs_path), "/sys/block/ublkb%u/dev",
		 dev_id);
	snprintf(dev_path, sizeof(dev_path), "/dev/ublkb%u", dev_id);
	for (retries = 0; retries < 100; retries++) {
		dev_file = fopen(sysfs_path, "r");
		if (dev_file) {
			if (fscanf(dev_file, "%u:%u", &major, &minor) == 2)
				mknod(dev_path, S_IFBLK | 0600,
				      makedev(major, minor));
			fclose(dev_file);
		}
		fd = open(dev_path, O_RDWR | O_DIRECT | O_CLOEXEC);
		if (fd >= 0)
			return fd;
		usleep(10000);
	}
	ksft_print_msg("open %s failed: %s\n", dev_path, strerror(errno));
	return -1;
}

static int open_ublk_control(void)
{
	char line[128];
	char name[64];
	unsigned int minor;
	FILE *devices;
	int fd;

	fd = open("/dev/ublk-control", O_RDWR | O_CLOEXEC);
	if (fd >= 0)
		return fd;
	devices = fopen("/proc/misc", "r");
	if (!devices)
		return -1;
	while (fgets(line, sizeof(line), devices)) {
		if (sscanf(line, "%u %63s", &minor, name) == 2 &&
		    !strcmp(name, "ublk-control")) {
			mknod("/dev/ublk-control", S_IFCHR | 0600,
			      makedev(10, minor));
			break;
		}
	}
	fclose(devices);
	return open("/dev/ublk-control", O_RDWR | O_CLOEXEC);
}

static size_t user_cmd_buf_size(unsigned int depth)
{
	size_t size = depth * sizeof(struct ublksrv_io_desc);

	return (size + USER_PAGE_SIZE - 1) & ~(USER_PAGE_SIZE - 1);
}

static int run_test(void)
{
	struct ublksrv_ctrl_dev_info info = {
		.nr_hw_queues = 2,
		.queue_depth = 1,
		.max_io_buf_bytes = 64 * 1024,
		.dev_id = UINT32_MAX,
		.flags = UBLK_F_SHMEM_ZC,
	};
	struct ublk_shmem_buf_reg buf_reg = {
		.len = 2 * USER_PAGE_SIZE,
	};
	struct ublk_params dev_params = {
		.len = sizeof(dev_params),
		.types = UBLK_PARAM_TYPE_BASIC,
		.basic = {
			.logical_bs_shift = 9,
			.physical_bs_shift = 12,
			.io_opt_shift = 12,
			.io_min_shift = 9,
			.max_sectors = NATIVE_TEST_PAGE_SIZE >> 9,
			.dev_sectors = 1024,
		},
	};
	struct io_uring_params ctrl_params = {
		.flags = IORING_SETUP_SQE128,
	};
	struct io_uring_params io_params = {
		.flags = IORING_SETUP_SQE128,
	};
	struct ring_mapping ctrl_map;
	struct ring_mapping io_map;
	const struct ublksrv_io_desc *iod = NULL;
	void *queue0 = MAP_FAILED;
	void *queue1 = MAP_FAILED;
	void *shared = MAP_FAILED;
	void *fallback = MAP_FAILED;
	void *child_stack = MAP_FAILED;
	struct async_write write;
	size_t cmd_size;
	off_t queue1_offset;
	bool ctrl_ring_mapped = false;
	bool io_ring_mapped = false;
	bool added = false;
	bool registered = false;
	bool registration_cycle = false;
	bool queue_armed = false;
	bool started = false;
	bool fetched = false;
	bool matched = false;
	bool io_completed = false;
	bool stopped = false;
	bool deleted = false;
	unsigned int queue_id = 0;
	unsigned int attempts;
	pid_t io_pid = -1;
	int io_status = -1;
	int ctrl_fd;
	int ctrl_ring_fd = -1;
	int io_ring_fd = -1;
	int char_fd = -1;
	int block_fd = -1;
	int buf_index = -1;
	int fetch0_ret = -EINVAL;
	int fetch1_ret = -EINVAL;
	int ret;

	ksft_print_header();
	ksft_set_plan(16);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	ctrl_fd = open_ublk_control();
	ksft_test_result(ctrl_fd >= 0, "open ublk-control\n");
	if (ctrl_fd >= 0)
		ctrl_ring_fd = setup_ring(2, &ctrl_params);
	ksft_test_result(ctrl_ring_fd >= 0, "create SQE128 control ring\n");
	if (ctrl_ring_fd >= 0)
		ctrl_ring_mapped = map_ring(ctrl_ring_fd, &ctrl_params,
					    &ctrl_map);
	ksft_test_result(ctrl_ring_mapped, "map ublk control ring\n");
	if (ctrl_ring_mapped) {
		ret = submit_ctrl_cmd(ctrl_ring_fd, &ctrl_params, &ctrl_map,
				      ctrl_fd,
				      UBLK_U_CMD_ADD_DEV, &info, &info,
				      sizeof(info), 0);
		added = ret == 0;
		if (!added)
			ksft_print_msg("add device failed: %d\n", ret);
	}
	ksft_test_result(added, "add a two-queue ublk device\n");
	if (added) {
		shared = mmap(SHARED_MAP_ADDR, 4 * USER_PAGE_SIZE,
			      PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			      -1, 0);
		if (shared != MAP_FAILED) {
			buf_reg.addr = (uintptr_t)shared;
			memset(shared, 0xa5, buf_reg.len);
			buf_index = submit_ctrl_cmd(ctrl_ring_fd, &ctrl_params,
						    &ctrl_map, ctrl_fd,
						    UBLK_U_CMD_REG_BUF,
						    &info, &buf_reg,
						    sizeof(buf_reg), 0);
			registered = buf_index >= 0;
			if (registered) {
				ret = submit_ctrl_cmd(ctrl_ring_fd, &ctrl_params,
						      &ctrl_map, ctrl_fd,
						      UBLK_U_CMD_UNREG_BUF,
						      &info, NULL, 0, buf_index);
				if (!ret) {
					registered = false;
					buf_index = submit_ctrl_cmd(ctrl_ring_fd,
								    &ctrl_params,
								    &ctrl_map, ctrl_fd,
								    UBLK_U_CMD_REG_BUF,
								    &info, &buf_reg,
								    sizeof(buf_reg), 0);
					registered = buf_index >= 0;
					registration_cycle = registered;
				}
			}
		}
	}
	ksft_test_result(registration_cycle,
			 "re-register two non-native-aligned process pages\n");
	if (added)
		char_fd = open_ublk_char(info.dev_id);
	if (char_fd >= 0) {
		cmd_size = user_cmd_buf_size(info.queue_depth);
		queue0 = mmap(NULL, cmd_size, PROT_READ,
			      MAP_SHARED | MAP_POPULATE, char_fd,
			      UBLKSRV_CMD_BUF_OFFSET);
		queue1_offset = UBLKSRV_CMD_BUF_OFFSET +
			user_cmd_buf_size(UBLK_MAX_QUEUE_DEPTH);
		queue1 = mmap(NULL, cmd_size, PROT_READ,
			      MAP_SHARED | MAP_POPULATE, char_fd,
			      queue1_offset);
	}
	ksft_test_result(queue0 != MAP_FAILED,
			 "map queue 0 command buffer in one process page\n");
	ksft_test_result(queue1 != MAP_FAILED,
			 "decode and map queue 1 command buffer offset\n");
	if (char_fd >= 0) {
		io_ring_fd = setup_ring(4, &io_params);
		if (io_ring_fd >= 0)
			io_ring_mapped = map_ring(io_ring_fd, &io_params,
						 &io_map);
	}
	ksft_test_result(io_ring_mapped, "create and map ublk I/O ring\n");
	if (io_ring_mapped && queue0 != MAP_FAILED && queue1 != MAP_FAILED) {
		fallback = mmap(FALLBACK_MAP_ADDR, 2 * NATIVE_TEST_PAGE_SIZE,
				PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS |
				MAP_FIXED_NOREPLACE, -1, 0);
		ret = submit_ctrl_cmd(ctrl_ring_fd, &ctrl_params, &ctrl_map,
				      ctrl_fd, UBLK_U_CMD_SET_PARAMS, &info,
				      &dev_params, sizeof(dev_params), 0);
		if (!ret && fallback != MAP_FAILED) {
			fetch0_ret = submit_io_cmd(io_ring_fd, &io_params, &io_map,
						   char_fd, UBLK_U_IO_FETCH_REQ,
						   0, 0, fallback);
			fetch1_ret = submit_io_cmd(io_ring_fd, &io_params, &io_map,
						   char_fd, UBLK_U_IO_FETCH_REQ,
						   1, 0,
						   (char *)fallback +
						   NATIVE_TEST_PAGE_SIZE);
		}
		if (!ret && !fetch0_ret && !fetch1_ret)
			queue_armed = true;
		if (!queue_armed)
			ksft_print_msg("set_params=%d fallback=%p fetch0=%d fetch1=%d\n",
				       ret, fallback, fetch0_ret, fetch1_ret);
	}
	ksft_test_result(queue_armed, "set parameters and arm both queues\n");
	if (queue_armed) {
		ret = submit_ctrl_cmd(ctrl_ring_fd, &ctrl_params, &ctrl_map,
				      ctrl_fd, UBLK_U_CMD_START_DEV, &info,
				      NULL, 0, getpid());
		started = ret == 0;
		if (!started)
			ksft_print_msg("start device failed: %d\n", ret);
	}
	ksft_test_result(started, "start the ublk device\n");
	if (started)
		block_fd = open_ublk_block(info.dev_id);
	if (block_fd >= 0) {
		child_stack = mmap(NULL, CHILD_STACK_SIZE,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		write.fd = block_fd;
		write.buffer = (char *)shared + USER_PAGE_SIZE;
		if (child_stack != MAP_FAILED)
			io_pid = clone(direct_write_child,
				       (char *)child_stack + CHILD_STACK_SIZE,
				       CLONE_VM | SIGCHLD, &write);
	}
	if (io_pid > 0) {
		for (attempts = 0; attempts < 200; attempts++) {
			ret = reap_io_cqe(&io_params, &io_map, &queue_id);
			if (ret != -EAGAIN) {
				fetched = ret >= 0;
				break;
			}
			if (waitpid(io_pid, &io_status, WNOHANG) == io_pid) {
				io_pid = -1;
				break;
			}
			usleep(10000);
		}
	}
	ksft_test_result(fetched, "fetch a direct write from registered pages\n");
	if (fetched) {
		iod = queue_id ? queue1 : queue0;
		matched = (iod->op_flags & UBLK_IO_F_SHMEM_ZC) &&
			  ublk_shmem_zc_index(iod->addr) == buf_index &&
			  ublk_shmem_zc_offset(iod->addr) == USER_PAGE_SIZE;
		ksft_print_msg("queue=%u op_flags=0x%x addr=0x%llx index=%u offset=%u\n",
			       queue_id, iod->op_flags,
			       (unsigned long long)iod->addr,
			       ublk_shmem_zc_index(iod->addr),
			       ublk_shmem_zc_offset(iod->addr));
	}
	ksft_test_result(matched,
			 "match the second anonymous process page\n");
	if (fetched) {
		ret = submit_io_cmd(io_ring_fd, &io_params, &io_map, char_fd,
				    UBLK_U_IO_COMMIT_AND_FETCH_REQ, queue_id,
				    USER_PAGE_SIZE,
				    (char *)fallback +
				    queue_id * NATIVE_TEST_PAGE_SIZE);
		if (!ret) {
			for (attempts = 0; attempts < 200; attempts++) {
				if (waitpid(io_pid, &io_status, WNOHANG) == io_pid) {
					io_pid = -1;
					break;
				}
				usleep(10000);
			}
			io_completed = io_pid == -1 && WIFEXITED(io_status) &&
				       WEXITSTATUS(io_status) == EXIT_SUCCESS;
		}
	}
	ksft_test_result(io_completed, "complete the direct write\n");
	if (io_pid > 0) {
		kill(io_pid, SIGKILL);
		waitpid(io_pid, &io_status, 0);
	}
	if (block_fd >= 0)
		close(block_fd);
	if (child_stack != MAP_FAILED)
		munmap(child_stack, CHILD_STACK_SIZE);
	if (started) {
		ret = submit_ctrl_cmd(ctrl_ring_fd, &ctrl_params, &ctrl_map,
				      ctrl_fd, UBLK_U_CMD_STOP_DEV, &info,
				      NULL, 0, 0);
		stopped = ret == 0;
	}
	ksft_test_result(stopped, "stop the ublk device\n");
	if (registered)
		submit_ctrl_cmd(ctrl_ring_fd, &ctrl_params, &ctrl_map, ctrl_fd,
				UBLK_U_CMD_UNREG_BUF, &info, NULL, 0,
				buf_index);
	if (queue0 != MAP_FAILED)
		munmap(queue0, cmd_size);
	if (queue1 != MAP_FAILED)
		munmap(queue1, cmd_size);
	if (fallback != MAP_FAILED)
		munmap(fallback, 2 * NATIVE_TEST_PAGE_SIZE);
	if (shared != MAP_FAILED)
		munmap(shared, 4 * USER_PAGE_SIZE);
	if (char_fd >= 0)
		close(char_fd);
	if (io_ring_mapped)
		unmap_ring(&io_map);
	if (io_ring_fd >= 0)
		close(io_ring_fd);
	if (added) {
		ret = submit_ctrl_cmd(ctrl_ring_fd, &ctrl_params, &ctrl_map,
				      ctrl_fd,
				      UBLK_U_CMD_DEL_DEV_ASYNC, &info,
				      NULL, 0, 0);
		deleted = ret == 0;
	}
	ksft_test_result(deleted, "delete the ublk device\n");
	if (ctrl_ring_mapped)
		unmap_ring(&ctrl_map);
	if (ctrl_ring_fd >= 0)
		close(ctrl_ring_fd);
	if (ctrl_fd >= 0)
		close(ctrl_fd);
	ksft_finished();
}

static int exec_compat(void)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_fail_msg("personality get failed: %s\n",
				   strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_fail_msg("personality set failed: %s\n",
				   strerror(errno));
	execl("/proc/self/exe", "ublk_mmap_ppps", "--run", NULL);
	ksft_exit_fail_msg("exec failed: %s\n", strerror(errno));
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exec_compat();
	if (argc == 2 && !strcmp(argv[1], "--run"))
		return run_test();
	return EXIT_FAILURE;
}
