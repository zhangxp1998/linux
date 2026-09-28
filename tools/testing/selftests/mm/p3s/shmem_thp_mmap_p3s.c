// SPDX-License-Identifier: GPL-2.0
/* Huge shmem policy does not impose PMD placement or mappings on P3S. */
#define _GNU_SOURCE

#include <linux/memfd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define MIN_ARM64_PMD_SIZE (2UL * 1024 * 1024)
#define MAPPING_SIZE (64UL * 1024 * 1024)
#define SAMPLE_COUNT 16
#define SHMEM_POLICY_PATH "/sys/kernel/mm/transparent_hugepage/shmem_enabled"

static bool aslr_enabled(void)
{
	int randomize_va_space;
	FILE *file = fopen("/proc/sys/kernel/randomize_va_space", "re");

	if (!file)
		return false;
	if (fscanf(file, "%d", &randomize_va_space) != 1)
		randomize_va_space = 0;
	fclose(file);
	return randomize_va_space > 0;
}

static bool get_shmem_policy(char *policy, size_t policy_size)
{
	char buffer[256];
	char *end;
	char *start;
	size_t length;
	FILE *file;

	file = fopen(SHMEM_POLICY_PATH, "re");
	if (!file)
		return false;
	if (!fgets(buffer, sizeof(buffer), file)) {
		fclose(file);
		return false;
	}
	fclose(file);
	start = strchr(buffer, '[');
	end = start ? strchr(start, ']') : NULL;
	if (!start || !end)
		return false;
	length = end - ++start;
	if (!length || length >= policy_size)
		return false;
	memcpy(policy, start, length);
	policy[length] = '\0';
	return true;
}

static bool set_shmem_policy(const char *policy)
{
	int fd;
	ssize_t length = strlen(policy);
	bool success;

	fd = open(SHMEM_POLICY_PATH, O_WRONLY);
	if (fd < 0)
		return false;
	success = write(fd, policy, length) == length;
	close(fd);
	return success;
}

struct sample {
	uintptr_t address;
	unsigned long pmd_mapped;
};

static bool collect_samples(unsigned int *aligned, unsigned int *pte_only)
{
	unsigned int i;

	*aligned = *pte_only = 0;
	for (i = 0; i < SAMPLE_COUNT; i++) {
		struct sample sample;
		int pipefd[2];
		int status;
		pid_t pid;

		if (pipe(pipefd))
			return false;
		pid = fork();
		if (pid < 0) {
			close(pipefd[0]);
			close(pipefd[1]);
			return false;
		}
		if (!pid) {
			close(pipefd[0]);
			if (pipefd[1] != 3) {
				if (dup2(pipefd[1], 3) < 0)
					_exit(126);
				close(pipefd[1]);
			}
			p3s_execl(true, NULL, "--sample", NULL);
			_exit(127);
		}
		close(pipefd[1]);
		if (read(pipefd[0], &sample, sizeof(sample)) !=
		    sizeof(sample)) {
			close(pipefd[0]);
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			return false;
		}
		close(pipefd[0]);
		if (waitpid(pid, &status, 0) != pid ||
		    !WIFEXITED(status) || WEXITSTATUS(status))
			return false;
		if (!(sample.address & (NATIVE_PAGE_SIZE - 1)))
			(*aligned)++;
		if (!sample.pmd_mapped)
			(*pte_only)++;
	}
	return true;
}

FIXTURE(shmem_policy) {
	char original[32];
	bool restore;
};

FIXTURE_SETUP(shmem_policy)
{
	/* Set the global policy in the test body so parent teardown always runs. */
}

static const char *enable_shmem_policy(struct _test_data_shmem_policy *self)
{
	if (!aslr_enabled())
		return "address randomization is disabled";
	if (!get_shmem_policy(self->original, sizeof(self->original)))
		return "shmem THP policy is unavailable";
	/* The parent restores even if this child fails after a partial write. */
	self->restore = true;
	if (!set_shmem_policy("force"))
		return "could not enable shmem THP policy";
	return NULL;
}

FIXTURE_TEARDOWN_PARENT(shmem_policy)
{
	if (self->restore)
		EXPECT_TRUE(set_shmem_policy(self->original));
}

TEST_F(shmem_policy, default_mapping_stays_native_folio_aligned)
{
	const char *reason = enable_shmem_policy(self);
	unsigned int aligned, pte_only;

	if (reason)
		SKIP(return, "%s", reason);
	ASSERT_TRUE(collect_samples(&aligned, &pte_only));
	TH_LOG("native-folio-aligned memfd mappings: %u/%u", aligned,
	       SAMPLE_COUNT);
	EXPECT_EQ(SAMPLE_COUNT, aligned);
}

TEST_F(shmem_policy, compat_mapping_uses_pte_not_shmem_pmd)
{
	const char *reason = enable_shmem_policy(self);
	unsigned int aligned, pte_only;

	if (reason)
		SKIP(return, "%s", reason);
	ASSERT_TRUE(collect_samples(&aligned, &pte_only));
	EXPECT_EQ(SAMPLE_COUNT, pte_only);
}

static int take_sample(void)
{
	struct sample sample;
	void *mapping;
	int fd;

	fd = memfd_create("shmem-thp-mmap-p3s", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, MAPPING_SIZE))
		return EXIT_FAILURE;
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
		return EXIT_FAILURE;
	*(volatile unsigned char *)mapping = 0x5a;
	if (!p3s_smaps_bytes(mapping, MAPPING_SIZE, "ShmemPmdMapped", &sample.pmd_mapped))
		return EXIT_FAILURE;
	sample.address = (uintptr_t)mapping;
	if (write(3, &sample, sizeof(sample)) != sizeof(sample))
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (!mode)
		exec_compat(argv[0], P3S_RUN_FLAG, NULL);
	if (argc == 2 && !strcmp(mode, P3S_RUN_FLAG))
		return p3s_harness_run();
	if (argc == 2 && !strcmp(mode, "--sample"))
		return take_sample();
	return EXIT_FAILURE;
}
