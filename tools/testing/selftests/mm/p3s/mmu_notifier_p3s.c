// SPDX-License-Identifier: GPL-2.0
/*
 * An MGLRU page-table aging pass over a 4K compat process's anonymous
 * mapping invokes the fixture MMU notifier with ranges in whole process
 * pages that never exceed one native page (a packed anonymous tuple).
 */
#define _GNU_SOURCE

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "p3s_test.h"
#include "p3s_harness.h"
#include "mmu_notifier_p3s.h"

#define TEST_PAGES 512
#define CGROUP_ROOT "/sys/fs/cgroup"
#define LRU_GEN_PATH "/sys/kernel/debug/lru_gen"

struct generation_stats {
	unsigned long memcg_id;
	int node_id;
	int max_gen;
};

static bool write_text(const char *path, const char *text)
{
	ssize_t length = strlen(text);
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	bool ok;

	if (fd < 0)
		return false;
	ok = write(fd, text, length) == length;
	close(fd);
	return ok;
}

static bool join_test_cgroup(const char *path, bool *restore_memory,
			     bool *created)
{
	char pid[32];
	char procs[PATH_MAX];
	char subtree[256];
	FILE *file;

	file = fopen(CGROUP_ROOT "/cgroup.subtree_control", "re");
	if (!file)
		return false;
	if (!fgets(subtree, sizeof(subtree), file)) {
		fclose(file);
		return false;
	}
	fclose(file);
	if (!strstr(subtree, "memory")) {
		if (!write_text(CGROUP_ROOT "/cgroup.subtree_control", "+memory\n"))
			return false;
		*restore_memory = true;
	}
	if (mkdir(path, 0755))
		return false;
	*created = true;
	if (snprintf(procs, sizeof(procs), "%s/cgroup.procs", path) >=
	    sizeof(procs))
		return false;
	snprintf(pid, sizeof(pid), "%d\n", getpid());
	return write_text(procs, pid);
}

static bool read_generation_stats(const char *cgroup_name,
				  struct generation_stats *stats)
{
	char line[512];
	bool in_memcg = false;
	bool in_node = false;
	FILE *file;

	memset(stats, 0, sizeof(*stats));
	stats->max_gen = -1;
	stats->node_id = -1;
	file = fopen(LRU_GEN_PATH, "r");
	if (!file)
		return false;

	while (fgets(line, sizeof(line), file)) {
		unsigned long memcg_id;
		long age, anon, file_pages;
		int node, gen;

		if (sscanf(line, " memcg %lu", &memcg_id) == 1) {
			in_memcg = !!strstr(line, cgroup_name);
			in_node = false;
			if (in_memcg)
				stats->memcg_id = memcg_id;
			continue;
		}
		if (!in_memcg)
			continue;
		if (sscanf(line, " node %d", &node) == 1) {
			in_node = node == 0;
			if (in_node)
				stats->node_id = node;
			continue;
		}
		if (!in_node)
			continue;
		if (sscanf(line, " %d %ld %ld %ld", &gen, &age, &anon,
			   &file_pages) != 4)
			continue;
		if (gen > stats->max_gen)
			stats->max_gen = gen;
	}
	fclose(file);
	return stats->memcg_id && stats->node_id == 0 && stats->max_gen >= 0;
}

static bool age_memcg(const struct generation_stats *stats)
{
	char command[128];

	snprintf(command, sizeof(command), "+ %lu %d %d 1 1\n",
		 stats->memcg_id, stats->node_id, stats->max_gen);
	return write_text(LRU_GEN_PATH, command);
}

static void reclaim_memcg(const char *path)
{
	char reclaim[PATH_MAX];

	if (snprintf(reclaim, sizeof(reclaim), "%s/memory.reclaim", path) <
	    sizeof(reclaim))
		write_text(reclaim, "1M\n");
}

FIXTURE(mmu_notifier) {
	char cgroup_path[PATH_MAX];
	bool created_cgroup;
	bool restore_memory;
};

FIXTURE_SETUP(mmu_notifier)
{
}

/* The test child joins the cgroup; remove it after that child has exited. */
FIXTURE_TEARDOWN_PARENT(mmu_notifier)
{
	if (self->created_cgroup)
		EXPECT_EQ(0, rmdir(self->cgroup_path));
	if (self->restore_memory)
		EXPECT_TRUE(write_text(CGROUP_ROOT "/cgroup.subtree_control",
				       "-memory\n"));
}

TEST_F(mmu_notifier, aging_ranges_are_process_pages_inside_a_native_page)
{
	struct mmu_notifier_p3s_range range;
	struct mmu_notifier_p3s_stats stats = {};
	struct generation_stats generations;
	unsigned char *mapping;
	size_t mapping_size = TEST_PAGES * PROCESS_PAGE_SIZE;
	char child_result = 0;
	char *cgroup_name;
	int pipefd[2];
	pid_t child;
	int fd;
	int status;
	bool aged;

	if (access(LRU_GEN_PATH, R_OK | W_OK))
		SKIP(return, "debugfs lru_gen is unavailable");
	ASSERT_LT(snprintf(self->cgroup_path, sizeof(self->cgroup_path),
			   CGROUP_ROOT "/p3s-mmu-notifier-%d", getpid()),
		  sizeof(self->cgroup_path));
	ASSERT_TRUE(join_test_cgroup(self->cgroup_path,
				     &self->restore_memory,
				     &self->created_cgroup));
	fd = p3s_open_fixture("/dev/mmu_notifier_p3s", O_RDWR);
	if (fd < 0 && p3s_fixture_unavailable(errno))
		SKIP(return, "MMU notifier fixture unavailable");
	ASSERT_GE(fd, 0);

	mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	/* This hint is best-effort, as in the original notifier test. */
	madvise(mapping, mapping_size, MADV_NOHUGEPAGE);
	range = (struct mmu_notifier_p3s_range) {
		.start = (uintptr_t)mapping,
		.end = (uintptr_t)mapping + mapping_size,
		.page_size = PROCESS_PAGE_SIZE,
	};
	ASSERT_EQ(0, ioctl(fd, MMU_NOTIFIER_P3S_SET_RANGE, &range));
	for (int i = 0; i < TEST_PAGES; i++)
		mapping[i * PROCESS_PAGE_SIZE]++;

	cgroup_name = strrchr(self->cgroup_path, '/') + 1;
	ASSERT_TRUE(read_generation_stats(cgroup_name, &generations));
	ASSERT_EQ(0, pipe(pipefd));
	child = fork();
	ASSERT_GE(child, 0);
	if (!child) {
		close(pipefd[0]);
		for (int attempt = 0; attempt < 8; attempt++) {
			if (!age_memcg(&generations))
				break;
			child_result = 1;
			reclaim_memcg(self->cgroup_path);
			if (ioctl(fd, MMU_NOTIFIER_P3S_GET_STATS, &stats))
				break;
			if (stats.callbacks)
				break;
			if (!read_generation_stats(cgroup_name, &generations))
				break;
		}
		if (write(pipefd[1], &child_result, sizeof(child_result)) !=
		    sizeof(child_result))
			child_result = 0;
		close(pipefd[1]);
		_exit(child_result ? 0 : 1);
	}
	close(pipefd[1]);
	aged = read(pipefd[0], &child_result, sizeof(child_result)) ==
		sizeof(child_result) && child_result;
	close(pipefd[0]);
	ASSERT_EQ(child, waitpid(child, &status, 0));
	ASSERT_TRUE(WIFEXITED(status) && !WEXITSTATUS(status));
	ASSERT_TRUE(aged);
	ASSERT_EQ(0, ioctl(fd, MMU_NOTIFIER_P3S_GET_STATS, &stats));
	TH_LOG("total=%llu callbacks=%llu bad=%llu max_span=%llu last=[%#llx,%#llx)",
	       (unsigned long long)stats.total_callbacks,
	       (unsigned long long)stats.callbacks,
	       (unsigned long long)stats.bad_ranges,
	       (unsigned long long)stats.max_span,
	       (unsigned long long)stats.last_start,
	       (unsigned long long)stats.last_end);
	EXPECT_GT(stats.callbacks, 0);
	EXPECT_EQ(0, stats.bad_ranges);
	munmap(mapping, mapping_size);
	close(fd);
}

P3S_HARNESS_COMPAT_MAIN
