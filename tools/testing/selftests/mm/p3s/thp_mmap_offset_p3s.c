// SPDX-License-Identifier: GPL-2.0
/* Process-page-offset mappings retain file bytes across huge-page placement. */
#define _GNU_SOURCE

#include <linux/magic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>

#define P3S_HARNESS_LAUNCH prepare_harness
#define P3S_HARNESS_SETUP setup_harness
#include "p3s_harness.h"

#define THP_SIZE_PATH "/sys/kernel/mm/transparent_hugepage/hpage_pmd_size"

static bool supports_thp_unmapped_area(long type)
{
	return type == EXT4_SUPER_MAGIC || type == EROFS_SUPER_MAGIC_V1 ||
	       type == BTRFS_SUPER_MAGIC || type == XFS_SUPER_MAGIC ||
	       type == FUSE_SUPER_MAGIC;
}

static unsigned long read_thp_size(void)
{
	char buffer[64];
	char *end;
	ssize_t bytes;
	unsigned long size;
	int fd;

	fd = open(THP_SIZE_PATH, O_RDONLY);
	if (fd < 0)
		return 0;
	bytes = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (bytes <= 0)
		return 0;
	buffer[bytes] = '\0';
	errno = 0;
	size = strtoul(buffer, &end, 10);
	if (errno || end == buffer || size < PROCESS_PAGE_SIZE ||
	    (size & (size - 1)))
		return 0;
	return size;
}

static int test_fd;

FIXTURE(thp_file) {
	unsigned long thp_size;
	size_t length;
	off_t offset;
	void *mapping;
};

FIXTURE_SETUP(thp_file)
{
	struct statfs fs;

	self->thp_size = read_thp_size();
	if (!self->thp_size)
		SKIP(return, "PMD THP size is unavailable");
	if (fstatfs(test_fd, &fs) || !supports_thp_unmapped_area(fs.f_type))
		SKIP(return, "test file lacks THP mmap alignment");
	self->length = 2 * self->thp_size;
	self->offset = getpagesize();
	self->mapping = mmap(NULL, self->length, PROT_READ,
			     MAP_PRIVATE, test_fd, self->offset);
	ASSERT_NE(self->mapping, MAP_FAILED);
}

FIXTURE_TEARDOWN(thp_file)
{
	munmap(self->mapping, self->length);
}

TEST_F(thp_file, mapping_reads_requested_file_slice)
{
	unsigned char expected[64];

	ASSERT_EQ(sizeof(expected), pread(test_fd, expected,
					  sizeof(expected), self->offset));
	EXPECT_EQ(0, memcmp(self->mapping, expected, sizeof(expected)));
}

/* Create the native file before the shared main re-execs as a 4K process. */
static int create_fixture(const char *path)
{
	static const char * const fallback_files[] = {
		"/system/bin/linker64", "/system/bin/sh", "/bin/sh",
	};
	unsigned long thp_size;
	struct statfs fs;
	int fd;

	thp_size = read_thp_size();
	if (!thp_size)
		ksft_exit_skip("PMD THP size is unavailable\n");
	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		ksft_exit_fail_msg("open %s failed: %s\n", path,
				   strerror(errno));
	unlink(path);
	if (fstatfs(fd, &fs))
		ksft_exit_fail_msg("fstatfs failed: %s\n", strerror(errno));
	if (supports_thp_unmapped_area(fs.f_type)) {
		if (ftruncate(fd, PROCESS_PAGE_SIZE + 2 * thp_size))
			ksft_exit_fail_msg("ftruncate failed: %s\n", strerror(errno));
	} else {
		/*
		 * Android /data is usually F2FS and Linux TMPDIR can be tmpfs
		 * or 9p, whose file ops do not request THP-aligned addresses.
		 * Try standard binaries on any THP-aligned filesystem; mmap()
		 * need not read past EOF for this test.
		 */
		close(fd);
		fd = -1;
		for (unsigned int i = 0; i < ARRAY_SIZE(fallback_files); i++) {
			fd = open(fallback_files[i], O_RDONLY);
			if (fd >= 0 && !fstatfs(fd, &fs) &&
			    supports_thp_unmapped_area(fs.f_type))
				break;
			if (fd >= 0)
				close(fd);
			fd = -1;
		}
		if (fd < 0)
			ksft_exit_skip("no file on a THP-aligned filesystem\n");
	}
	return fd;
}

static int prepare_harness(int argc, char **argv)
{
	char path[] = "./thp-mmap-offset-p3s-XXXXXX";
	char fd_string[32];
	const char *selected_path = path;
	int fd;

	if (getenv("P3S_THP_FILE_FD") || p3s_harness_listing(argc, argv))
		return -1;
	if (argc == 1 || argv[1][0] == '-') {
		fd = mkstemp(path);
		if (fd < 0)
			ksft_exit_fail_msg("mkstemp failed: %s\n",
					   strerror(errno));
		close(fd);
		unlink(path);
	} else if (argc >= 2) {
		selected_path = argv[1];
	} else {
		ksft_exit_fail_msg("usage: %s [FILE]\n", argv[0]);
	}
	fd = create_fixture(selected_path);
	snprintf(fd_string, sizeof(fd_string), "%d", fd);
	if (setenv("P3S_THP_FILE_FD", fd_string, 1))
		ksft_exit_fail_msg("save file FD: %s\n", strerror(errno));
	return -1;
}

static void close_test_file(void)
{
	close(test_fd);
}

static void setup_harness(int *argc, char ***argv)
{
	const char *saved = getenv("P3S_THP_FILE_FD");

	p3s_harness_take_positional(argc, argv);
	if (!saved && p3s_harness_listing(*argc, *argv))
		return;
	if (!saved)
		ksft_exit_fail_msg("missing THP fixture FD\n");
	test_fd = atoi(saved);
	atexit(close_test_file);
}

P3S_HARNESS_4KB_MAIN
