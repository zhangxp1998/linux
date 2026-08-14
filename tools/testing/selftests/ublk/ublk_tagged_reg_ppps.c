// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <linux/ublk_cmd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define USER_PAGE_SIZE 4096UL
#define NATIVE_PAGE_SIZE 16384UL
#define SQE_SIZE_128 128UL
#define USER_DATA 0x75626c6b2d746167ULL
#define ADDRESS_TAG (0xb4ULL << 56)

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

static void unmap_ring(struct ring_mapping *map)
{
	if (map->sqes != MAP_FAILED)
		munmap(map->sqes, map->sqes_size);
	if (map->cq_ring != MAP_FAILED && !map->single_mmap)
		munmap(map->cq_ring, map->cq_ring_size);
	if (map->sq_ring != MAP_FAILED)
		munmap(map->sq_ring, map->sq_ring_size);
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

static int run_test(void)
{
	struct ublksrv_ctrl_dev_info info = {
		.nr_hw_queues = 1,
		.queue_depth = 1,
		.max_io_buf_bytes = NATIVE_PAGE_SIZE,
		.dev_id = UINT32_MAX,
		.flags = UBLK_F_SHMEM_ZC,
	};
	struct ublk_shmem_buf_reg buf_reg = {
		.len = USER_PAGE_SIZE,
	};
	struct io_uring_params params = {
		.flags = IORING_SETUP_SQE128,
	};
	struct ring_mapping map;
	void *buffer = MAP_FAILED;
	bool mapped = false;
	bool added = false;
	bool registered = false;
	int tagged_result = -EINVAL;
	int untagged_result = -EINVAL;
	int ring_fd = -1;
	int ctrl_fd;
	int ret;

	ksft_print_header();
	ksft_set_plan(10);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	ctrl_fd = open("/dev/ublk-control", O_RDWR | O_CLOEXEC);
	ksft_test_result(ctrl_fd >= 0, "open ublk-control\n");
	if (ctrl_fd >= 0)
		ring_fd = setup_ring(2, &params);
	ksft_test_result(ring_fd >= 0, "create SQE128 control ring\n");
	if (ring_fd >= 0)
		mapped = map_ring(ring_fd, &params, &map);
	ksft_test_result(mapped, "map ublk control ring\n");
	if (mapped) {
		ret = submit_ctrl_cmd(ring_fd, &params, &map, ctrl_fd,
				      UBLK_U_CMD_ADD_DEV, &info, &info,
				      sizeof(info), 0);
		added = ret == 0;
	}
	ksft_test_result(added, "add a minimal ublk device\n");
	if (added) {
		buffer = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (buffer != MAP_FAILED &&
		    !((uintptr_t)buffer & (NATIVE_PAGE_SIZE - 1))) {
			buf_reg.addr = (uintptr_t)buffer;
			untagged_result = submit_ctrl_cmd(ring_fd, &params, &map,
							  ctrl_fd,
							  UBLK_U_CMD_REG_BUF,
							  &info, &buf_reg,
							  sizeof(buf_reg), 0);
			registered = untagged_result >= 0;
		}
	}
	ksft_test_result(buffer != MAP_FAILED &&
			 !((uintptr_t)buffer & (NATIVE_PAGE_SIZE - 1)),
			 "map a native-aligned compat buffer\n");
	ksft_test_result(registered,
			 "register the untagged compat buffer (%s)\n",
			 registered ? "ok" : strerror(-untagged_result));
	if (registered) {
		ret = submit_ctrl_cmd(ring_fd, &params, &map, ctrl_fd,
				      UBLK_U_CMD_UNREG_BUF, &info, NULL, 0,
				      untagged_result);
		registered = ret != 0;
		if (!registered) {
			buf_reg.addr = (uintptr_t)buffer | ADDRESS_TAG;
			tagged_result = submit_ctrl_cmd(ring_fd, &params, &map,
							ctrl_fd,
							UBLK_U_CMD_REG_BUF,
							&info, &buf_reg,
							sizeof(buf_reg), 0);
			registered = tagged_result >= 0;
		}
	}
	ksft_test_result(!registered || tagged_result >= 0,
			 "unregister the untagged buffer\n");
	ksft_test_result(tagged_result >= 0,
			 "register the same tagged compat buffer (%s)\n",
			 tagged_result >= 0 ? "ok" : strerror(-tagged_result));

	if (tagged_result >= 0)
		submit_ctrl_cmd(ring_fd, &params, &map, ctrl_fd,
				UBLK_U_CMD_UNREG_BUF, &info, NULL, 0,
				tagged_result);
	if (buffer != MAP_FAILED)
		munmap(buffer, NATIVE_PAGE_SIZE);
	if (added)
		ret = submit_ctrl_cmd(ring_fd, &params, &map, ctrl_fd,
				      UBLK_U_CMD_DEL_DEV_ASYNC, &info,
				      NULL, 0, 0);
	else
		ret = -EINVAL;
	ksft_test_result(ret == 0, "delete the ublk device\n");
	if (mapped)
		unmap_ring(&map);
	if (ring_fd >= 0)
		close(ring_fd);
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
	execl("/proc/self/exe", "ublk_tagged_reg_ppps", "--run", NULL);
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
