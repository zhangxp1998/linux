// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include "../kselftest.h"

#define PROCESS_PAGE_SIZE 4096UL
#define PPPS_NATIVE_PAGE_SIZE 16384UL
#define TEST_LENGTH (2 * PROCESS_PAGE_SIZE)
#define USER_DATA 0x6669786564627566ULL

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
	return syscall(SYS_io_uring_setup, entries, params);
}

static int enter_ring(int fd, unsigned int submit, unsigned int wait_nr,
		      unsigned int flags)
{
	return syscall(SYS_io_uring_enter, fd, submit, wait_nr, flags, NULL, 0);
}

static int register_ring(int fd, unsigned int opcode, void *arg,
			 unsigned int nr_args)
{
	return syscall(SYS_io_uring_register, fd, opcode, arg, nr_args);
}

static void *map_region(int fd, size_t size, off_t offset)
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
	map->sqes_size = params->sq_entries * sizeof(struct io_uring_sqe);
	map->single_mmap = params->features & IORING_FEAT_SINGLE_MMAP;
	if (map->single_mmap) {
		shared_size = map->sq_ring_size > map->cq_ring_size ?
			map->sq_ring_size : map->cq_ring_size;
		map->sq_ring_size = shared_size;
		map->cq_ring_size = shared_size;
	}

	map->sq_ring = map_region(fd, map->sq_ring_size, IORING_OFF_SQ_RING);
	if (map->sq_ring == MAP_FAILED)
		return false;
	if (map->single_mmap) {
		map->cq_ring = map->sq_ring;
	} else {
		map->cq_ring = map_region(fd, map->cq_ring_size,
					  IORING_OFF_CQ_RING);
		if (map->cq_ring == MAP_FAILED)
			return false;
	}
	map->sqes = map_region(fd, map->sqes_size, IORING_OFF_SQES);
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

static int submit_read_fixed(int ring_fd, int input_fd,
			     const struct io_uring_params *params,
			     struct ring_mapping *map, void *buffer)
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
	unsigned int index;
	unsigned int tail;
	int result;

	if (*sq_tail - *sq_head >= params->sq_entries)
		return -EBUSY;
	tail = *sq_tail;
	index = tail & *sq_mask;
	sqe = &map->sqes[index];
	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = IORING_OP_READ_FIXED;
	sqe->fd = input_fd;
	sqe->off = 0;
	sqe->addr = (uintptr_t)buffer;
	sqe->len = TEST_LENGTH;
	sqe->buf_index = 0;
	sqe->user_data = USER_DATA;
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

static void *map_backing_slice(int fd, void **reservation_out)
{
	uintptr_t aligned;
	void *reservation;
	void *mapping;

	reservation = mmap(NULL, 3 * PPPS_NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return MAP_FAILED;
	aligned = ((uintptr_t)reservation + PPPS_NATIVE_PAGE_SIZE - 1) &
		  ~(PPPS_NATIVE_PAGE_SIZE - 1);
	mapping = mmap((void *)(aligned + PROCESS_PAGE_SIZE), TEST_LENGTH,
		       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
	if (mapping == MAP_FAILED) {
		munmap(reservation, 3 * PPPS_NATIVE_PAGE_SIZE);
		return MAP_FAILED;
	}
	*reservation_out = reservation;
	return mapping;
}

static int make_file(char *path, size_t size)
{
	int fd = mkstemp(path);

	if (fd >= 0) {
		unlink(path);
		if (ftruncate(fd, size)) {
			close(fd);
			fd = -1;
		}
	}
	return fd;
}

static long read_status_kb(const char *name)
{
	char line[256];
	FILE *file;
	long value = -1;

	file = fopen("/proc/self/status", "re");
	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		if (!strncmp(line, name, strlen(name)) &&
		    sscanf(line, "%*[^:]: %ld kB", &value) == 1)
			break;
		value = -1;
	}
	fclose(file);
	return value;
}

static unsigned long mapping_kernel_page_size(uintptr_t address)
{
	char *line = NULL;
	size_t line_size = 0;
	unsigned long page_size = 0;
	bool in_mapping = false;
	FILE *file;

	file = fopen("/proc/self/smaps", "re");
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

int main(void)
{
	unsigned char input_data[TEST_LENGTH];
	struct io_uring_params params = {};
	struct ring_mapping ring = {};
	char backing_path[] = "/tmp/io-fixed-backing-XXXXXX";
	char input_path[] = "/tmp/io-fixed-input-XXXXXX";
	void *reservation = MAP_FAILED;
	unsigned char *mapping = MAP_FAILED;
	unsigned long kernel_page_size;
	unsigned long expected_pin_kb;
	unsigned long pin_span;
	struct iovec iov;
	unsigned char adjacent = 0;
	bool ring_mapped = false;
	long vmpin_before;
	long vmpin_after;
	long vmpin_released;
	int registered = -1;
	int unregistered = -1;
	int io_result = -1;
	int backing_fd;
	int input_fd;
	int ring_fd;

	ksft_print_header();
	ksft_set_plan(9);
	ksft_test_result(sysconf(_SC_PAGESIZE) == PROCESS_PAGE_SIZE,
			 "process uses 4K pages\n");

	backing_fd = make_file(backing_path, PPPS_NATIVE_PAGE_SIZE);
	input_fd = make_file(input_path, TEST_LENGTH);
	if (backing_fd < 0 || input_fd < 0)
		ksft_exit_fail_msg("test file setup failed: %s\n", strerror(errno));
	memset(input_data, 0xa5, PROCESS_PAGE_SIZE);
	memset(input_data + PROCESS_PAGE_SIZE, 0xb6, PROCESS_PAGE_SIZE);
	if (pwrite(input_fd, input_data, sizeof(input_data), 0) !=
	    sizeof(input_data) ||
	    pwrite(backing_fd, "\x11", 1, 0) != 1 ||
	    pwrite(backing_fd, "\x22", 1, PROCESS_PAGE_SIZE) != 1 ||
	    pwrite(backing_fd, "\x5c", 1, TEST_LENGTH) != 1)
		ksft_exit_fail_msg("test file initialization failed: %s\n",
				   strerror(errno));

	mapping = map_backing_slice(backing_fd, &reservation);
	ksft_test_result(mapping != MAP_FAILED && mapping[0] == 0x11,
			 "map file offset zero at a mismatched native offset\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("test mapping failed: %s\n", strerror(errno));

	ring_fd = setup_ring(2, &params);
	if (ring_fd >= 0)
		ring_mapped = map_ring(ring_fd, &params, &ring);
	ksft_test_result(ring_fd >= 0 && ring_mapped, "create and map io_uring\n");

	iov.iov_base = mapping;
	iov.iov_len = TEST_LENGTH;
	vmpin_before = read_status_kb("VmPin");
	if (ring_mapped)
		registered = register_ring(ring_fd, IORING_REGISTER_BUFFERS,
					   &iov, 1);
	ksft_test_result(registered == 0,
			 "register the mismatched 4K fixed buffer (ret=%d errno=%d)\n",
			 registered, registered < 0 ? errno : 0);
	vmpin_after = read_status_kb("VmPin");
	kernel_page_size = mapping_kernel_page_size((uintptr_t)mapping);
	pin_span = kernel_page_size ?
		((uintptr_t)mapping & (kernel_page_size - 1)) + TEST_LENGTH : 0;
	expected_pin_kb = kernel_page_size ?
		((pin_span + kernel_page_size - 1) / kernel_page_size) *
		kernel_page_size / 1024 : 0;
	ksft_print_msg("VmPin=%ld->%ldkB kernel_page_size=%lu expected_delta=%lu\n",
		       vmpin_before, vmpin_after, kernel_page_size, expected_pin_kb);
	ksft_test_result(vmpin_before >= 0 &&
			 vmpin_after - vmpin_before == (long)expected_pin_kb,
			 "report fixed buffers in native pinned bytes\n");

	if (registered == 0)
		io_result = submit_read_fixed(ring_fd, input_fd, &params, &ring,
					      mapping);
	ksft_test_result(io_result == TEST_LENGTH,
			 "complete READ_FIXED into the buffer (ret=%d)\n",
			 io_result);
	ksft_test_result(mapping[0] == 0xa5 &&
			 mapping[PROCESS_PAGE_SIZE] == 0xb6,
			 "READ_FIXED updates both registered file slices\n");
	if (pread(backing_fd, &adjacent, 1, TEST_LENGTH) != 1)
		adjacent = 0;
	ksft_test_result(adjacent == 0x5c,
			 "READ_FIXED leaves the adjacent file slice unchanged\n");
	if (registered == 0)
		unregistered = register_ring(ring_fd, IORING_UNREGISTER_BUFFERS,
					     NULL, 0);
	vmpin_released = read_status_kb("VmPin");
	ksft_test_result(unregistered == 0 && vmpin_released == vmpin_before,
			 "unaccount fixed-buffer pins after unregister\n");

	if (ring_mapped)
		unmap_ring(&ring);
	if (ring_fd >= 0)
		close(ring_fd);
	munmap(reservation, 3 * PPPS_NATIVE_PAGE_SIZE);
	close(input_fd);
	close(backing_fd);
	ksft_finished();
}
