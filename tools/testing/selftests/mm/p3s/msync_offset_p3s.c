// SPDX-License-Identifier: GPL-2.0
/* msync() passes the 4KB VMA's file offset and byte length to fsync. */
#define _GNU_SOURCE
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define TRACEFS "/sys/kernel/tracing"

FIXTURE(msync_offset) {
	char instance[256];
	char event[64];
	bool instance_created;
	bool probe_created;
};

FIXTURE_SETUP(msync_offset)
{
	/* Acquire global resources in the TEST so parent cleanup always runs. */
}

static bool write_path(const char *path, const char *value, int flags)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC | flags);
	bool ok;
	int saved;

	if (fd < 0)
		return false;
	ok = write_full(fd, value, strlen(value));
	saved = errno;
	close(fd);
	errno = saved;
	return ok;
}

static bool write_instance(const FIXTURE_DATA(msync_offset) *self,
			   const char *name, const char *value, int flags)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", self->instance, name);
	return write_path(path, value, flags);
}

FIXTURE_TEARDOWN_PARENT(msync_offset)
{
	char control[160];

	if (self->instance_created) {
		EXPECT_TRUE(write_instance(self, "tracing_on", "0", 0));
		if (self->probe_created) {
			snprintf(control, sizeof(control), "events/kprobes/%s/enable",
				 self->event);
			EXPECT_TRUE(write_instance(self, control, "0", 0));
		}
		EXPECT_EQ(0, rmdir(self->instance));
	}
	if (self->probe_created) {
		snprintf(control, sizeof(control), "-:%s\n", self->event);
		/* Never truncate the global list of other users' kprobes. */
		EXPECT_TRUE(write_path(TRACEFS "/kprobe_events", control, O_APPEND));
	}
}

static bool read_fsync_range(const FIXTURE_DATA(msync_offset) *self,
			     unsigned long long *start, unsigned long long *end)
{
	char path[512], line[1024];
	unsigned int found = 0;
	bool in_case = false;
	FILE *trace;

	snprintf(path, sizeof(path), "%s/trace", self->instance);
	trace = fopen(path, "re");
	if (!trace)
		return false;
	while (fgets(line, sizeof(line), trace)) {
		char *range;

		if (strstr(line, "P3S_MSYNC_BEGIN"))
			in_case = true;
		if (strstr(line, "P3S_MSYNC_END"))
			break;
		if (!in_case || !strstr(line, self->event))
			continue;
		range = strstr(line, "start=0x");
		if (range && sscanf(range, "start=0x%llx end=0x%llx", start, end) == 2)
			found++;
	}
	fclose(trace);
	return found == 1;
}

TEST_F(msync_offset, fsync_range_matches_4kb_file_slice)
{
	unsigned long long start = 0, end = 0;
	char control[256], value[128];
	unsigned char *mapping;
	int memfd;

	snprintf(self->instance, sizeof(self->instance),
		 TRACEFS "/instances/p3s_msync_%d", getpid());
	snprintf(self->event, sizeof(self->event), "p3s_msync_%d", getpid());
	if (mkdir(self->instance, 0700)) {
		if (p3s_fixture_unavailable(errno))
			SKIP(return, "private trace instances unavailable: %s", strerror(errno));
		ASSERT_EQ(0, errno);
	}
	self->instance_created = true;
	snprintf(control, sizeof(control),
		 "p:%s vfs_fsync_range start=%%x1:x64 end=%%x2:x64\n", self->event);
	if (!write_path(TRACEFS "/kprobe_events", control, O_APPEND))
		SKIP(return, "vfs_fsync_range kprobe unavailable: %s", strerror(errno));
	self->probe_created = true;
	snprintf(control, sizeof(control), "events/kprobes/%s/filter", self->event);
	snprintf(value, sizeof(value), "common_pid == %d", getpid());
	ASSERT_TRUE(write_instance(self, control, value, 0));
	snprintf(control, sizeof(control), "events/kprobes/%s/enable", self->event);
	ASSERT_TRUE(write_instance(self, control, "1", 0));
	ASSERT_TRUE(write_instance(self, "trace", "", O_TRUNC));

	memfd = memfd_create("msync-offset-p3s", MFD_CLOEXEC);
	ASSERT_GE(memfd, 0);
	ASSERT_EQ(0, ftruncate(memfd, NATIVE_PAGE_SIZE));
	mapping = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, memfd, PROCESS_PAGE_SIZE);
	ASSERT_NE(MAP_FAILED, mapping);
	mapping[0] = 0x4d;
	ASSERT_TRUE(write_instance(self, "tracing_on", "1", 0));
	ASSERT_TRUE(write_instance(self, "trace_marker", "P3S_MSYNC_BEGIN", 0));
	ASSERT_EQ(0, msync(mapping, PROCESS_PAGE_SIZE, MS_SYNC));
	ASSERT_TRUE(write_instance(self, "trace_marker", "P3S_MSYNC_END", 0));
	ASSERT_TRUE(write_instance(self, "tracing_on", "0", 0));
	ASSERT_TRUE(read_fsync_range(self, &start, &end));
	EXPECT_EQ(PROCESS_PAGE_SIZE, start);
	EXPECT_EQ(2 * PROCESS_PAGE_SIZE - 1, end);
	munmap(mapping, PROCESS_PAGE_SIZE);
	close(memfd);
}

P3S_HARNESS_4KB_MAIN
