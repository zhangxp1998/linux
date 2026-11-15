// SPDX-License-Identifier: GPL-2.0
/*
 * A native parent triggers MGLRU page-table aging via debugfs lru_gen over a
 * 4K compat child's anonymous mapping and verifies that clear_young /
 * clear_flush_young MMU notifier callbacks span exactly one 4K process page.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "p3s_test.h"
static int mmu_notifier_role(int argc, char **argv);
#define P3S_HARNESS_DEFAULT_NATIVE 1
#define P3S_HARNESS_ROLE mmu_notifier_role
#include "p3s_harness.h"
#include "mmu_notifier_p3s.h"

#define TEST_PAGES 512
#define CGROUP_ROOT "/sys/fs/cgroup"
#define LRU_GEN_ENABLED_PATH "/sys/kernel/mm/lru_gen/enabled"
#define LRU_GEN_PATH "/sys/kernel/debug/lru_gen"

struct generation_stats {
	unsigned long memcg_id;
	int node_id;
	int max_gen;
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
	return !p3s_write_text(LRU_GEN_PATH, command);
}

static void touch_pages(unsigned char *mapping)
{
	int i;

	for (i = 0; i < TEST_PAGES; i++)
		((volatile unsigned char *)mapping)[i * PROCESS_PAGE_SIZE] ^= 1;
}

static int run_child(int argc, char **argv)
{
	const char *cgroup_path = argv[2];
	int ready_fd = atoi(argv[3]);
	int cmd_fd = atoi(argv[4]);
	int stat_fd = atoi(argv[5]);
	struct mmu_notifier_p3s_range range;
	struct mmu_notifier_p3s_stats stats;
	size_t mapping_size = TEST_PAGES * PROCESS_PAGE_SIZE;
	unsigned char *mapping;
	char procs_path[PATH_MAX];
	char pid_text[32];
	char status = 'e';
	char cmd;
	int fd = -1;
	ssize_t n;

	if (argc != 6 || !p3s_is_4kb_process()) {
		status = 's';
		(void)write(ready_fd, &status, 1);
		return KSFT_SKIP;
	}
	if (snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs",
		     cgroup_path) >= (int)sizeof(procs_path)) {
		(void)write(ready_fd, &status, 1);
		return 1;
	}
	snprintf(pid_text, sizeof(pid_text), "%d\n", getpid());
	if (p3s_write_text(procs_path, pid_text)) {
		(void)write(ready_fd, &status, 1);
		return 1;
	}
	fd = p3s_open_fixture("/dev/mmu_notifier_p3s", O_RDWR);
	if (fd < 0) {
		status = p3s_fixture_unavailable(errno) ? 's' : 'e';
		(void)write(ready_fd, &status, 1);
		return status == 's' ? KSFT_SKIP : 1;
	}
	mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		close(fd);
		(void)write(ready_fd, &status, 1);
		return 1;
	}
	madvise(mapping, mapping_size, MADV_NOHUGEPAGE);
	touch_pages(mapping);
	/* Flush newly faulted folios to the LRU, then restore young PTEs. */
	madvise(mapping, mapping_size, MADV_COLD);
	touch_pages(mapping);

	range = (struct mmu_notifier_p3s_range) {
		.start = (uintptr_t)mapping,
		.end = (uintptr_t)mapping + mapping_size,
		.page_size = PROCESS_PAGE_SIZE,
	};
	if (ioctl(fd, MMU_NOTIFIER_P3S_SET_RANGE, &range)) {
		munmap(mapping, mapping_size);
		close(fd);
		(void)write(ready_fd, &status, 1);
		return 1;
	}
	status = 'r';
	if (!write_full(ready_fd, &status, 1)) {
		munmap(mapping, mapping_size);
		close(fd);
		return 1;
	}
	while ((n = read(cmd_fd, &cmd, 1)) == 1) {
		if (cmd == 'q')
			break;
		if (cmd == 't') {
			char ack = 'k';

			touch_pages(mapping);
			if (!write_full(ready_fd, &ack, 1))
				break;
		} else if (cmd == 'g') {
			if (ioctl(fd, MMU_NOTIFIER_P3S_GET_STATS, &stats) ||
			    !write_full(stat_fd, &stats, sizeof(stats)))
				break;
		}
	}
	munmap(mapping, mapping_size);
	close(fd);
	return 0;
}

static int mmu_notifier_role(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--child"))
		return run_child(argc, argv);
	return -1;
}

FIXTURE(mmu_notifier) {
	char cgroup_name[64];
	char cgroup_path[PATH_MAX];
	char saved_enabled[32];
	bool created_cgroup;
	bool restore_enabled;
};

FIXTURE_SETUP(mmu_notifier)
{
}

/* The 4KB child joins the cgroup; remove it after the child exits. */
FIXTURE_TEARDOWN_PARENT(mmu_notifier)
{
	if (self->created_cgroup)
		EXPECT_EQ(0, rmdir(self->cgroup_path));
	if (self->restore_enabled)
		EXPECT_EQ(0, p3s_write_text(LRU_GEN_ENABLED_PATH,
					    self->saved_enabled));
}

TEST_F(mmu_notifier, aging_ranges_are_process_pages_inside_a_native_page)
{
	struct mmu_notifier_p3s_stats stats = {};
	struct generation_stats generations;
	int ready[2], cmd[2], stat_pipe[2];
	char ready_fd_arg[16], cmd_fd_arg[16], stat_fd_arg[16];
	char state = 0;
	unsigned long val;
	pid_t child;
	int status;

	if (geteuid())
		SKIP(return, "root is required for MGLRU debugfs aging");
	if (access(LRU_GEN_PATH, R_OK | W_OK))
		SKIP(return, "debugfs lru_gen is unavailable");
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

	ASSERT_LT(snprintf(self->cgroup_name, sizeof(self->cgroup_name),
			   "p3s-mmu-notifier-%ld", (long)getpid()),
		  (int)sizeof(self->cgroup_name));
	ASSERT_LT(snprintf(self->cgroup_path, sizeof(self->cgroup_path),
			   CGROUP_ROOT "/%s", self->cgroup_name),
		  (int)sizeof(self->cgroup_path));
	ASSERT_EQ(0, mkdir(self->cgroup_path, 0755));
	self->created_cgroup = true;

	ASSERT_EQ(0, pipe(ready));
	ASSERT_EQ(0, pipe(cmd));
	ASSERT_EQ(0, pipe(stat_pipe));
	child = fork();
	ASSERT_GE(child, 0);
	if (!child) {
		close(ready[0]);
		close(cmd[1]);
		close(stat_pipe[0]);
		snprintf(ready_fd_arg, sizeof(ready_fd_arg), "%d", ready[1]);
		snprintf(cmd_fd_arg, sizeof(cmd_fd_arg), "%d", cmd[0]);
		snprintf(stat_fd_arg, sizeof(stat_fd_arg), "%d", stat_pipe[1]);
		p3s_execl(true, NULL, "--child", self->cgroup_path,
			  ready_fd_arg, cmd_fd_arg, stat_fd_arg, NULL);
		state = 's';
		(void)write(ready[1], &state, 1);
		_exit(KSFT_SKIP);
	}
	close(ready[1]);
	close(cmd[0]);
	close(stat_pipe[1]);

	ASSERT_EQ(1, read(ready[0], &state, 1));
	if (state == 's') {
		close(cmd[1]);
		close(ready[0]);
		close(stat_pipe[0]);
		waitpid(child, &status, 0);
		SKIP(return, "MMU notifier fixture unavailable");
	}
	ASSERT_EQ('r', state);

	for (int attempt = 0; attempt < 6; attempt++) {
		char op = 'g';

		ASSERT_TRUE(read_generation_stats(self->cgroup_name,
						  &generations));
		ASSERT_TRUE(age_memcg(&generations));
		ASSERT_TRUE(write_full(cmd[1], &op, 1));
		ASSERT_TRUE(read_full(stat_pipe[0], &stats, sizeof(stats)));
		if (stats.callbacks > 0)
			break;
		op = 't';
		ASSERT_TRUE(write_full(cmd[1], &op, 1));
		ASSERT_EQ(1, read(ready[0], &state, 1));
		ASSERT_EQ('k', state);
	}

	{
		char quit = 'q';

		(void)write_full(cmd[1], &quit, 1);
	}
	close(cmd[1]);
	close(ready[0]);
	close(stat_pipe[0]);
	ASSERT_EQ(child, waitpid(child, &status, 0));
	ASSERT_TRUE(WIFEXITED(status) && !WEXITSTATUS(status));

	TH_LOG("total=%llu callbacks=%llu bad=%llu max_span=%llu last=[%#llx,%#llx)",
	       (unsigned long long)stats.total_callbacks,
	       (unsigned long long)stats.callbacks,
	       (unsigned long long)stats.bad_ranges,
	       (unsigned long long)stats.max_span,
	       (unsigned long long)stats.last_start,
	       (unsigned long long)stats.last_end);
	EXPECT_GT(stats.callbacks, 0);
	EXPECT_EQ(0, stats.bad_ranges);
}

P3S_HARNESS_4KB_MAIN
