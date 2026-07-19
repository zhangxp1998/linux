// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "../kselftest.h"
#include "iov_iter_ppps.h"

#define PROCESS_PAGE_SIZE 4096UL
#define PPPS_NATIVE_PAGE_SIZE 16384UL
#define TEST_LENGTH (2 * PROCESS_PAGE_SIZE)

static void *map_test_file(int *fd_out, void **reservation_out)
{
	char path[] = "/tmp/iov-iter-XXXXXX";
	uintptr_t aligned;
	void *reservation;
	void *mapping;
	int fd;

	fd = mkstemp(path);
	if (fd < 0)
		return MAP_FAILED;
	unlink(path);
	if (ftruncate(fd, PPPS_NATIVE_PAGE_SIZE))
		goto err;

	reservation = mmap(NULL, 3 * PPPS_NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		goto err;
	aligned = ((uintptr_t)reservation + PPPS_NATIVE_PAGE_SIZE - 1) &
		  ~(PPPS_NATIVE_PAGE_SIZE - 1);
	mapping = mmap((void *)(aligned + PROCESS_PAGE_SIZE), TEST_LENGTH,
		       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
	if (mapping == MAP_FAILED) {
		munmap(reservation, 3 * PPPS_NATIVE_PAGE_SIZE);
		goto err;
	}

	*fd_out = fd;
	*reservation_out = reservation;
	return mapping;

err:
	close(fd);
	return MAP_FAILED;
}

int main(void)
{
	struct iov_iter_ppps_args request = { .length = TEST_LENGTH };
	void *reservation = MAP_FAILED;
	unsigned char *mapping;
	int backing_fd = -1;
	int device_fd;

	ksft_print_header();
	ksft_set_plan(7);
	ksft_test_result(sysconf(_SC_PAGESIZE) == PROCESS_PAGE_SIZE,
			 "process uses 4K pages\n");

	device_fd = open("/dev/" IOV_ITER_PPPS_DEVICE_NAME,
			 O_RDWR | O_CLOEXEC);
	ksft_test_result(device_fd >= 0, "open the iov_iter test device\n");
	if (device_fd < 0)
		ksft_exit_fail_msg("open failed: %s\n", strerror(errno));

	mapping = map_test_file(&backing_fd, &reservation);
	ksft_test_result(mapping != MAP_FAILED,
			 "map two file slices at a mismatched native offset\n");
	if (mapping == MAP_FAILED)
		ksft_exit_fail_msg("test mapping failed: %s\n", strerror(errno));
	mapping[0] = 0x31;
	mapping[PROCESS_PAGE_SIZE] = 0x72;

	request.address = (uintptr_t)mapping;
	ksft_test_result(ioctl(device_fd, IOV_ITER_PPPS_IOCTL, &request) == 0,
			 "run kernel iov_iter extraction checks\n");
	ksft_test_result(request.get_pages_result == 0,
			 "iov_iter_get_pages2 returns both correct slices (%d)\n",
			 request.get_pages_result);
	ksft_test_result(request.extract_pages_result == 0,
			 "iov_iter_extract_pages returns both correct slices (%d)\n",
			 request.extract_pages_result);
	ksft_test_result(request.bulk_first_len ==
			 (int)(request.native_page_size == PROCESS_PAGE_SIZE ?
			       TEST_LENGTH : PROCESS_PAGE_SIZE),
			 "compat bulk extraction stops at one process page (%d)\n",
			 request.bulk_first_len);

	munmap(reservation, 3 * PPPS_NATIVE_PAGE_SIZE);
	close(backing_fd);
	close(device_fd);
	ksft_finished();
}
