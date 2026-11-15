// SPDX-License-Identifier: GPL-2.0
/*
 * A forced MGLRU page-table aging pass over a 4K compat process's anonymous
 * mapping, driven through debugfs lru_gen in a dedicated memory cgroup,
 * produces a new generation.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/stat.h>

#include "kselftest_p3s.h"
#include "p3s_harness.h"

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

FIXTURE(mglru) {
	char name[64];
	char path[128];
	bool created;
};

FIXTURE_SETUP(mglru)
{
}

FIXTURE_TEARDOWN_PARENT(mglru)
{
	if (self->created)
		EXPECT_EQ(0, rmdir(self->path));
}

static bool memory_controller_enabled(void)
{
	char line[128];
	FILE *file = fopen(CGROUP_ROOT "/cgroup.subtree_control", "re");
	bool enabled = false;

	if (!file)
		return false;
	if (fgets(line, sizeof(line), file))
		enabled = strstr(line, "memory") != NULL;
	fclose(file);
	return enabled;
}

static bool join_test_cgroup(struct _test_data_mglru *self)
{
	char pid[32];
	char procs_path[160];

	if (snprintf(self->name, sizeof(self->name), "p3s-mglru-%ld",
		     (long)getpid()) >= sizeof(self->name) ||
	    snprintf(self->path, sizeof(self->path), CGROUP_ROOT "/%s",
		     self->name) >= sizeof(self->path))
		return false;
	if (mkdir(self->path, 0755))
		return false;
	self->created = true;
	snprintf(pid, sizeof(pid), "%d\n", getpid());
	snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", self->path);
	return write_text(procs_path, pid);
}

static bool read_generation_stats(struct generation_stats *stats,
				  const char *cgroup_name)
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
			in_memcg = strstr(line, cgroup_name);
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

TEST_F(mglru, page_table_aging_creates_new_generation)
{
	struct generation_stats before;
	struct generation_stats after;
	unsigned char *mapping;
	bool joined;
	bool have_before;
	bool aged;
	bool have_after;
	int i;

	if (access(LRU_GEN_PATH, R_OK | W_OK))
		SKIP(return, "MGLRU debugfs control is unavailable: %s",
		     strerror(errno));
	if (!memory_controller_enabled())
		SKIP(return, "memory cgroup controller is not enabled");

	joined = join_test_cgroup(self);
	ASSERT_TRUE(joined);

	mapping = mmap(NULL, TEST_PAGES * PROCESS_PAGE_SIZE,
		       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
		       -1, 0);
	if (mapping != MAP_FAILED)
		madvise((void *)mapping, TEST_PAGES * PROCESS_PAGE_SIZE,
			MADV_NOHUGEPAGE);
	ASSERT_NE(MAP_FAILED, mapping);
	for (i = 0; i < TEST_PAGES; i++)
		mapping[i * PROCESS_PAGE_SIZE] = (unsigned char)i;

	have_before = read_generation_stats(&before, self->name);
	ASSERT_TRUE(have_before);

	aged = age_memcg(&before);
	ASSERT_TRUE(aged);
	have_after = read_generation_stats(&after, self->name);
	EXPECT_TRUE(have_after && after.max_gen > before.max_gen);

	munmap((void *)mapping, TEST_PAGES * PROCESS_PAGE_SIZE);
}

P3S_HARNESS_COMPAT_MAIN
