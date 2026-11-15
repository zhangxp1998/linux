// SPDX-License-Identifier: GPL-2.0
/*
 * A forced MGLRU page-table aging pass over a 4K compat process's anonymous
 * mapping, driven through debugfs lru_gen in a dedicated memory cgroup,
 * walks the 4K PTEs and advances both the generation and its anon count.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define TEST_PAGES 512
#define CGROUP_ROOT "/sys/fs/cgroup"
#define LRU_GEN_ENABLED_PATH "/sys/kernel/mm/lru_gen/enabled"
#define LRU_GEN_PATH "/sys/kernel/debug/lru_gen"

struct generation_stats {
	unsigned long memcg_id;
	int node_id;
	int max_gen;
	long target_anon;
};

static bool read_text_file(const char *path, char *buf, size_t size)
{
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n <= 0)
		return false;
	buf[n] = '\0';
	return true;
}

FIXTURE(mglru) {
	char name[64];
	char path[128];
	char saved_enabled[32];
	bool created;
	bool restore_enabled;
};

FIXTURE_SETUP(mglru)
{
}

FIXTURE_TEARDOWN_PARENT(mglru)
{
	if (self->created)
		EXPECT_EQ(0, rmdir(self->path));
	if (self->restore_enabled)
		EXPECT_EQ(0, p3s_write_text(LRU_GEN_ENABLED_PATH,
					    self->saved_enabled));
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
		     (long)getpid()) >= (int)sizeof(self->name) ||
	    snprintf(self->path, sizeof(self->path), CGROUP_ROOT "/%s",
		     self->name) >= (int)sizeof(self->path))
		return false;
	if (mkdir(self->path, 0755))
		return false;
	self->created = true;
	snprintf(pid, sizeof(pid), "%d\n", getpid());
	snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", self->path);
	return !p3s_write_text(procs_path, pid);
}

static bool read_generation_stats(struct generation_stats *stats,
				  const char *cgroup_name, int target_gen)
{
	char line[512];
	bool in_memcg = false;
	bool in_node = false;
	FILE *file;

	memset(stats, 0, sizeof(*stats));
	stats->max_gen = -1;
	stats->node_id = -1;
	stats->target_anon = -1;
	file = fopen(LRU_GEN_PATH, "re");
	if (!file)
		return false;

	while (fgets(line, sizeof(line), file)) {
		unsigned long memcg_id;
		long age, anon, file_pages;
		int node, gen;

		if (sscanf(line, " memcg %lu", &memcg_id) == 1) {
			in_memcg = strstr(line, cgroup_name) != NULL;
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
		if (gen > stats->max_gen) {
			stats->max_gen = gen;
			if (target_gen < 0)
				stats->target_anon = anon;
		}
		if (gen == target_gen)
			stats->target_anon = anon;
	}
	fclose(file);
	return stats->memcg_id && stats->node_id == 0 && stats->max_gen >= 0;
}

static bool age_memcg(const struct generation_stats *stats)
{
	char command[128];

	snprintf(command, sizeof(command), "+ %lu %d %d 1 1\n",
		 stats->memcg_id, stats->node_id, stats->max_gen);
	return !p3s_write_text(LRU_GEN_PATH, command);
}

TEST_F(mglru, page_table_aging_creates_new_generation)
{
	struct generation_stats initial, before, after;
	unsigned char *mapping;
	unsigned long val;
	int i;

	if (geteuid())
		SKIP(return, "root is required for MGLRU debugfs aging");
	if (access(LRU_GEN_PATH, R_OK | W_OK))
		SKIP(return, "MGLRU debugfs control is unavailable: %s",
		     strerror(errno));
	if (!memory_controller_enabled())
		SKIP(return, "memory cgroup controller is not enabled");
	if (!read_text_file(LRU_GEN_ENABLED_PATH, self->saved_enabled,
			    sizeof(self->saved_enabled)))
		SKIP(return, "MGLRU enabled sysfs control is unavailable");
	val = strtoul(self->saved_enabled, NULL, 0);
	if ((val & 0x7) != 0x7) {
		if (p3s_write_text(LRU_GEN_ENABLED_PATH, "0x7\n"))
			SKIP(return, "cannot enable MGLRU walks: %s",
			     strerror(errno));
		self->restore_enabled = true;
	}

	ASSERT_TRUE(join_test_cgroup(self));

	mapping = mmap(NULL, TEST_PAGES * PROCESS_PAGE_SIZE,
		       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
		       -1, 0);
	ASSERT_NE(MAP_FAILED, mapping);
	madvise(mapping, TEST_PAGES * PROCESS_PAGE_SIZE, MADV_NOHUGEPAGE);
	for (i = 0; i < TEST_PAGES; i++)
		mapping[i * PROCESS_PAGE_SIZE] = (unsigned char)i;
	/* Flush folio batch to LRU and clear young PTEs via an initial age pass. */
	madvise(mapping, TEST_PAGES * PROCESS_PAGE_SIZE, MADV_COLD);
	ASSERT_TRUE(read_generation_stats(&initial, self->name, -1));
	ASSERT_TRUE(age_memcg(&initial));

	/*
	 * MGLRU sets PG_referenced without promoting on a folio's first young
	 * PTE walk; run one warm-up touch-and-age pass so the measured pass
	 * promotes referenced 4K-mapped folios into before.max_gen.
	 */
	ASSERT_TRUE(read_generation_stats(&before, self->name, -1));
	for (i = 0; i < TEST_PAGES; i++)
		mapping[i * PROCESS_PAGE_SIZE] =
			FORCE_READ(mapping[i * PROCESS_PAGE_SIZE]) ^ 1;
	ASSERT_TRUE(age_memcg(&before));

	ASSERT_TRUE(read_generation_stats(&before, self->name, -1));
	for (i = 0; i < TEST_PAGES; i++)
		mapping[i * PROCESS_PAGE_SIZE] =
			FORCE_READ(mapping[i * PROCESS_PAGE_SIZE]) ^ 1;

	ASSERT_TRUE(age_memcg(&before));
	ASSERT_TRUE(read_generation_stats(&after, self->name, before.max_gen));
	TH_LOG("gen %d->%d anon at gen %d: %ld->%ld",
	       before.max_gen, after.max_gen, before.max_gen,
	       before.target_anon, after.target_anon);
	EXPECT_GT(after.max_gen, before.max_gen);
	EXPECT_GE(after.target_anon,
		  before.target_anon + TEST_PAGES / (2 * P3S_SLICES));

	munmap(mapping, TEST_PAGES * PROCESS_PAGE_SIZE);
}

P3S_HARNESS_4KB_MAIN
