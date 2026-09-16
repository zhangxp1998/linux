// SPDX-License-Identifier: GPL-2.0
/*
 * A 4K compat ublk server maps both per-queue command buffers at their 4K
 * offsets: queue 0 fits in one process page and queue 1 sits at an offset
 * that only decodes correctly when ublk uses the process page size.
 */
#define _GNU_SOURCE

#include <linux/io_uring.h>
#include <linux/ublk_cmd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

#include "kselftest_ppps.h"

#define USER_DATA 0x75626c6b2d707070ULL
#define SQE_SIZE_128 128UL

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
			   struct ublksrv_ctrl_dev_info *info)
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
	if (_IOC_NR(cmd_op) == UBLK_CMD_ADD_DEV) {
		cmd->addr = (uintptr_t)info;
		cmd->len = sizeof(*info);
	}
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

	return (size + PROCESS_PAGE_SIZE - 1) & ~(PROCESS_PAGE_SIZE - 1);
}

static int run_test(void)
{
	struct ublksrv_ctrl_dev_info info = {
		.nr_hw_queues = 2,
		.queue_depth = 1,
		.max_io_buf_bytes = 64 * 1024,
		.dev_id = UINT32_MAX,
	};
	struct io_uring_params ctrl_params = {
		.flags = IORING_SETUP_SQE128,
	};
	struct ring_mapping ctrl_map;
	void *queue0 = MAP_FAILED;
	void *queue1 = MAP_FAILED;
	size_t cmd_size = 0;
	off_t queue1_offset;
	bool ctrl_ring_mapped = false;
	bool added = false;
	bool deleted = false;
	int ctrl_fd;
	int ctrl_ring_fd = -1;
	int char_fd = -1;
	int ret;

	ksft_print_header();
	ksft_set_plan(7);
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
				      UBLK_U_CMD_ADD_DEV, &info);
		added = ret == 0;
		if (!added)
			ksft_print_msg("add device failed: %d\n", ret);
	}
	ksft_test_result(added, "add a two-queue ublk device\n");
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
	if (queue0 != MAP_FAILED)
		munmap(queue0, cmd_size);
	if (queue1 != MAP_FAILED)
		munmap(queue1, cmd_size);
	if (char_fd >= 0)
		close(char_fd);
	if (added) {
		ret = submit_ctrl_cmd(ctrl_ring_fd, &ctrl_params, &ctrl_map,
				      ctrl_fd,
				      UBLK_U_CMD_DEL_DEV_ASYNC, &info);
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

PPPS_COMPAT_MAIN(run_test)
