// SPDX-License-Identifier: GPL-2.0
/* DMA object bounds follow the process page size, not the backing page size. */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define P3S_HARNESS_NATIVE_CONTROL 1
#include "p3s_harness.h"
#include "dma_mmap_bounds_p3s.h"

static const unsigned int sizes[] = {
	0, 4096, 4097, 8192, 8193, 12288, 16384, 16385, 20481,
};
static const char * const apis[] = { "attrs", "pages", "page-array callback",
				  "noncontiguous" };
static sigjmp_buf fault_env;

static void fault_handler(int sig)
{
	siglongjmp(fault_env, sig);
}

static bool read_byte(const volatile unsigned char *address, unsigned char *value)
{
	if (sigsetjmp(fault_env, 1))
		return false;
	*value = *address;
	return true;
}

static bool check_mapping(int fd, size_t length, off_t offset,
			  int expected_error)
{
	size_t reserve_size = length + 2 * NATIVE_PAGE_SIZE, i;
	unsigned char *reservation, *target, *mapping, value;
	bool ok = true;
	int error;

	reservation = mmap(NULL, reserve_size, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return false;
	/* Use virtual slice 1 in 4KB mode, independently of the file offset. */
	target = (void *)(((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
			 ~(NATIVE_PAGE_SIZE - 1));
	if (p3s_is_4kb_process())
		target += PROCESS_PAGE_SIZE;
	errno = 0;
	mapping = mmap(target, length, PROT_READ, MAP_SHARED | MAP_FIXED, fd, offset);
	error = errno;
	if (!expected_error) {
		ok = mapping == target;
		if (ok) {
			for (i = 0; i < length; i += PROCESS_PAGE_SIZE) {
				unsigned char want = 0x40 + (offset + i) / PROCESS_PAGE_SIZE;

				if (!read_byte(mapping + i, &value) || value != want ||
				    !read_byte(mapping + i + PROCESS_PAGE_SIZE - 1, &value) ||
				    value != want)
					ok = false;
			}
		}
	} else {
		ok = mapping == MAP_FAILED && error == expected_error;
		/* No readable slice may survive a rejected fixed mapping. */
		for (i = 0; i < length; i += PROCESS_PAGE_SIZE)
			if (read_byte(target + i, &value))
				ok = false;
	}
	if (!ok)
		ksft_print_msg("offset=%lld length=%zu expected_errno=%d map=%p errno=%d\n",
			       (long long)offset, length, expected_error,
			       mapping, error);
	munmap(reservation, reserve_size);
	return ok;
}

static void run_bounds_case(struct __test_metadata *_metadata,
			    unsigned int test)
{
	struct sigaction action = { .sa_handler = fault_handler };
	unsigned int api, index, supported = 0, unavailable = 0;
	size_t step = getpagesize();
	int fd;

	sigemptyset(&action.sa_mask);
	ASSERT_EQ(0, sigaction(SIGBUS, &action, NULL));
	ASSERT_EQ(0, sigaction(SIGSEGV, &action, NULL));
	for (api = 0; api < DMA_BOUNDS_API_COUNT; api++) {
		for (index = 0; index < ARRAY_SIZE(sizes); index++) {
			struct dma_bounds_request request = { api, sizes[index] };
			size_t end = (request.size + step - 1) & ~(step - 1);
			size_t lengths[] = { end ?: step, step,
					     (end ?: step) + step, step, step };
			off_t offsets[] = { 0, end ? end - step : 0,
					    0, end, 1ULL << 40 };
			int expected_error = !end || test >= 2 ? ENXIO : 0;
			int error;

			if (!expected_error && p3s_is_4kb_process() &&
			    (api == DMA_BOUNDS_ATTRS ||
			     api == DMA_BOUNDS_PAGES ||
			     api == DMA_BOUNDS_NONCONTIG) &&
			    offsets[test] % NATIVE_PAGE_SIZE)
				expected_error = EINVAL;
			/* Each setup owns one DMA allocation until this fd closes. */
			fd = p3s_open_fixture("/dev/dma_mmap_bounds_p3s", O_RDWR);
			if (fd < 0 && p3s_fixture_unavailable(errno))
				SKIP(return, "DMA bounds fixture is unavailable");
			ASSERT_GE(fd, 0);
			errno = 0;
			error = ioctl(fd, DMA_BOUNDS_SETUP, &request) ? errno : 0;
			if (error == EOPNOTSUPP) {
				unavailable++;
				close(fd);
				continue;
			}
			supported++;
			TH_LOG("%s page=%zu size=%u offset=%lld length=%zu",
			       apis[api], step, request.size,
			       (long long)offsets[test], lengths[test]);
			EXPECT_EQ(0, error);
			if (!error)
				EXPECT_TRUE(check_mapping(fd, lengths[test],
							  offsets[test],
							  expected_error));
			close(fd);
		}
	}
	if (!supported)
		SKIP(return, "required DMA exports unavailable");
	TH_LOG("checked=%u unsupported=%u", supported, unavailable);
}

TEST(map_from_object_start)
{
	run_bounds_case(_metadata, 0);
}

TEST(map_last_object_page)
{
	run_bounds_case(_metadata, 1);
}

TEST(reject_mapping_past_object_end)
{
	run_bounds_case(_metadata, 2);
}

TEST(reject_mapping_starting_at_object_end)
{
	run_bounds_case(_metadata, 3);
}

TEST(reject_huge_object_offset)
{
	run_bounds_case(_metadata, 4);
}

P3S_HARNESS_4KB_MAIN
