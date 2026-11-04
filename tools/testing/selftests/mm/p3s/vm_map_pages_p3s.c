// SPDX-License-Identifier: GPL-2.0
/*
 * vm_map_pages() through the vm_map_pages_p3s fixture maps five 4K slices at
 * a 4K file offset for a 4K compat process, each with a present PTE holding
 * the expected logical page, and a forked child inherits all of them.
 *
 * The vm_insert_pages() edge cases need symbols that a kernel built with
 * CONFIG_TRIM_UNUSED_KSYMS does not export; the fixture then fails those
 * mmap() calls with EOPNOTSUPP and they are reported as skipped.
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

enum edge_result {
	EDGE_FAIL,
	EDGE_PASS,
	EDGE_UNSUPPORTED,
};

static enum edge_result mmap_edge_case(int fd, unsigned long offset,
				       bool readable)
{
	unsigned char value;
	unsigned char *mapping;
	bool result;

	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, offset * PROCESS_PAGE_SIZE);
	if (mapping == MAP_FAILED)
		return errno == EOPNOTSUPP ? EDGE_UNSUPPORTED : EDGE_FAIL;
	result = read_mapping(mapping, &value) == readable;
	munmap(mapping, PROCESS_PAGE_SIZE);
	return result ? EDGE_PASS : EDGE_FAIL;
}

static enum edge_result native_insert_preserves_siblings(int fd,
							   enum edge_result *conflict)
{
	unsigned char value;
	unsigned char *mapping;
	bool populated;

	*conflict = EDGE_FAIL;
	mapping = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd,
		       VM_MAP_PAGES_P3S_NATIVE_PARTIAL * PROCESS_PAGE_SIZE);
	if (mapping == MAP_FAILED) {
		if (errno != EOPNOTSUPP)
			return EDGE_FAIL;
		*conflict = EDGE_UNSUPPORTED;
		return EDGE_UNSUPPORTED;
	}
	populated = read_mapping(mapping, &value) && value == 0x41;
	populated &= read_mapping(mapping + PROCESS_PAGE_SIZE, &value) &&
		     value == 0x42;
	populated &= read_mapping(mapping + 2 * PROCESS_PAGE_SIZE, &value) &&
		     value == 0x43;
	if (read_mapping(mapping + 3 * PROCESS_PAGE_SIZE, &value) && value == 0x44)
		*conflict = EDGE_PASS;
	munmap(mapping, NATIVE_PAGE_SIZE);
	return populated ? EDGE_PASS : EDGE_FAIL;
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

TEST_F(map_pages, zero_page_insert_is_noop)
{
	enum edge_result result = mmap_edge_case(self->fd,
					VM_MAP_PAGES_P3S_ZERO, false);

	if (result == EDGE_UNSUPPORTED)
		SKIP(return, "vm_insert_pages unavailable");
	EXPECT_EQ(EDGE_PASS, result);
}

TEST_F(map_pages, insert_before_vma_rejected)
{
	enum edge_result result = mmap_edge_case(self->fd,
					VM_MAP_PAGES_P3S_BEFORE, false);

	if (result == EDGE_UNSUPPORTED)
		SKIP(return, "vm_insert_pages unavailable");
	EXPECT_EQ(EDGE_PASS, result);
}

TEST_F(map_pages, insert_at_vma_end_rejected)
{
	enum edge_result result = mmap_edge_case(self->fd,
					VM_MAP_PAGES_P3S_AFTER, false);

	if (result == EDGE_UNSUPPORTED)
		SKIP(return, "vm_insert_pages unavailable");
	EXPECT_EQ(EDGE_PASS, result);
}

TEST_F(map_pages, too_many_native_pages_rejected)
{
	enum edge_result result = mmap_edge_case(self->fd,
					VM_MAP_PAGES_P3S_TOO_MANY, false);

	if (result == EDGE_UNSUPPORTED)
		SKIP(return, "vm_insert_pages unavailable");
	EXPECT_EQ(EDGE_PASS, result);
}

TEST_F(map_pages, duplicate_pte_preserves_remaining_page)
{
	enum edge_result result = mmap_edge_case(self->fd,
					VM_MAP_PAGES_P3S_BUSY, true);

	if (result == EDGE_UNSUPPORTED)
		SKIP(return, "vm_insert_pages unavailable");
	EXPECT_EQ(EDGE_PASS, result);
}

TEST_F(map_pages, native_insert_populates_unused_slices)
{
	enum edge_result conflict;
	enum edge_result result = native_insert_preserves_siblings(self->fd,
							   &conflict);

	if (result == EDGE_UNSUPPORTED)
		SKIP(return, "vm_insert_pages unavailable");
	EXPECT_EQ(EDGE_PASS, result);
}

TEST_F(map_pages, native_insert_preserves_existing_sibling)
{
	enum edge_result conflict;
	enum edge_result result = native_insert_preserves_siblings(self->fd,
							   &conflict);

	if (result == EDGE_UNSUPPORTED)
		SKIP(return, "vm_insert_pages unavailable");
	EXPECT_EQ(EDGE_PASS, conflict);
}

P3S_HARNESS_COMPAT_MAIN
