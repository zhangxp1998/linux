// SPDX-License-Identifier: GPL-2.0
/* Huge shmem policy must preserve bytes and process-page address hints. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "p3s_test.h"
#include "p3s_harness.h"

#define TEST_LENGTH (64UL * 1024 * 1024)

static int test_fd;

static bool correct_bytes(const unsigned char *mapping)
{
	return mapping[0] == 0x59 &&
	       mapping[TEST_LENGTH - PROCESS_PAGE_SIZE] == 0x6d;
}

FIXTURE(shmem_offset) {
	unsigned char *mapping;
};

FIXTURE_SETUP(shmem_offset)
{
	self->mapping = mmap(NULL, TEST_LENGTH, PROT_READ,
			     MAP_SHARED, test_fd, PROCESS_PAGE_SIZE);
	ASSERT_NE(self->mapping, MAP_FAILED);
}

FIXTURE_TEARDOWN(shmem_offset)
{
	munmap(self->mapping, TEST_LENGTH);
}

TEST_F(shmem_offset, default_placement_uses_native_alignment)
{
	EXPECT_EQ(0, (uintptr_t)self->mapping & (NATIVE_PAGE_SIZE - 1));
}

TEST_F(shmem_offset, process_page_file_offset_keeps_bytes)
{
	EXPECT_TRUE(correct_bytes(self->mapping));
}

TEST_F(shmem_offset, compat_shmem_mapping_does_not_use_pmd)
{
	unsigned long pmd_mapped = ~0UL;

	ASSERT_TRUE(p3s_smaps_bytes(self->mapping, TEST_LENGTH,
				    "ShmemPmdMapped", &pmd_mapped));
	EXPECT_EQ(0, pmd_mapped);
}

static unsigned char *map_with_hint(unsigned char **expected)
{
	size_t reserve_length = TEST_LENGTH + 2 * NATIVE_PAGE_SIZE;
	unsigned char *reservation, *hint, *mapping;

	reservation = mmap(NULL, reserve_length, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return MAP_FAILED;
	hint = (unsigned char *)(((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
				~(NATIVE_PAGE_SIZE - 1)) + PROCESS_PAGE_SIZE;
	munmap(reservation, reserve_length);
	mapping = mmap(hint, TEST_LENGTH, PROT_READ,
		       MAP_SHARED, test_fd, PROCESS_PAGE_SIZE);
	*expected = hint;
	return mapping;
}

TEST(honor_nonfixed_process_page_hint)
{
	unsigned char *expected, *mapping = map_with_hint(&expected);

	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_EQ(expected, mapping);
	munmap(mapping, TEST_LENGTH);
}

TEST(hinted_mapping_keeps_file_bytes)
{
	unsigned char *expected, *mapping = map_with_hint(&expected);

	ASSERT_NE(mapping, MAP_FAILED);
	EXPECT_TRUE(correct_bytes(mapping));
	munmap(mapping, TEST_LENGTH);
}

static int run_parent(char *argv0)
{
	char mount_path[] = "./shmem-mmap-offset-p3s-XXXXXX";
	char file_path[sizeof(mount_path) + 16], fd_string[32];
	unsigned char head4k = 0x59, head16k = 0x5a, tail = 0x6d;
	pid_t child = -1;
	int status = 0, fd = -1, result = EXIT_FAILURE;
	bool mounted = false;

	if (!mkdtemp(mount_path))
		ksft_exit_fail_msg("mkdtemp: %s\n", strerror(errno));
	if (mount("tmpfs", mount_path, "tmpfs", 0, "size=128m,huge=always")) {
		int error = errno;

		rmdir(mount_path);
		ksft_exit_skip("huge=always tmpfs: %s\n", strerror(error));
	}
	mounted = true;
	snprintf(file_path, sizeof(file_path), "%s/test", mount_path);
	fd = open(file_path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0 || ftruncate(fd, NATIVE_PAGE_SIZE + TEST_LENGTH))
		goto out;
	unlink(file_path);
	if (pwrite(fd, &head4k, 1, PROCESS_PAGE_SIZE) != 1 ||
	    pwrite(fd, &head16k, 1, NATIVE_PAGE_SIZE) != 1 ||
	    pwrite(fd, &tail, 1, TEST_LENGTH) != 1)
		goto out;
	snprintf(fd_string, sizeof(fd_string), "%d", fd);
	child = fork();
	if (child < 0)
		goto out;
	if (!child) {
		if (getpagesize() == NATIVE_PAGE_SIZE)
			exec_compat(argv0, P3S_RUN_FLAG, fd_string, NULL);
		else {
			char *const args[] = { argv0, P3S_RUN_FLAG,
					       fd_string, NULL };

			execv("/proc/self/exe", args);
		}
		_exit(EXIT_FAILURE);
	}
	if (waitpid(child, &status, 0) == child && WIFEXITED(status))
		result = WEXITSTATUS(status);
out:
	if (fd >= 0)
		close(fd);
	if (mounted && umount(mount_path))
		result = EXIT_FAILURE;
	if (rmdir(mount_path))
		result = EXIT_FAILURE;
	return result;
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (mode && argc == 3 && !strcmp(mode, P3S_RUN_FLAG)) {
		p3s_require_compat();
		test_fd = atoi(argv[2]);
		return p3s_harness_run();
	}
	if (!mode && argc == 1)
		return run_parent(argv[0]);
	return EXIT_FAILURE;
}
