// SPDX-License-Identifier: GPL-2.0
/* Process-page-offset mappings retain file bytes across huge-page placement. */
#define _GNU_SOURCE

#include <linux/magic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>

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

TEST_F(thp_file, default_placement_obeys_page_geometry)
{
	unsigned long alignment = p3s_is_compat_process() ?
					  NATIVE_PAGE_SIZE : getpagesize();

	EXPECT_EQ(0, (uintptr_t)self->mapping & (alignment - 1));
}

TEST_F(thp_file, mapping_reads_requested_file_slice)
{
	unsigned char expected[64];

	ASSERT_EQ(sizeof(expected), pread(test_fd, expected,
					   sizeof(expected), self->offset));
	EXPECT_EQ(0, memcmp(self->mapping, expected, sizeof(expected)));
}

/* Create the test file at @path and re-exec as a compat process with its fd. */
static void __noreturn exec_compat_run(const char *argv0, const char *path)
{
	static const char * const erofs_files[] = {
		"/system/bin/linker64", "/system/bin/sh",
	};
	char fd_string[32];
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
		/* Android /data is usually F2FS, whose file operations do not
		 * request THP-aligned addresses.  EROFS /system does, and mmap()
		 * need not read the range beyond the file's end for this test.
		 */
		close(fd);
		fd = -1;
		for (unsigned int i = 0; i < ARRAY_SIZE(erofs_files); i++) {
			fd = open(erofs_files[i], O_RDONLY);
			if (fd >= 0 && !fstatfs(fd, &fs) &&
			    fs.f_type == EROFS_SUPER_MAGIC_V1)
				break;
			if (fd >= 0)
				close(fd);
			fd = -1;
		}
		if (fd < 0)
			ksft_exit_skip("no THP-aligned file on /system\n");
	}
	snprintf(fd_string, sizeof(fd_string), "%d", fd);
	if (getpagesize() == NATIVE_PAGE_SIZE)
		exec_compat(argv0, P3S_RUN_FLAG, fd_string, NULL);
	else {
		char *const args[] = { (char *)argv0, P3S_RUN_FLAG, fd_string,
				       NULL };

		execv("/proc/self/exe", args);
		ksft_exit_fail_msg("re-exec failed: %s\n", strerror(errno));
	}
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);
	char path[] = "./thp-mmap-offset-p3s-XXXXXX";
	int fd;

	if (!mode && argc == 1) {
		fd = mkstemp(path);
		if (fd < 0)
			ksft_exit_fail_msg("mkstemp failed: %s\n",
					   strerror(errno));
		close(fd);
		unlink(path);
		exec_compat_run(argv[0], path);
	}
	if (!mode && argc == 2)
		exec_compat_run(argv[0], argv[1]);
	if (mode && argc == 3 && !strcmp(mode, P3S_RUN_FLAG)) {
		int result;

		p3s_require_compat();
		test_fd = atoi(argv[2]);
		result = p3s_harness_run();
		close(test_fd);
		return result;
	}
	return EXIT_FAILURE;
}
