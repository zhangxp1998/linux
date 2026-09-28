// SPDX-License-Identifier: GPL-2.0
/*
 * A native (16K) reader walks the page tables of a 4K compat child through
 * /proc/<pid>/smaps, pagemap and clear_refs: RSS, Referenced and present
 * PTEs are reported in the child's 4K units and the walks leave the child's
 * page contents intact.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/wait.h>

#include "p3s_test.h"
#define P3S_HARNESS_ROLE child_role
#define P3S_HARNESS_DEFAULT_NATIVE 1
#include "p3s_harness.h"

#define TEST_PAGES	32
#define MAPPING_SIZE	(TEST_PAGES * PROCESS_PAGE_SIZE)
#define RESERVE_SIZE	(MAPPING_SIZE + 2 * PROCESS_PAGE_SIZE)
#define PAGEMAP_ENTRY_SIZE sizeof(uint64_t)

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

static bool read_smaps(pid_t pid, unsigned long address,
			unsigned long *rss_bytes,
			unsigned long *referenced_bytes)
{
	unsigned long start, end, value_kb;
	char path[64];
	char *line = NULL;
	size_t capacity = 0;
	bool found_rss = false;
	bool found_referenced = false;
	bool in_target = false;
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
			continue;
		}
		if (!in_target)
			continue;
		if (sscanf(line, "Rss: %lu kB", &value_kb) == 1) {
			*rss_bytes = value_kb * 1024;
			found_rss = true;
		} else if (sscanf(line, "Referenced: %lu kB", &value_kb) == 1) {
			*referenced_bytes = value_kb * 1024;
			found_referenced = true;
		}
	}
	free(line);
	fclose(smaps);
	return found_rss && found_referenced;
}

static bool count_present_pages(pid_t pid, unsigned long address,
				unsigned int *present)
{
	uint64_t entries[TEST_PAGES];
	char path[64];
	off_t offset;
	ssize_t bytes;
	unsigned int i;
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	offset = (address / PROCESS_PAGE_SIZE) * PAGEMAP_ENTRY_SIZE;
	bytes = pread(fd, entries, sizeof(entries), offset);
	close(fd);
	if (bytes != sizeof(entries))
		return false;
	*present = 0;
	for (i = 0; i < TEST_PAGES; i++) {
		if (entries[i] & PM_PRESENT)
			(*present)++;
	}
	return true;
}

static bool clear_references(pid_t pid)
{
	char path[64];
	int fd;
	bool cleared;

	snprintf(path, sizeof(path), "/proc/%d/clear_refs", pid);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	cleared = write(fd, "1\n", 2) == 2;
	close(fd);
	return cleared;
}

static int run_compat_child(int ready_fd, int command_fd)
{
	struct child_status status = {
		.page_size = sysconf(_SC_PAGESIZE),
	};
	unsigned long span = 0;
	unsigned char *mapping;
	unsigned char *reservation;
	unsigned char command;
	bool preserved = true;
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

	if (!write_full(ready_fd, &status, sizeof(status)) ||
	    !read_full(command_fd, &command, sizeof(command)))
		preserved = false;
	if (mapping == MAP_FAILED) {
		preserved = false;
	} else {
		for (i = 0; i < TEST_PAGES; i++) {
			if (mapping[i * PROCESS_PAGE_SIZE] !=
			    (unsigned char)(0x40 + i)) {
				preserved = false;
				break;
			}
		}
		munmap(reservation, RESERVE_SIZE);
	}
	return preserved ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct walk_outcome {
	struct child_status child;
	unsigned long rss, referenced;
	unsigned long rss_after, referenced_after;
	unsigned int present;
	bool child_ready, smaps_ok, pagemap_ok;
	bool clear_ok, smaps_after_ok, child_waited;
	int child_wait_status;
};

static struct walk_outcome observe_child(void)
{
	struct walk_outcome outcome = {};
	int ready_pipe[2] = { -1, -1 };
	int command_pipe[2] = { -1, -1 };
	unsigned char command = 1;
	char ready_fd[16], command_fd[16];
	pid_t pid = -1;

	if (pipe(ready_pipe) || pipe(command_pipe))
		goto out;
	pid = fork();
	if (pid < 0)
		goto out;
	if (!pid) {
		close(ready_pipe[0]);
		close(command_pipe[1]);
		snprintf(ready_fd, sizeof(ready_fd), "%d", ready_pipe[1]);
		snprintf(command_fd, sizeof(command_fd), "%d", command_pipe[0]);
		p3s_execl(true, NULL, "--child", ready_fd, command_fd, NULL);
		_exit(127);
	}
	close(ready_pipe[1]); ready_pipe[1] = -1;
	close(command_pipe[0]); command_pipe[0] = -1;
	outcome.child_ready = read_full(ready_pipe[0], &outcome.child,
						 sizeof(outcome.child));
	close(ready_pipe[0]); ready_pipe[0] = -1;
	if (outcome.child_ready && outcome.child.mapped) {
		outcome.smaps_ok = read_smaps(pid, outcome.child.address,
					      &outcome.rss,
					      &outcome.referenced);
		outcome.pagemap_ok = count_present_pages(pid,
						     outcome.child.address,
						     &outcome.present);
		outcome.clear_ok = clear_references(pid);
		outcome.smaps_after_ok = read_smaps(pid, outcome.child.address,
						    &outcome.rss_after,
						    &outcome.referenced_after);
	}
	if (command_pipe[1] >= 0)
		write_full(command_pipe[1], &command, sizeof(command));
 out:
	if (ready_pipe[0] >= 0)
		close(ready_pipe[0]);
	if (ready_pipe[1] >= 0)
		close(ready_pipe[1]);
	if (command_pipe[0] >= 0)
		close(command_pipe[0]);
	if (command_pipe[1] >= 0)
		close(command_pipe[1]);
	if (pid > 0) {
		pid_t waited;

		do {
			waited = waitpid(pid, &outcome.child_wait_status, 0);
		} while (waited < 0 && errno == EINTR);
		outcome.child_waited = waited == pid;
	}
	return outcome;
}

TEST(proc_pagewalk_respects_target_geometry)
{
	struct walk_outcome obs = observe_child();

	if (obs.child_waited && WIFEXITED(obs.child_wait_status) &&
	    WEXITSTATUS(obs.child_wait_status) == KSFT_SKIP)
		SKIP(return, "4K child target unavailable");
	ASSERT_TRUE(obs.child_ready);
	EXPECT_EQ(PROCESS_PAGE_SIZE, obs.child.page_size);
	EXPECT_TRUE(obs.child.mapped && obs.child.isolated);
	ASSERT_TRUE(obs.smaps_ok);
	EXPECT_EQ(MAPPING_SIZE, obs.rss);
	EXPECT_EQ(MAPPING_SIZE, obs.referenced);
	ASSERT_TRUE(obs.pagemap_ok);
	EXPECT_EQ(TEST_PAGES, obs.present);
	EXPECT_TRUE(obs.clear_ok);
	ASSERT_TRUE(obs.smaps_after_ok);
	EXPECT_EQ(MAPPING_SIZE, obs.rss_after);
	EXPECT_EQ(0, obs.referenced_after);
	EXPECT_TRUE(obs.child_waited &&
		    WIFEXITED(obs.child_wait_status) &&
		    WEXITSTATUS(obs.child_wait_status) == EXIT_SUCCESS);
}

static int child_role(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "--child"))
		return run_compat_child(atoi(argv[2]), atoi(argv[3]));
	return -1;
}

P3S_HARNESS_4KB_MAIN
