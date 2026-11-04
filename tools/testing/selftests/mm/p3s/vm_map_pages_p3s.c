// SPDX-License-Identifier: GPL-2.0
/*
 * vm_map_pages() through the vm_map_pages_p3s fixture maps five 4K slices at
 * a 4K file offset for a 4K compat process, each with a present PTE holding
 * the expected logical page, and a forked child inherits all of them.
 * Partial native vm_insert_page() insertion populates the targeted slices
 * without disturbing sibling 4K slices.
 */
#define _GNU_SOURCE

#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_test.h"
#include "p3s_harness.h"
#include "vm_map_pages_p3s.h"

#define FILE_OFFSET	PROCESS_PAGE_SIZE
#define MAPPING_PAGES	5
#define MAPPING_SIZE	(MAPPING_PAGES * PROCESS_PAGE_SIZE)
#define FIRST_MARKER	0x41

static sigjmp_buf fault_environment;

static void sigbus_handler(int signal_number)
{
	siglongjmp(fault_environment, signal_number);
}

static bool read_mapping(const unsigned char *mapping, unsigned char *value)
{
	if (sigsetjmp(fault_environment, 1))
		return false;
	*value = *mapping;
	return true;
}

static bool native_insert_preserves_siblings(int fd, bool *conflict)
{
	unsigned char value;
	unsigned char *mapping;
	bool populated;

	*conflict = false;
	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd,
		       VM_MAP_PAGES_P3S_NATIVE_PARTIAL * PROCESS_PAGE_SIZE);
	if (mapping == MAP_FAILED)
		return false;
	populated = read_mapping(mapping, &value) && value == 0x41;
	populated &= read_mapping(mapping + PROCESS_PAGE_SIZE, &value) &&
		     value == 0x42;
	populated &= read_mapping(mapping + 2 * PROCESS_PAGE_SIZE, &value) &&
		     value == 0x43;
	*conflict = read_mapping(mapping + 3 * PROCESS_PAGE_SIZE, &value) &&
		    value == 0x44;
	munmap(mapping, NATIVE_PAGE_SIZE);
	return populated;
}

FIXTURE(map_pages) {
	int fd;
};

FIXTURE_SETUP(map_pages)
{
	struct sigaction action = {
		.sa_handler = sigbus_handler,
	};

	self->fd = p3s_open_fixture("/dev/vm_map_pages_p3s", O_RDWR);
	if (self->fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "vm_map_pages fixture is unavailable");
	ASSERT_GE(self->fd, 0);
	sigemptyset(&action.sa_mask);
	ASSERT_EQ(0, sigaction(SIGBUS, &action, NULL));
}

FIXTURE_TEARDOWN(map_pages)
{
	close(self->fd);
}

static unsigned char *map_five_pages(int fd)
{
	return mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		    MAP_SHARED, fd, FILE_OFFSET);
}

TEST_F(map_pages, all_five_slices_are_present)
{
	unsigned char *mapping = map_five_pages(self->fd);
	unsigned char value;
	unsigned int page;

	ASSERT_NE(mapping, MAP_FAILED);
	for (page = 0; page < MAPPING_PAGES; page++)
		EXPECT_TRUE(read_mapping(mapping + page * PROCESS_PAGE_SIZE,
					 &value));
	munmap(mapping, MAPPING_SIZE);
}

TEST_F(map_pages, slices_hold_logical_pages_one_through_five)
{
	unsigned char *mapping = map_five_pages(self->fd);
	unsigned char value;
	unsigned int page;

	ASSERT_NE(mapping, MAP_FAILED);
	for (page = 0; page < MAPPING_PAGES; page++) {
		ASSERT_TRUE(read_mapping(mapping + page * PROCESS_PAGE_SIZE,
					 &value));
		EXPECT_EQ(FIRST_MARKER + page, value);
	}
	munmap(mapping, MAPPING_SIZE);
}

TEST_F(map_pages, fork_inherits_all_five_slices)
{
	unsigned char *mapping = map_five_pages(self->fd);
	unsigned char value;
	unsigned int page;
	int status;
	pid_t pid;

	ASSERT_NE(mapping, MAP_FAILED);
	pid = fork();
	ASSERT_GE(pid, 0);
	if (!pid) {
		for (page = 0; page < MAPPING_PAGES; page++)
			if (!read_mapping(mapping + page * PROCESS_PAGE_SIZE,
					  &value) || value != FIRST_MARKER + page)
				_exit(EXIT_FAILURE);
		_exit(EXIT_SUCCESS);
	}
	ASSERT_EQ(pid, waitpid(pid, &status, 0));
	EXPECT_TRUE(WIFEXITED(status) && !WEXITSTATUS(status));
	munmap(mapping, MAPPING_SIZE);
}

TEST_F(map_pages, native_insert_populates_unused_slices)
{
	bool conflict;

	EXPECT_TRUE(native_insert_preserves_siblings(self->fd, &conflict));
}

TEST_F(map_pages, native_insert_preserves_existing_sibling)
{
	bool conflict;

	EXPECT_TRUE(native_insert_preserves_siblings(self->fd, &conflict));
	EXPECT_TRUE(conflict);
}

P3S_HARNESS_4KB_MAIN
