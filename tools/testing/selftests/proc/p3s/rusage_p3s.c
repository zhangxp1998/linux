// SPDX-License-Identifier: GPL-2.0
/*
 * getrusage() ru_maxrss of a 4K compat process and of its 4K and native
 * children matches the VmHWM each process reports, in common kB units.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define PARENT_SIZE	(32UL * 1024 * 1024)
#define CHILD_4K_SIZE	(24UL * 1024 * 1024)
#define CHILD_NATIVE_SIZE (40UL * 1024 * 1024)

struct child_report {
	long page_size;
	unsigned long hwm_kb;
};

static bool read_hwm(unsigned long *hwm_kb)
{
	long value = p3s_status_kb(0, "VmHWM");

	if (value < 0)
		return false;
	*hwm_kb = value;
	return true;
}

static void *fault_mapping(size_t length)
{
	unsigned char *mapping;
	size_t offset;

	mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return MAP_FAILED;
	for (offset = 0; offset < length; offset += PROCESS_PAGE_SIZE)
		mapping[offset] = (unsigned char)offset + 1;
	return mapping;
}

/*
 * VmHWM and ru_maxrss both derive from the mm RSS counters, which are
 * per-CPU batched, so two readings can differ by a few MB of slack.  The
 * unit mismatch this test guards against is a factor of four, so a 4 MB
 * tolerance keeps the check meaningful on a busy system.
 */
static bool close_enough(unsigned long actual, unsigned long expected)
{
	unsigned long difference = actual > expected ? actual - expected :
		expected - actual;

	return difference <= 4096;
}

static void child_workload(int report_fd, size_t length)
{
	struct child_report report;
	void *mapping;

	mapping = fault_mapping(length);
	if (mapping == MAP_FAILED)
		_exit(120);
	/*
	 * VmHWM is an approximate per-CPU reading until an unmap latches the
	 * high-water mark; exit reports that latched value as ru_maxrss.
	 */
	munmap(mapping, length);
	if (!read_hwm(&report.hwm_kb))
		_exit(120);
	report.page_size = sysconf(_SC_PAGESIZE);
	if (write(report_fd, &report, sizeof(report)) != sizeof(report))
		_exit(121);
	close(report_fd);
	_exit(0);
}

static bool collect_child(bool native, size_t length,
			  struct child_report *report)
{
	char fd_string[24];
	char length_string[32];
	int pipefd[2];
	int status;
	pid_t pid;
	pid_t waited;
	ssize_t got;

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
		if (!native)
			child_workload(pipefd[1], length);
		snprintf(fd_string, sizeof(fd_string), "%d", pipefd[1]);
		snprintf(length_string, sizeof(length_string), "%zu", length);
		p3s_execl(false, NULL, "--native-child", fd_string,
			   length_string, NULL);
		_exit(123);
	}
	close(pipefd[1]);
	got = read(pipefd[0], report, sizeof(*report));
	close(pipefd[0]);
	waited = waitpid(pid, &status, 0);
	return got == sizeof(*report) && waited == pid &&
		WIFEXITED(status) &&
		WEXITSTATUS(status) == 0;
}

TEST(self_maxrss_uses_common_kb_units)
{
	struct rusage usage;
	unsigned long parent_hwm;
	void *mapping;

	mapping = fault_mapping(PARENT_SIZE);
	ASSERT_NE(mapping, MAP_FAILED);
	ASSERT_TRUE(read_hwm(&parent_hwm));
	ASSERT_EQ(0, getrusage(RUSAGE_SELF, &usage));
	TH_LOG("self VmHWM=%lu ru_maxrss=%ld", parent_hwm,
	       usage.ru_maxrss);
	EXPECT_TRUE(close_enough(usage.ru_maxrss, parent_hwm));
	munmap(mapping, PARENT_SIZE);
}

TEST(children_maxrss_uses_common_kb_units)
{
	struct child_report child_4k = { };
	struct child_report child_native = { };
	struct rusage usage;
	unsigned long expected_children;

	ASSERT_TRUE(collect_child(false, CHILD_4K_SIZE, &child_4k));
	ASSERT_TRUE(collect_child(true, CHILD_NATIVE_SIZE, &child_native));
	TH_LOG("child4k page=%ld hwm=%lu child_native page=%ld hwm=%lu",
		       child_4k.page_size, child_4k.hwm_kb,
		       child_native.page_size, child_native.hwm_kb);
	ASSERT_EQ(PROCESS_PAGE_SIZE, child_4k.page_size);
	ASSERT_EQ(NATIVE_PAGE_SIZE, child_native.page_size);
	ASSERT_EQ(0, getrusage(RUSAGE_CHILDREN, &usage));
	expected_children = child_4k.hwm_kb > child_native.hwm_kb ?
		child_4k.hwm_kb : child_native.hwm_kb;
	TH_LOG("children expected_max=%lu ru_maxrss=%ld",
		       expected_children, usage.ru_maxrss);
	EXPECT_TRUE(close_enough(usage.ru_maxrss, expected_children));
}

int main(int argc, char **argv)
{
	const char *mode = p3s_run_mode(argc, argv, NULL);

	if (!mode)
		exec_compat(argv[0], P3S_RUN_FLAG, NULL);
	if (argc == 2 && !strcmp(mode, P3S_RUN_FLAG)) {
		p3s_require_compat();
		return p3s_harness_run();
	}
	if (argc == 4 && !strcmp(mode, "--native-child"))
		child_workload(atoi(argv[2]), strtoull(argv[3], NULL, 0));
	return EXIT_FAILURE;
}
