// SPDX-License-Identifier: GPL-2.0
/*
 * A 4KB process probes text mapped one 4KB slice past native alignment:
 * the sliced uprobe registers, bumps its ref counter, traces, and on EPAN
 * CPUs delivers an unmatched trap from an execute-only VMA.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "kselftest_p3s.h"
#include "p3s_harness.h"

#define TARGET_OFFSET	(2 * PROCESS_PAGE_SIZE)
#define REF_CTR_OFFSET	(3 * PROCESS_PAGE_SIZE)
#define TARGET_MAP_ADDR	((void *)0x20001000UL)
#define REF_MAP_ADDR	((void *)0x30001000UL)
#define UPROBE_BRK_INSN	0xd42000a0U
#define RET_INSN	0xd65f03c0U

typedef unsigned long (*target_fn_t)(unsigned long);

static sigjmp_buf segv_env;

static void segv_handler(int signal)
{
	siglongjmp(segv_env, signal);
}

static bool write_text(const char *path, const char *text, int flags)
{
	ssize_t length = strlen(text);
	ssize_t written;
	int fd;

	fd = open(path, O_WRONLY | O_CLOEXEC | flags);
	if (fd < 0)
		return false;
	written = write(fd, text, length);
	close(fd);
	return written == length;
}

static bool tracefs_writable(const char *root)
{
	char path[128];

	snprintf(path, sizeof(path), "%s/uprobe_events", root);
	if (access(path, W_OK))
		return false;
	snprintf(path, sizeof(path), "%s/instances", root);
	return access(path, W_OK) == 0;
}

static void trap_child_exit(int signal)
{
	_exit(signal == SIGTRAP ? EXIT_SUCCESS : EXIT_FAILURE);
}

static void *map_target(const char *target_path, bool *target_created,
			void **ref_mapping)
{
	const uint32_t instructions[] = {
		0x91000400, /* add x0, x0, #1 */
		RET_INSN, /* ret */
	};
	const uint32_t unmatched_trap = UPROBE_BRK_INSN;
	const uint32_t wrong_slice = RET_INSN;
	void *mapping = MAP_FAILED;
	int fd;

	*ref_mapping = MAP_FAILED;
	fd = open(target_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
	if (fd < 0)
		return MAP_FAILED;
	*target_created = true;
	if (ftruncate(fd, NATIVE_PAGE_SIZE) ||
	    pwrite(fd, &unmatched_trap, sizeof(unmatched_trap), 0) !=
		    sizeof(unmatched_trap) ||
	    pwrite(fd, &wrong_slice, sizeof(wrong_slice), PROCESS_PAGE_SIZE) !=
		    sizeof(wrong_slice) ||
	    pwrite(fd, instructions, sizeof(instructions), TARGET_OFFSET) !=
		    sizeof(instructions))
		goto out;
	mapping = mmap(TARGET_MAP_ADDR, NATIVE_PAGE_SIZE, PROT_READ | PROT_EXEC,
		       MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 0);
	if (mapping == MAP_FAILED)
		goto out;
	*ref_mapping = mmap(REF_MAP_ADDR, NATIVE_PAGE_SIZE,
			    PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, 0);
	if (*ref_mapping == MAP_FAILED) {
		munmap(mapping, NATIVE_PAGE_SIZE);
		mapping = MAP_FAILED;
	}
out:
	close(fd);
	return mapping;
}

static bool write_trace(const char *instance, const char *name,
			const char *value, int flags)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/%s", instance, name);
	return write_text(path, value, flags);
}

static bool write_uprobe_events(const char *root, const char *text)
{
	return write_trace(root, "uprobe_events", text, O_APPEND);
}

static bool trace_has_event(const char *instance, const char *event)
{
	char buffer[65536], path[256];
	ssize_t size;
	int fd;

	snprintf(path, sizeof(path), "%s/trace", instance);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	size = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if (size < 0)
		return false;
	buffer[size] = '\0';
	return strstr(buffer, event);
}

FIXTURE(uprobe_trace) {
	char trace_root[64];
	char target_path[PATH_MAX];
	char instance[128];
	char event[64];
	bool target_created;
	bool instance_created;
	bool registered;
	void *mapping;
	void *ref_mapping;
};

FIXTURE_SETUP(uprobe_trace)
{
	static const char * const roots[] = {
		"/sys/kernel/tracing",
		"/sys/kernel/debug/tracing",
	};
	char control[PATH_MAX + 128];
	const char *tmpdir;
	unsigned int i;
	int len;

	self->mapping = MAP_FAILED;
	self->ref_mapping = MAP_FAILED;
	for (i = 0; i < ARRAY_SIZE(roots); i++) {
		if (tracefs_writable(roots[i])) {
			snprintf(self->trace_root, sizeof(self->trace_root),
				 "%s", roots[i]);
			break;
		}
	}
	if (!self->trace_root[0])
		SKIP(return, "tracefs uprobe_events is unavailable or not writable");

	tmpdir = getenv("TMPDIR");
	if (!tmpdir || !tmpdir[0])
		tmpdir = "/tmp";
	len = snprintf(self->target_path, sizeof(self->target_path),
		       "%s/uprobe-p3s-target-%d", tmpdir, getpid());
	ASSERT_GT(len, 0);
	ASSERT_LT(len, (int)sizeof(self->target_path));

	self->mapping = map_target(self->target_path, &self->target_created,
				   &self->ref_mapping);
	ASSERT_EQ(TARGET_MAP_ADDR, self->mapping);
	ASSERT_EQ(REF_MAP_ADDR, self->ref_mapping);

	snprintf(self->instance, sizeof(self->instance),
		 "%s/instances/p3s_uprobe_%d", self->trace_root, getpid());
	snprintf(self->event, sizeof(self->event), "uprobe_p3s_%d", getpid());
	ASSERT_EQ(0, mkdir(self->instance, 0700));
	self->instance_created = true;

	len = snprintf(control, sizeof(control), "p:p3s/%s %s:0x2000(0x3000)\n",
		       self->event, self->target_path);
	ASSERT_GT(len, 0);
	ASSERT_LT(len, (int)sizeof(control));
	ASSERT_TRUE(write_uprobe_events(self->trace_root, control));
	self->registered = true;
}

FIXTURE_TEARDOWN_PARENT(uprobe_trace)
{
	char control[128];

	if (self->instance_created) {
		EXPECT_TRUE(write_trace(self->instance, "tracing_on", "0", 0));
		if (self->registered) {
			snprintf(control, sizeof(control), "events/p3s/%s/enable",
				 self->event);
			EXPECT_TRUE(write_trace(self->instance, control, "0", 0));
		}
		EXPECT_EQ(0, rmdir(self->instance));
	}
	if (self->registered) {
		snprintf(control, sizeof(control), "-:p3s/%s\n", self->event);
		EXPECT_TRUE(write_uprobe_events(self->trace_root, control));
	}
	if (self->target_created)
		EXPECT_EQ(0, unlink(self->target_path));
}

TEST_F(uprobe_trace, sliced_uprobe_traces_and_updates_ref_ctr)
{
	char enable[128], control[128];
	uint16_t *ref_ctr;
	target_fn_t target;

	target = (target_fn_t)((char *)self->mapping + TARGET_OFFSET);
	ref_ctr = (uint16_t *)((char *)self->ref_mapping + REF_CTR_OFFSET);

	snprintf(enable, sizeof(enable), "events/p3s/%s/enable", self->event);
	ASSERT_TRUE(write_trace(self->instance, enable, "1", 0));
	ASSERT_TRUE(write_trace(self->instance, "trace", "", O_TRUNC));
	ASSERT_TRUE(write_trace(self->instance, "tracing_on", "1", 0));
	EXPECT_EQ(1U, *ref_ctr);

	EXPECT_EQ(42UL, target(41));
	ASSERT_TRUE(write_trace(self->instance, enable, "0", 0));
	EXPECT_TRUE(trace_has_event(self->instance, self->event));

	snprintf(control, sizeof(control), "-:p3s/%s\n", self->event);
	ASSERT_TRUE(write_uprobe_events(self->trace_root, control));
	self->registered = false;
	EXPECT_EQ(0U, *ref_ctr);

	munmap(self->mapping, NATIVE_PAGE_SIZE);
	munmap(self->ref_mapping, NATIVE_PAGE_SIZE);
}

TEST_F(uprobe_trace, unmatched_xom_trap_is_delivered)
{
	char enable[128];
	int status;
	pid_t pid, waited;

	snprintf(enable, sizeof(enable), "events/p3s/%s/enable", self->event);
	ASSERT_TRUE(write_trace(self->instance, enable, "1", 0));

	pid = fork();
	ASSERT_GE(pid, 0);
	if (!pid) {
		struct sigaction segv = { .sa_handler = segv_handler };
		struct sigaction action = { .sa_handler = trap_child_exit };

		if (mprotect(self->mapping, PROCESS_PAGE_SIZE, PROT_EXEC))
			_exit(EXIT_FAILURE);
		sigemptyset(&segv.sa_mask);
		if (sigaction(SIGSEGV, &segv, NULL))
			_exit(EXIT_FAILURE);
		if (!sigsetjmp(segv_env, 1)) {
			(void)__atomic_load_n((const uint32_t *)self->mapping,
					      __ATOMIC_RELAXED);
			_exit(KSFT_SKIP);
		}
		segv.sa_handler = SIG_DFL;
		if (sigaction(SIGSEGV, &segv, NULL))
			_exit(EXIT_FAILURE);

		sigemptyset(&action.sa_mask);
		if (sigaction(SIGTRAP, &action, NULL) ||
		    sigaction(SIGALRM, &action, NULL))
			_exit(EXIT_FAILURE);
		alarm(2);
		((void (*)(void))self->mapping)();
		_exit(EXIT_FAILURE);
	}

	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	munmap(self->mapping, NATIVE_PAGE_SIZE);
	munmap(self->ref_mapping, NATIVE_PAGE_SIZE);
	ASSERT_EQ(pid, waited);
	ASSERT_TRUE(WIFEXITED(status));
	if (WEXITSTATUS(status) == KSFT_SKIP)
		SKIP(return, "PROT_EXEC remains readable without EPAN");
	EXPECT_EQ(EXIT_SUCCESS, WEXITSTATUS(status));
}

P3S_HARNESS_4KB_MAIN
