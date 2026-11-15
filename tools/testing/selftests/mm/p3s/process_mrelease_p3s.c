// SPDX-License-Identifier: GPL-2.0
/*
 * process_mrelease() from a native (16K) caller on a killed 4KB child
 * succeeds and zaps the child's resident anonymous VMA.
 */
#define _GNU_SOURCE

#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define P3S_HARNESS_ROLE child_role
#define P3S_HARNESS_DEFAULT_NATIVE 1
#include "p3s_harness.h"

#define TEST_PAGES	32
#define MAPPING_SIZE	(TEST_PAGES * PROCESS_PAGE_SIZE)
#define RESERVE_SIZE	(MAPPING_SIZE + 2 * PROCESS_PAGE_SIZE)

struct child_status {
	long page_size;
	unsigned long address;
	bool mapped;
	bool isolated;
};

static bool vma_span(const void *address, unsigned long *span)
{
	unsigned long target = (unsigned long)address;
	unsigned long start, end;
	char *line = NULL;
	size_t capacity = 0;
	bool found = false;
	FILE *maps;

	maps = fopen("/proc/self/maps", "re");
	if (!maps)
		return false;
	while (getline(&line, &capacity, maps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) != 2)
			continue;
		if (target >= start && target < end) {
			*span = end - start;
			found = true;
			break;
		}
	}
	free(line);
	fclose(maps);
	return found;
}

/*
 * Rss of the child VMA containing @address; a VMA that is no longer there
 * (the child has been released) reports 0 bytes rather than a failure.
 */
static bool read_smaps_rss(pid_t pid, unsigned long address,
			   unsigned long *rss_bytes)
{
	unsigned long start, end, value_kb;
	char path[64];
	char *line = NULL;
	size_t capacity = 0;
	bool found = false;
	bool in_target = false;
	bool target_seen = false;
	FILE *smaps;

	snprintf(path, sizeof(path), "/proc/%d/smaps", pid);
	smaps = fopen(path, "re");
	if (!smaps)
		return false;
	while (getline(&line, &capacity, smaps) >= 0) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			if (in_target)
				break;
			in_target = address >= start && address < end;
			target_seen |= in_target;
			continue;
		}
		if (in_target && sscanf(line, "Rss: %lu kB", &value_kb) == 1) {
			*rss_bytes = value_kb * 1024;
			found = true;
			break;
		}
	}
	if (!target_seen) {
		*rss_bytes = 0;
		found = true;
	}
	free(line);
	fclose(smaps);
	return found;
}

static int run_4kb_child(int ready_fd)
{
	struct child_status status = {
		.page_size = sysconf(_SC_PAGESIZE),
	};
	unsigned long span = 0;
	unsigned char *mapping;
	unsigned char *reservation;
	unsigned int i;

	reservation = mmap(NULL, RESERVE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	mapping = reservation == MAP_FAILED ? MAP_FAILED :
		mmap(reservation + PROCESS_PAGE_SIZE, MAPPING_SIZE,
		     PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (mapping != MAP_FAILED) {
		madvise(mapping, MAPPING_SIZE, MADV_NOHUGEPAGE);
		for (i = 0; i < TEST_PAGES; i++)
			mapping[i * PROCESS_PAGE_SIZE] = 0x40 + i;
		status.address = (unsigned long)mapping;
		status.mapped = true;
		status.isolated = vma_span(mapping, &span) &&
				  span == MAPPING_SIZE;
	}

	if (!write_full(ready_fd, &status, sizeof(status)))
		return EXIT_FAILURE;
	for (;;)
		pause();
}

FIXTURE(release) {
	struct child_status info;
	pid_t child;
	int pidfd;
	cpu_set_t saved_cpus;
	struct sched_param saved_param;
	int saved_policy;
	bool affinity_changed;
	bool scheduler_changed;
};

FIXTURE_SETUP(release)
{
	int ready[2];

	self->child = -1;
	self->pidfd = -1;
	ASSERT_EQ(0, sched_getaffinity(0, sizeof(self->saved_cpus),
				       &self->saved_cpus));
	self->saved_policy = sched_getscheduler(0);
	ASSERT_GE(self->saved_policy, 0);
	ASSERT_EQ(0, sched_getparam(0, &self->saved_param));
	ASSERT_EQ(0, pipe(ready));
	self->child = fork();
	ASSERT_GE(self->child, 0);
	if (!self->child) {
		char fd[16];

		close(ready[0]);
		snprintf(fd, sizeof(fd), "%d", ready[1]);
		p3s_execl(true, NULL, "--child", fd, NULL);
		_exit(127);
	}
	close(ready[1]);
	bool received = read_full(ready[0], &self->info, sizeof(self->info));

	close(ready[0]);
	if (!received) {
		kill(self->child, SIGKILL);
		waitpid(self->child, NULL, 0);
		self->child = -1;
	}
	ASSERT_TRUE(received);
}

FIXTURE_TEARDOWN(release)
{
	if (self->scheduler_changed)
		EXPECT_EQ(0, sched_setscheduler(0, self->saved_policy,
						&self->saved_param));
	if (self->affinity_changed)
		EXPECT_EQ(0, sched_setaffinity(0, sizeof(self->saved_cpus),
					       &self->saved_cpus));
	if (self->child > 0) {
		kill(self->child, SIGKILL);
		waitpid(self->child, NULL, 0);
	}
	if (self->pidfd >= 0)
		close(self->pidfd);
}

TEST_F(release, native_caller_zaps_4kb_child_mapping)
{
	struct sched_param realtime = { .sched_priority = 1 };
	cpu_set_t one_cpu;
	unsigned long rss;
	int cpu, ret, saved;

	ASSERT_EQ(PROCESS_PAGE_SIZE, self->info.page_size);
	ASSERT_TRUE(self->info.mapped && self->info.isolated);
	ASSERT_TRUE(read_smaps_rss(self->child, self->info.address, &rss));
	ASSERT_EQ(MAPPING_SIZE, rss);
	self->pidfd = syscall(SYS_pidfd_open, self->child, 0);
	ASSERT_GE(self->pidfd, 0);
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &self->saved_cpus))
			break;
	ASSERT_LT(cpu, CPU_SETSIZE);
	CPU_ZERO(&one_cpu);
	CPU_SET(cpu, &one_cpu);
	/* Hold the victim off CPU until the explicit reaper has inspected it. */
	ASSERT_EQ(0, sched_setaffinity(self->child, sizeof(one_cpu), &one_cpu));
	ASSERT_EQ(0, sched_setaffinity(0, sizeof(one_cpu), &one_cpu));
	self->affinity_changed = true;
	ret = sched_setscheduler(0, SCHED_FIFO, &realtime);
	if (ret && errno == EPERM)
		SKIP(return, "SCHED_FIFO needs CAP_SYS_NICE or RLIMIT_RTPRIO");
	ASSERT_EQ(0, ret);
	self->scheduler_changed = true;
	ASSERT_EQ(0, kill(self->child, SIGKILL));
	ret = syscall(SYS_process_mrelease, self->pidfd, 0);
	saved = errno;
	ASSERT_TRUE(read_smaps_rss(self->child, self->info.address, &rss));
	ASSERT_EQ(0, sched_setscheduler(0, self->saved_policy, &self->saved_param));
	self->scheduler_changed = false;
	EXPECT_EQ(0, ret) TH_LOG("process_mrelease errno=%d", saved);
	EXPECT_EQ(0, rss);
}

static int child_role(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--child"))
		return run_4kb_child(atoi(argv[2]));
	return -1;
}

P3S_HARNESS_4KB_MAIN
