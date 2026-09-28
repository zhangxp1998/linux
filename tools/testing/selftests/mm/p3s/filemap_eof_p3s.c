// SPDX-License-Identifier: GPL-2.0
/*
 * A shared file mapping in a 4K compat process observes EOF at 4K
 * granularity: the partial final 4K page is readable and the first complete
 * 4K page beyond EOF raises SIGBUS, at file offset zero and at a 4K offset.
 */
#define _GNU_SOURCE

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_harness.h"

#define FILE_SIZE 5000UL

static const char *mount_dir;

static int read_status(const unsigned char *address)
{
	int status;
	pid_t child = fork();

	if (child < 0)
		return -1;
	if (!child)
		_exit(!!*address);
	if (waitpid(child, &status, 0) != child)
		return -1;
	return status;
}

static bool read_succeeds(const unsigned char *address)
{
	int status = read_status(address);

	return status >= 0 && WIFEXITED(status);
}

static bool read_gets_sigbus(const unsigned char *address)
{
	int status = read_status(address);

	return status >= 0 && WIFSIGNALED(status) &&
	       WTERMSIG(status) == SIGBUS;
}

FIXTURE(filemap_eof) {
	char path[256];
	int fd;
};

FIXTURE_SETUP(filemap_eof)
{
	int length;

	self->fd = -1;
	length = snprintf(self->path, sizeof(self->path),
			  "%s/filemap-eof.bin", mount_dir);
	ASSERT_GE(length, 0);
	ASSERT_LT(length, sizeof(self->path));
}

FIXTURE_TEARDOWN(filemap_eof)
{
	if (self->fd >= 0)
		close(self->fd);
	unlink(self->path);
}

static bool prepare_file(FIXTURE_DATA(filemap_eof) *self)
{
	unsigned char marker = 0x5a;

	self->fd = open(self->path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC,
			0600);
	return self->fd >= 0 && !ftruncate(self->fd, FILE_SIZE) &&
		pwrite(self->fd, &marker, 1, 0) == 1 &&
		pwrite(self->fd, &marker, 1, PROCESS_PAGE_SIZE) == 1 &&
		!fsync(self->fd);
}

TEST_F(filemap_eof, first_page_is_readable)
{
	unsigned char *mapping;

	ASSERT_TRUE(prepare_file(self));
	mapping = mmap(NULL, 3 * PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED,
		       self->fd, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_TRUE(read_succeeds(mapping));
	munmap(mapping, 3 * PROCESS_PAGE_SIZE);
}

TEST_F(filemap_eof, partial_final_page_is_readable)
{
	unsigned char *mapping;

	ASSERT_TRUE(prepare_file(self));
	mapping = mmap(NULL, 3 * PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED,
		       self->fd, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_TRUE(read_succeeds(mapping + PROCESS_PAGE_SIZE));
	munmap(mapping, 3 * PROCESS_PAGE_SIZE);
}

TEST_F(filemap_eof, first_page_past_eof_raises_sigbus)
{
	unsigned char *mapping;

	ASSERT_TRUE(prepare_file(self));
	mapping = mmap(NULL, 3 * PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED,
		       self->fd, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_TRUE(read_gets_sigbus(mapping + 2 * PROCESS_PAGE_SIZE));
	munmap(mapping, 3 * PROCESS_PAGE_SIZE);
}

TEST_F(filemap_eof, offset_mapping_observes_4k_eof)
{
	unsigned char *mapping;

	ASSERT_TRUE(prepare_file(self));
	mapping = mmap(NULL, 2 * PROCESS_PAGE_SIZE, PROT_READ, MAP_SHARED,
		       self->fd, PROCESS_PAGE_SIZE);
	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_TRUE(read_succeeds(mapping));
	EXPECT_TRUE(read_gets_sigbus(mapping + PROCESS_PAGE_SIZE));
	munmap(mapping, 2 * PROCESS_PAGE_SIZE);
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (argc == 2 && !mode)
		exec_compat(argv[0], "--compat", argv[1], NULL);
	if (argc == 3 && mode && !strcmp(mode, "--compat")) {
		p3s_require_compat();
		mount_dir = argv[2];
		return p3s_harness_run();
	}
	return EXIT_FAILURE;
}
