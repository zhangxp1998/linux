// SPDX-License-Identifier: GPL-2.0
/* Private file COW preserves 4KB slices, file contents and fork isolation. */
#define _GNU_SOURCE

#include <signal.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "p3s_slice_test.h"
#include "p3s_harness.h"

#define PTE_SPAN (512UL * PROCESS_PAGE_SIZE)

FIXTURE(file_cow) {
	unsigned char *map, *reservation, *expected;
	size_t reservation_size;
	int fd, second_fd;
	pid_t child;
};

FIXTURE_SETUP(file_cow)
{
	unsigned char page[PROCESS_PAGE_SIZE];
	unsigned int i;

	self->fd = self->second_fd = -1;
	self->map = self->reservation = MAP_FAILED;
	self->child = -1;
	self->fd = memfd_create("p3s-file-cow", MFD_CLOEXEC);
	ASSERT_GE(self->fd, 0);
	for (i = 0; i < 3 * P3S_SLICES; i++) {
		memset(page, 0x20 + i, sizeof(page));
		ASSERT_EQ(sizeof(page), pwrite(self->fd, page, sizeof(page),
					       i * PROCESS_PAGE_SIZE));
	}
	self->map = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE, self->fd, 0);
	ASSERT_NE(MAP_FAILED, self->map);
}

FIXTURE_TEARDOWN(file_cow)
{
	if (self->child > 0) {
		kill(self->child, SIGKILL);
		waitpid(self->child, NULL, 0);
	}
	if (self->reservation != MAP_FAILED)
		munmap(self->reservation, self->reservation_size);
	if (self->map != MAP_FAILED)
		munmap(self->map, NATIVE_PAGE_SIZE);
	if (self->second_fd >= 0)
		close(self->second_fd);
	if (self->fd >= 0)
		close(self->fd);
	free(self->expected);
}

static void expect_slices(struct __test_metadata *_metadata,
			  const unsigned char *map, unsigned char first)
{
	unsigned int i;

	for (i = 0; i < P3S_SLICES; i++)
		EXPECT_EQ(first + i, map[i * PROCESS_PAGE_SIZE]);
}

static void write_slices(unsigned char *map, unsigned char first)
{
	unsigned int i;

	for (i = 0; i < P3S_SLICES; i++)
		map[i * PROCESS_PAGE_SIZE] = first + i;
}

TEST_F(file_cow, faults_preserve_all_other_bytes)
{
	const unsigned int order[] = { 3, 1, 0, 2 };
	unsigned int step, i, written = 0;
	unsigned char byte;
	size_t pos;

	for (step = 0; step < ARRAY_SIZE(order); step++) {
		unsigned int slice = order[step];

		self->map[slice * PROCESS_PAGE_SIZE] = 0x70 + slice;
		written |= 1U << slice;
		for (pos = 0; pos < NATIVE_PAGE_SIZE; pos++) {
			i = pos / PROCESS_PAGE_SIZE;
			byte = (pos % PROCESS_PAGE_SIZE == 0 &&
				(written & (1U << i))) ? 0x70 + i : 0x20 + i;
			ASSERT_EQ(byte, self->map[pos]) TH_LOG("offset=%zu", pos);
		}
	}
	for (i = 0; i < P3S_SLICES; i++) {
		ASSERT_EQ(1, pread(self->fd, &byte, 1, i * PROCESS_PAGE_SIZE));
		EXPECT_EQ(0x20 + i, byte);
	}
}

TEST_F(file_cow, mixed_file_and_anonymous_accounting)
{
	unsigned long anonymous;

	expect_slices(_metadata, self->map, 0x20);
	self->map[PROCESS_PAGE_SIZE] = 0x61;
	self->map[0] = 0x60;
	ASSERT_TRUE(p3s_smaps_bytes(self->map, NATIVE_PAGE_SIZE,
				    "Anonymous", &anonymous));
	EXPECT_EQ(2 * PROCESS_PAGE_SIZE, anonymous);
	EXPECT_EQ(0x60, self->map[0]);
	EXPECT_EQ(0x61, self->map[PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0x22, self->map[2 * PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0x23, self->map[3 * PROCESS_PAGE_SIZE]);
}

TEST_F(file_cow, fork_write_preserves_parent)
{
	int status;

	write_slices(self->map, 0x40);
	self->child = fork();
	ASSERT_GE(self->child, 0);
	if (!self->child) {
		self->map[2 * PROCESS_PAGE_SIZE] = 0x7a;
		_exit(self->map[0] == 0x40 &&
		      self->map[2 * PROCESS_PAGE_SIZE] == 0x7a ? 0 : 1);
	}
	ASSERT_EQ(self->child, waitpid(self->child, &status, 0));
	self->child = -1;
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));
	expect_slices(_metadata, self->map, 0x40);
}

static unsigned char *offset_mapping(struct __test_metadata *_metadata,
				     FIXTURE_DATA(file_cow) *self, bool cross_pte)
{
	unsigned char *slices, *mapping;
	uintptr_t boundary;

	self->reservation_size = 2 * PTE_SPAN;
	self->reservation = mmap(NULL, self->reservation_size, PROT_NONE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, self->reservation);
	boundary = ((uintptr_t)self->reservation + 2 * NATIVE_PAGE_SIZE +
		    PTE_SPAN - 1) & ~(PTE_SPAN - 1);
	slices = (unsigned char *)(cross_pte ? boundary - 2 * PROCESS_PAGE_SIZE :
					      boundary + NATIVE_PAGE_SIZE);
	mapping = slices - 3 * PROCESS_PAGE_SIZE;
	ASSERT_EQ(mapping, mmap(mapping, 2 * NATIVE_PAGE_SIZE,
				PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED,
				self->fd, PROCESS_PAGE_SIZE));
	return slices;
}

TEST_F(file_cow, nonzero_file_offset_preserves_unwritten_slices)
{
	unsigned char *slices = offset_mapping(_metadata, self, false);

	expect_slices(_metadata, slices, 0x24);
	write_slices(slices, 0x50);
	expect_slices(_metadata, slices, 0x50);
}

TEST_F(file_cow, pte_boundary_falls_back_without_file_corruption)
{
	unsigned char *slices = offset_mapping(_metadata, self, true);
	unsigned long anonymous;
	unsigned char byte;
	unsigned int i;

	expect_slices(_metadata, slices, 0x24);
	ASSERT_TRUE(p3s_smaps_bytes(slices, NATIVE_PAGE_SIZE, "Anonymous",
				    &anonymous));
	EXPECT_EQ(0, anonymous);
	write_slices(slices, 0x68);
	expect_slices(_metadata, slices, 0x68);
	ASSERT_TRUE(p3s_smaps_bytes(slices, NATIVE_PAGE_SIZE, "Anonymous",
				    &anonymous));
	EXPECT_EQ(NATIVE_PAGE_SIZE, anonymous);
	for (i = 0; i < P3S_SLICES; i++) {
		ASSERT_EQ(1, pread(self->fd, &byte, 1,
				   (P3S_SLICES + i) * PROCESS_PAGE_SIZE));
		EXPECT_EQ(0x24 + i, byte);
	}
}

TEST_F(file_cow, split_vma_retains_unwritten_file_offsets)
{
	unsigned int i;

	self->map[0] = 0x51;
	ASSERT_EQ(0, mprotect(self->map + PROCESS_PAGE_SIZE,
			      PROCESS_PAGE_SIZE, PROT_READ));
	ASSERT_EQ(0, mprotect(self->map + PROCESS_PAGE_SIZE,
			      PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE));
	EXPECT_EQ(0x51, self->map[0]);
	for (i = 1; i < P3S_SLICES; i++)
		EXPECT_EQ(0x20 + i, self->map[i * PROCESS_PAGE_SIZE]);
	write_slices(self->map, 0x51);
	expect_slices(_metadata, self->map, 0x51);
}

TEST_F(file_cow, pageout_refaults_private_slices)
{
	if (!p3s_swap_available())
		SKIP(return, "no active swap");
	write_slices(self->map, 0x30);
	ASSERT_TRUE(pageout_reaches_swap(self->map, NATIVE_PAGE_SIZE,
					 NATIVE_PAGE_SIZE));
	expect_slices(_metadata, self->map, 0x30);
}

TEST_F(file_cow, hole_punch_keeps_private_copy)
{
	self->map[PROCESS_PAGE_SIZE] = 0x6a;
	ASSERT_EQ(0, fallocate(self->fd, FALLOC_FL_PUNCH_HOLE |
			      FALLOC_FL_KEEP_SIZE, 0, NATIVE_PAGE_SIZE));
	EXPECT_EQ(0, *(volatile unsigned char *)self->map);
	EXPECT_EQ(0x6a, self->map[PROCESS_PAGE_SIZE]);
}

static void expect_sigbus(struct __test_metadata *_metadata,
			  FIXTURE_DATA(file_cow) *self, unsigned char *address)
{
	int status;

	self->child = fork();
	ASSERT_GE(self->child, 0);
	if (!self->child) {
		(void)*(volatile unsigned char *)address;
		_exit(0);
	}
	ASSERT_EQ(self->child, waitpid(self->child, &status, 0));
	self->child = -1;
	ASSERT_TRUE(WIFSIGNALED(status));
	EXPECT_EQ(SIGBUS, WTERMSIG(status));
}

TEST_F(file_cow, truncate_drops_file_and_private_ptes)
{
	(void)*(volatile unsigned char *)self->map;
	self->map[PROCESS_PAGE_SIZE] = 0x6a;
	ASSERT_EQ(0, ftruncate(self->fd, 0));
	expect_sigbus(_metadata, self, self->map);
	expect_sigbus(_metadata, self, self->map + PROCESS_PAGE_SIZE);
}

TEST_F(file_cow, separate_file_descriptions_keep_private_copies)
{
	unsigned char *base, byte;
	char path[64];

	snprintf(path, sizeof(path), "/proc/self/fd/%d", self->fd);
	self->second_fd = open(path, O_RDWR | O_CLOEXEC);
	ASSERT_GE(self->second_fd, 0);
	self->reservation_size = 2 * NATIVE_PAGE_SIZE;
	base = map_aligned(NATIVE_PAGE_SIZE, &self->reservation);
	ASSERT_NE(MAP_FAILED, base);
	ASSERT_EQ(base, mmap(base, 2 * PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_FIXED, self->fd, 0));
	ASSERT_EQ(base + 2 * PROCESS_PAGE_SIZE,
		  mmap(base + 2 * PROCESS_PAGE_SIZE, 2 * PROCESS_PAGE_SIZE,
		       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED,
		       self->second_fd, 2 * PROCESS_PAGE_SIZE));
	expect_slices(_metadata, base, 0x20);
	write_slices(base, 0x40);
	expect_slices(_metadata, base, 0x40);
	ASSERT_EQ(1, pread(self->fd, &byte, 1, 0));
	EXPECT_EQ(0x20, byte);
	ASSERT_EQ(1, pread(self->second_fd, &byte, 1, 2 * PROCESS_PAGE_SIZE));
	EXPECT_EQ(0x22, byte);
}

TEST_F(file_cow, non_head_pagecache_source_preserves_bytes)
{
	static const char * const candidates[] = {
		"/system/bin/linker64", "/system/bin/sh", "/bin/sh", "/proc/self/exe",
	};
	struct stat st;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(candidates); i++) {
		self->second_fd = open(candidates[i], O_RDONLY | O_CLOEXEC);
		if (self->second_fd >= 0 && !fstat(self->second_fd, &st) &&
		    st.st_size >= (off_t)(2 * NATIVE_PAGE_SIZE))
			break;
		if (self->second_fd >= 0)
			close(self->second_fd);
		self->second_fd = -1;
	}
	if (self->second_fd < 0)
		SKIP(return, "no file large enough for a non-head page");
	self->reservation_size = NATIVE_PAGE_SIZE;
	self->reservation = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE, self->second_fd, NATIVE_PAGE_SIZE);
	ASSERT_NE(MAP_FAILED, self->reservation);
	self->expected = malloc(NATIVE_PAGE_SIZE);
	ASSERT_NE(NULL, self->expected);
	memcpy(self->expected, self->reservation, NATIVE_PAGE_SIZE);
	for (i = 0; i < P3S_SLICES; i++) {
		unsigned long pos = i * PROCESS_PAGE_SIZE;

		self->expected[pos] ^= 0x5a;
		self->reservation[pos] = self->expected[pos];
		EXPECT_EQ(0, memcmp(self->reservation, self->expected,
				    NATIVE_PAGE_SIZE)) TH_LOG("slice=%u", i);
	}
}

TEST_F(file_cow, ptrace_forced_writes_preserve_file)
{
	const unsigned long first = 0x5151515151515151UL;
	const unsigned long second = 0x6262626262626262UL;
	unsigned char byte;
	int status;

	ASSERT_EQ(0, mprotect(self->map, NATIVE_PAGE_SIZE, PROT_READ));
	expect_slices(_metadata, self->map, 0x20);
	self->child = fork();
	ASSERT_GE(self->child, 0);
	if (!self->child) {
		unsigned long anonymous, got_first, got_second;

		if (ptrace(PTRACE_TRACEME, 0, NULL, NULL))
			_exit(2);
		raise(SIGSTOP);
		memcpy(&got_first, self->map, sizeof(got_first));
		memcpy(&got_second, self->map + PROCESS_PAGE_SIZE, sizeof(got_second));
		_exit(got_first == first && got_second == second &&
		      self->map[2 * PROCESS_PAGE_SIZE] == 0x22 &&
		      self->map[3 * PROCESS_PAGE_SIZE] == 0x23 &&
		      p3s_smaps_bytes(self->map, NATIVE_PAGE_SIZE,
				      "Anonymous", &anonymous) &&
		      anonymous == 2 * PROCESS_PAGE_SIZE ? 0 : 1);
	}
	ASSERT_EQ(self->child, waitpid(self->child, &status, WUNTRACED));
	ASSERT_TRUE(WIFSTOPPED(status));
	ASSERT_EQ(0, ptrace(PTRACE_POKEDATA, self->child, self->map,
			    (void *)(uintptr_t)first));
	ASSERT_EQ(0, ptrace(PTRACE_POKEDATA, self->child,
			    self->map + PROCESS_PAGE_SIZE, (void *)(uintptr_t)second));
	ASSERT_EQ(0, ptrace(PTRACE_CONT, self->child, NULL, NULL));
	ASSERT_EQ(self->child, waitpid(self->child, &status, 0));
	self->child = -1;
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));
	ASSERT_EQ(1, pread(self->fd, &byte, 1, 0));
	EXPECT_EQ(0x20, byte);
	ASSERT_EQ(1, pread(self->fd, &byte, 1, PROCESS_PAGE_SIZE));
	EXPECT_EQ(0x21, byte);
}

P3S_HARNESS_4KB_MAIN
