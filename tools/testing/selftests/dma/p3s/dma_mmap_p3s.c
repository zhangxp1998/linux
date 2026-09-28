// SPDX-License-Identifier: GPL-2.0
/*
 * dma_mmap_attrs() / dma_mmap_pages() map exactly the requested 4K slice.
 * A mapping at a 4K DMA offset is rejected on native 16K backing.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "p3s_test.h"
#include "p3s_harness.h"

struct dma_mmap_case {
	const char *name;
	const char *device;
	unsigned char first_marker;
};

static const struct dma_mmap_case dma_mmap_cases[] = {
	{ "attrs", "/dev/dma_mmap_attrs_p3s", 0x61 },
	{ "pages", "/dev/dma_mmap_pages_p3s", 0x51 },
};
static const struct dma_mmap_case *selected;
static sigjmp_buf fault_environment;

static void fault_handler(int signal_number)
{
	siglongjmp(fault_environment, signal_number);
}

static bool read_byte(const unsigned char *address, unsigned char *value)
{
	if (sigsetjmp(fault_environment, 1))
		return false;
	*value = *address;
	return true;
}

FIXTURE(dma_mmap) {
	int fd;
};

FIXTURE_SETUP(dma_mmap)
{
	struct sigaction action = { .sa_handler = fault_handler };

	self->fd = p3s_open_fixture(selected->device, O_RDWR);
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "%s fixture is unavailable", selected->name);
	ASSERT_GE(self->fd, 0);
	sigemptyset(&action.sa_mask);
	ASSERT_EQ(0, sigaction(SIGSEGV, &action, NULL));
	ASSERT_EQ(0, sigaction(SIGBUS, &action, NULL));
}

FIXTURE_TEARDOWN(dma_mmap)
{
	close(self->fd);
}

static bool map_first_slice(int fd, unsigned char **reservation,
			    unsigned char **mapping)
{
	*reservation = mmap(NULL, NATIVE_PAGE_SIZE, PROT_NONE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (*reservation == MAP_FAILED)
		return false;
	*mapping = mmap(*reservation, PROCESS_PAGE_SIZE, PROT_READ,
			MAP_SHARED | MAP_FIXED, fd, 0);
	if (*mapping == *reservation)
		return true;
	munmap(*reservation, NATIVE_PAGE_SIZE);
	return false;
}

TEST_F(dma_mmap, first_slice_preserves_marker)
{
	unsigned char *reservation, *mapping, value = 0;

	ASSERT_TRUE(map_first_slice(self->fd, &reservation, &mapping));
	EXPECT_TRUE(read_byte(mapping, &value));
	EXPECT_EQ(selected->first_marker, value);
	munmap(reservation, NATIVE_PAGE_SIZE);
}

TEST_F(dma_mmap, unrequested_guard_slices_remain_inaccessible)
{
	unsigned char *reservation, *mapping, value = 0;
	unsigned int guard;

	ASSERT_TRUE(map_first_slice(self->fd, &reservation, &mapping));
	for (guard = 1; guard < P3S_SLICES; guard++)
		EXPECT_FALSE(read_byte(mapping + guard * PROCESS_PAGE_SIZE,
				       &value));
	munmap(reservation, NATIVE_PAGE_SIZE);
}

TEST_F(dma_mmap, non_native_dma_offset_is_rejected)
{
	unsigned char *reservation, *mapping;
	int error;

	reservation = mmap(NULL, NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(reservation, MAP_FAILED);
	errno = 0;
	mapping = mmap(reservation, PROCESS_PAGE_SIZE, PROT_READ,
		       MAP_SHARED | MAP_FIXED, self->fd, PROCESS_PAGE_SIZE);
	error = errno;
	EXPECT_EQ(MAP_FAILED, mapping);
	EXPECT_EQ(EINVAL, error);
	munmap(reservation, NATIVE_PAGE_SIZE);
}

static const struct dma_mmap_case *find_case(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dma_mmap_cases); i++)
		if (!strcmp(dma_mmap_cases[i].name, name))
			return &dma_mmap_cases[i];
	return NULL;
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (mode) {
		if (!strcmp(mode, P3S_RUN_FLAG) && argc == 3)
			selected = find_case(argv[2]);
	} else if (argc == 2) {
		selected = find_case(argv[1]);
	}
	if (!selected) {
		fprintf(stderr, "usage: %s attrs|pages\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (getpagesize() == NATIVE_PAGE_SIZE) {
		p3s_require_native_compat();
		exec_compat(argv[0], P3S_RUN_FLAG, selected->name, NULL);
	}
	if (mode)
		p3s_require_compat();
	return p3s_harness_run();
}
