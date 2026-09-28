// SPDX-License-Identifier: GPL-2.0
/* Huge shmem keeps 4KB file offsets and never maps a 4KB VMA with a PMD. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "p3s_test.h"
#define P3S_HARNESS_LAUNCH launch_harness
#define P3S_HARNESS_SETUP setup_harness
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

TEST_F(shmem_offset, process_page_file_offset_keeps_bytes)
{
	EXPECT_TRUE(correct_bytes(self->mapping));
}

TEST(faulted_pmd_aligned_shmem_uses_pte_mappings)
{
	unsigned long pmd_size = read_pmd_pagesize();
	unsigned long pmd_mapped = ~0UL;
	unsigned char *reservation, *mapping;
	size_t length;
	uintptr_t aligned;

	if (!pmd_size)
		SKIP(return, "PMD THP size is unavailable");
	length = 2 * pmd_size;
	ASSERT_LE(length, TEST_LENGTH);
	reservation = mmap(NULL, length + pmd_size, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, reservation);
	aligned = ((uintptr_t)reservation + pmd_size - 1) & ~(pmd_size - 1);
	mapping = mmap((void *)aligned, length, PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_FIXED, test_fd, 0);
	ASSERT_EQ((void *)aligned, mapping);
	/* Offset zero, PMD alignment and real faults exercise the huge path. */
	for (size_t i = 0; i < length; i += PROCESS_PAGE_SIZE)
		FORCE_READ(mapping[i]);
	ASSERT_TRUE(p3s_smaps_bytes(mapping, length, "ShmemPmdMapped", &pmd_mapped));
	EXPECT_EQ(0, pmd_mapped);
	munmap(reservation, length + pmd_size);
}

static int run_parent(char **argv)
{
	char mount_path[] = "./shmem-mmap-offset-p3s-XXXXXX";
	char file_path[sizeof(mount_path) + 16], fd_string[32];
	unsigned char head4k = 0x59, tail = 0x6d;
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
	    pwrite(fd, &tail, 1, TEST_LENGTH) != 1)
		goto out;
	snprintf(fd_string, sizeof(fd_string), "%d", fd);
	child = fork();
	if (child < 0)
		goto out;
	if (!child) {
		if (setenv("P3S_SHMEM_FD", fd_string, 1))
			_exit(KSFT_FAIL);
		p3s_execv(true, argv);
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

static int launch_harness(int argc, char **argv)
{
	if (!getenv("P3S_SHMEM_FD") && !p3s_harness_listing(argc, argv))
		return run_parent(argv);
	return -1;
}

static void setup_harness(int *argc, char ***argv)
{
	const char *saved = getenv("P3S_SHMEM_FD");

	if (!saved && p3s_harness_listing(*argc, *argv))
		return;
	if (!saved)
		ksft_exit_fail_msg("missing native shmem fixture FD\n");
	test_fd = atoi(saved);
}

P3S_HARNESS_4KB_MAIN
