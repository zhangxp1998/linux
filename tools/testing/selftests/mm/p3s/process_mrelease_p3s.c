// SPDX-License-Identifier: GPL-2.0
/*
 * process_mrelease() from a native (16K) caller on a killed 4K compat child
 * succeeds and zaps the child's resident anonymous VMA.
 */
#define _GNU_SOURCE

#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

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

static int run_compat_child(int ready_fd)
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

struct release_outcome {
	struct child_status child;
	unsigned long rss_before, rss_after;
	bool child_ready, before_ok, after_ok;
	bool pidfd_opened, release_ok;
	bool affinity_set, affinity_restored, scheduler_restored;
	int release_errno;
};

static struct release_outcome observe_release(void)
{
	struct release_outcome outcome = {};
	cpu_set_t saved_cpus, one_cpu;
	struct sched_param realtime = { .sched_priority = 1 };
	struct sched_param normal = {};
	int cpu, ready_pipe[2] = { -1, -1 }, pidfd = -1;
	char ready_fd[16];
	pid_t pid = -1;
	bool realtime_set = false;

	outcome.scheduler_restored = true;
	if (sched_getaffinity(0, sizeof(saved_cpus), &saved_cpus))
		return outcome;
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++)
		if (CPU_ISSET(cpu, &saved_cpus))
			break;
	if (cpu == CPU_SETSIZE)
		return outcome;
	CPU_ZERO(&one_cpu);
	CPU_SET(cpu, &one_cpu);
	if (sched_setaffinity(0, sizeof(one_cpu), &one_cpu))
		return outcome;
	outcome.affinity_set = true;
	if (pipe(ready_pipe))
		goto out;
	pid = fork();
	if (pid < 0)
		goto out;
	if (!pid) {
		close(ready_pipe[0]);
		snprintf(ready_fd, sizeof(ready_fd), "%d", ready_pipe[1]);
		p3s_execl(true, NULL, "--child", ready_fd, NULL);
		_exit(127);
	}
	close(ready_pipe[1]); ready_pipe[1] = -1;
	outcome.child_ready = read_full(ready_pipe[0], &outcome.child,
						 sizeof(outcome.child));
	close(ready_pipe[0]); ready_pipe[0] = -1;
	if (!outcome.child_ready)
		goto out;
	outcome.before_ok = read_smaps_rss(pid, outcome.child.address,
						 &outcome.rss_before);
	pidfd = syscall(SYS_pidfd_open, pid, 0);
	outcome.pidfd_opened = pidfd >= 0;
	if (pidfd < 0)
		goto out;
	if (sched_setscheduler(0, SCHED_FIFO, &realtime))
		goto out;
	realtime_set = true;
	if (kill(pid, SIGKILL)) {
		outcome.release_errno = errno;
	} else {
		outcome.release_ok = syscall(SYS_process_mrelease, pidfd, 0) == 0;
		outcome.release_errno = errno;
	}
	outcome.after_ok = read_smaps_rss(pid, outcome.child.address,
						 &outcome.rss_after);
 out:
	if (realtime_set)
		outcome.scheduler_restored =
			!sched_setscheduler(0, SCHED_OTHER, &normal);
	if (pid > 0) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
	}
	if (pidfd >= 0)
		close(pidfd);
	if (ready_pipe[0] >= 0)
		close(ready_pipe[0]);
	if (ready_pipe[1] >= 0)
		close(ready_pipe[1]);
	if (outcome.affinity_set)
		outcome.affinity_restored =
			!sched_setaffinity(0, sizeof(saved_cpus), &saved_cpus);
	return outcome;
}

FIXTURE(release) {
	struct release_outcome result;
};

FIXTURE_SETUP(release)
{
	self->result = observe_release();
	ASSERT_TRUE(self->result.scheduler_restored);
	ASSERT_TRUE(self->result.affinity_restored);
}

FIXTURE_TEARDOWN(release)
{
}

TEST_F(release, compat_child_starts)
{
	EXPECT_TRUE(self->result.child_ready);
}

TEST_F(release, child_uses_4k_pages)
{
	ASSERT_TRUE(self->result.child_ready);
	EXPECT_EQ(PROCESS_PAGE_SIZE, self->result.child.page_size);
}

TEST_F(release, child_maps_isolated_anonymous_vma)
{
	ASSERT_TRUE(self->result.child_ready);
	EXPECT_TRUE(self->result.child.mapped && self->result.child.isolated);
}

TEST_F(release, child_vma_resident_before_release)
{
	EXPECT_TRUE(self->result.before_ok);
	EXPECT_EQ(MAPPING_SIZE, self->result.rss_before);
}

TEST_F(release, open_child_pidfd)
{
	EXPECT_TRUE(self->result.pidfd_opened);
}

TEST_F(release, process_mrelease_succeeds)
{
	EXPECT_TRUE(self->result.release_ok)
		TH_LOG("process_mrelease errno=%d", self->result.release_errno);
}

TEST_F(release, process_mrelease_zaps_child_vma)
{
	EXPECT_TRUE(self->result.after_ok);
	EXPECT_EQ(0, self->result.rss_after);
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (!mode)
		p3s_require_native_compat();
	if (!mode)
		exec_native(argv[0], "--parent", NULL);
	if (argc == 2 && !strcmp(mode, "--parent"))
		return p3s_harness_run();
	if (argc == 3 && !strcmp(mode, "--child"))
		return run_compat_child(atoi(argv[2]));
	return EXIT_FAILURE;
}
