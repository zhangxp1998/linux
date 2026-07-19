// SPDX-License-Identifier: GPL-2.0
/*
 * io_uring rings of a 4K compat process are mapped in 4K process-page
 * units: SQ/CQ/SQE and provided-buffer rings map and complete a NOP, their
 * padding beyond the ring is rejected or faults, and user-backed 4K ring
 * regions work with IORING_SETUP_NO_MMAP.
 */
#define _GNU_SOURCE

#include <linux/stddef.h>

#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "kselftest_ppps.h"

#define USER_DATA 0x697572696e672d34ULL
#define PBUF_BGID 7
#define USER_PBUF_BGID 8

struct ring_mapping {
	void *sq_ring;
	void *cq_ring;
	struct io_uring_sqe *sqes;
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

static int register_ring(int fd, unsigned int opcode, void *arg,
			 unsigned int nr_args)
{
	return syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

static void *mmap_ring_region(int fd, size_t size, off_t offset);

static bool ring_tail_is_inaccessible(int fd, off_t offset)
{
	void *mapping;
	ssize_t ret;
	int pipefd[2];
	int saved_errno;

	errno = 0;
	mapping = mmap_ring_region(fd, 2 * PROCESS_PAGE_SIZE, offset);
	if (mapping == MAP_FAILED)
		return errno == EINVAL;
	if (pipe(pipefd)) {
		munmap(mapping, 2 * PROCESS_PAGE_SIZE);
		return false;
	}
	errno = 0;
	ret = write(pipefd[1], mapping + PROCESS_PAGE_SIZE, 1);
	saved_errno = errno;
	close(pipefd[0]);
	close(pipefd[1]);
	munmap(mapping, 2 * PROCESS_PAGE_SIZE);
	if (ret == 1)
		ksft_print_msg("ring padding was readable\n");
	else if (ret < 0 && saved_errno != EFAULT)
		ksft_print_msg("padding probe failed: %s\n",
			       strerror(saved_errno));
	return ret < 0 && saved_errno == EFAULT;
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

static void *mmap_ring_region(int fd, size_t size, off_t offset)
{
	int prot = PROT_READ | PROT_WRITE;
	int flags = MAP_SHARED | MAP_POPULATE;

	return mmap(NULL, size, prot, flags, fd, offset);
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
	map->sqes_size = params->sq_entries * sizeof(struct io_uring_sqe);
	map->single_mmap = params->features & IORING_FEAT_SINGLE_MMAP;
	if (map->single_mmap) {
		shared_size = map->sq_ring_size > map->cq_ring_size ?
			map->sq_ring_size : map->cq_ring_size;
		map->sq_ring_size = shared_size;
		map->cq_ring_size = shared_size;
	}

	map->sq_ring = mmap_ring_region(fd, map->sq_ring_size, IORING_OFF_SQ_RING);
	if (map->sq_ring == MAP_FAILED) {
		ksft_print_msg("SQ ring mmap (%zu bytes) failed: %s\n",
			       map->sq_ring_size, strerror(errno));
		return false;
	}
	if (map->single_mmap) {
		map->cq_ring = map->sq_ring;
	} else {
		map->cq_ring = mmap_ring_region(fd, map->cq_ring_size, IORING_OFF_CQ_RING);
		if (map->cq_ring == MAP_FAILED) {
			ksft_print_msg("CQ ring mmap (%zu bytes) failed: %s\n",
				       map->cq_ring_size, strerror(errno));
			return false;
		}
	}
	map->sqes = mmap_ring_region(fd, map->sqes_size, IORING_OFF_SQES);
	if (map->sqes == MAP_FAILED)
		ksft_print_msg("SQE mmap (%zu bytes) failed: %s\n",
			       map->sqes_size, strerror(errno));
	return map->sqes != MAP_FAILED;
}

static bool submit_nop(int fd, const struct io_uring_params *params,
		       struct ring_mapping *map)
{
	unsigned int *sq_head = map->sq_ring + params->sq_off.head;
	unsigned int *sq_tail = map->sq_ring + params->sq_off.tail;
	unsigned int *sq_mask = map->sq_ring + params->sq_off.ring_mask;
	unsigned int *sq_array = map->sq_ring + params->sq_off.array;
	unsigned int *cq_head = map->cq_ring + params->cq_off.head;
	unsigned int *cq_tail = map->cq_ring + params->cq_off.tail;
	unsigned int *cq_mask = map->cq_ring + params->cq_off.ring_mask;
	struct io_uring_cqe *cqes = map->cq_ring + params->cq_off.cqes;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	unsigned int tail;
	unsigned int index;
	bool success;

	if (*sq_tail - *sq_head >= params->sq_entries)
		return false;
	tail = *sq_tail;
	index = tail & *sq_mask;
	sqe = &map->sqes[index];
	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = IORING_OP_NOP;
	sqe->user_data = USER_DATA;
	sq_array[index] = index;
	__atomic_store_n(sq_tail, tail + 1, __ATOMIC_RELEASE);
	if (enter_ring(fd, 1, 1, IORING_ENTER_GETEVENTS) != 1) {
		ksft_print_msg("io_uring_enter failed: %s\n", strerror(errno));
		return false;
	}
	if (__atomic_load_n(cq_tail, __ATOMIC_ACQUIRE) == *cq_head)
		return false;
	cqe = &cqes[*cq_head & *cq_mask];
	success = cqe->user_data == USER_DATA && cqe->res == 0;
	__atomic_store_n(cq_head, *cq_head + 1, __ATOMIC_RELEASE);
	return success;
}

static int run_test(void)
{
	struct io_uring_params params = {};
	struct io_uring_buf_reg reg = {};
	struct ring_mapping map;
	void *pbuf = MAP_FAILED;
	off_t pbuf_offset;
	bool mapped = false;
	bool completed = false;
	bool sqe_tail_rejected = false;
	bool pbuf_registered = false;
	bool pbuf_tail_rejected = false;
	bool user_pbuf_registered = false;
	void *user_pbuf = MAP_FAILED;
	int fd;

	ksft_print_header();
	if (access("/proc/sys/kernel/io_uring_disabled", F_OK))
		ksft_exit_skip("CONFIG_IO_URING is disabled\n");
	ksft_set_plan(8);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");
	fd = setup_ring(2, &params);
	ksft_test_result(fd >= 0, "create io_uring\n");
	if (fd >= 0) {
		mapped = map_ring(fd, &params, &map);
		if (mapped)
			completed = submit_nop(fd, &params, &map);
		unmap_ring(&map);
	}
	ksft_test_result(mapped && completed,
			 "map 4K rings and complete a NOP\n");
	if (fd >= 0)
		sqe_tail_rejected = ring_tail_is_inaccessible(fd,
							       IORING_OFF_SQES);
	ksft_test_result(sqe_tail_rejected,
			 "reject or fault the SQE ring's padding page\n");

	reg.ring_entries = 2;
	reg.bgid = PBUF_BGID;
	reg.flags = IOU_PBUF_RING_MMAP;
	if (fd >= 0)
		pbuf_registered = register_ring(fd, IORING_REGISTER_PBUF_RING,
						&reg, 1) == 0;
	ksft_test_result(pbuf_registered,
			 "register a kernel-allocated provided-buffer ring\n");
	pbuf_offset = IORING_OFF_PBUF_RING |
		((off_t)PBUF_BGID << IORING_OFF_PBUF_SHIFT);
	if (pbuf_registered)
		pbuf = mmap_ring_region(fd, PROCESS_PAGE_SIZE, pbuf_offset);
	ksft_test_result(pbuf != MAP_FAILED,
			 "map the provided-buffer ring's process page\n");
	if (pbuf != MAP_FAILED)
		munmap(pbuf, PROCESS_PAGE_SIZE);
	if (pbuf_registered)
		pbuf_tail_rejected = ring_tail_is_inaccessible(fd, pbuf_offset);
	ksft_test_result(pbuf_tail_rejected,
			 "reject or fault the provided-buffer ring's padding page\n");

	user_pbuf = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memset(&reg, 0, sizeof(reg));
	reg.ring_addr = (uintptr_t)user_pbuf;
	reg.ring_entries = 2;
	reg.bgid = USER_PBUF_BGID;
	if (fd >= 0 && user_pbuf != MAP_FAILED)
		user_pbuf_registered =
			register_ring(fd, IORING_REGISTER_PBUF_RING, &reg, 1) == 0;
	if (!user_pbuf_registered)
		ksft_print_msg("user provided-buffer registration failed: %s\n",
			       strerror(errno));
	ksft_test_result(user_pbuf_registered,
			 "register a user-backed 4K provided-buffer ring\n");
	if (fd >= 0)
		close(fd);
	if (user_pbuf != MAP_FAILED)
		munmap(user_pbuf, PROCESS_PAGE_SIZE);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
