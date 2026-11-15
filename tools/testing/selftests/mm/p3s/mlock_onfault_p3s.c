// SPDX-License-Identifier: GPL-2.0
/*
 * Check byte-sized VmLck accounting in a 4KB process.  Use several kernel
 * pages and inspect VM_LOCKED/VM_LOCKONFAULT separately from RSS.  This
 * is not a large file folio test and does not require pressure, root,
 * PFNs or fixture modules.
 */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define P3S_HARNESS_NATIVE_CONTROL 1
#include "p3s_harness.h"
#include "p3s_test.h"

#ifndef MLOCK_ONFAULT
#define MLOCK_ONFAULT 1
#endif

#define LENGTH (4 * NATIVE_PAGE_SIZE)

struct snapshot {
	unsigned long size;
	unsigned long rss;
	bool locked;
	bool onfault;
};

static bool snapshot(void *address, struct snapshot *s)
{
	unsigned long start, end;
	bool found = false, have_size = false, have_rss = false;
	char line[1024];
	FILE *file;

	memset(s, 0, sizeof(*s));
	file = fopen("/proc/self/smaps", "r");
	if (!file)
		return false;
	while (fgets(line, sizeof(line), file)) {
		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			if (found)
				break;
			found = start <= (uintptr_t)address && (uintptr_t)address < end;
			continue;
		}
		if (!found)
			continue;
		if (sscanf(line, "Size: %lu kB", &s->size) == 1)
			have_size = true;
		if (sscanf(line, "Rss: %lu kB", &s->rss) == 1)
			have_rss = true;
		if (!strncmp(line, "VmFlags:", 8)) {
			char *flag = strtok(line + 8, "\t\n ");

			while (flag) {
				s->locked |= !strcmp(flag, "lo");
				s->onfault |= !strcmp(flag, "lf");
				flag = strtok(NULL, "\t\n ");
			}
			fclose(file);
			return have_size && have_rss;
		}
	}
	fclose(file);
	return false;
}

static unsigned long locked_kb(struct __test_metadata *_metadata)
{
	long kb = p3s_status_kb("VmLck");

	ASSERT_GE(kb, 0);
	return kb;
}

static bool resident(void *map, size_t page_size, bool all)
{
	unsigned char vec[LENGTH / PROCESS_PAGE_SIZE];
	size_t nr = LENGTH / page_size;
	size_t i;

	if (mincore(map, LENGTH, vec))
		return false;
	for (i = 0; i < nr; i++) {
		if (!!(vec[i] & 1) != all)
			return false;
	}
	return true;
}

FIXTURE(mlock_onfault)
{
	unsigned char *reservation;
	unsigned char *map;
	size_t page_size;
	size_t reservation_size;
	unsigned long before;
	bool locked;
};

FIXTURE_SETUP(mlock_onfault)
{
	struct rlimit limit;

	self->reservation = MAP_FAILED;
	self->locked = false;
	self->page_size = getpagesize();
	self->reservation_size = LENGTH + 2 * NATIVE_PAGE_SIZE;
	ASSERT_EQ(0, getrlimit(RLIMIT_MEMLOCK, &limit));
	self->before = locked_kb(_metadata);
	if (limit.rlim_cur != RLIM_INFINITY &&
	    limit.rlim_cur < LENGTH + self->before * 1024)
		SKIP(return, "needs 64K of available RLIMIT_MEMLOCK");
	self->reservation = mmap(NULL, self->reservation_size, PROT_NONE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(self->reservation, MAP_FAILED);
	self->map = (unsigned char *)
		(((uintptr_t)self->reservation + NATIVE_PAGE_SIZE) &
		 ~(NATIVE_PAGE_SIZE - 1));
	ASSERT_EQ(0, mprotect(self->map, LENGTH, PROT_READ | PROT_WRITE));
	if (madvise(self->map, LENGTH, MADV_NOHUGEPAGE))
		ASSERT_EQ(EINVAL, errno);
}

FIXTURE_TEARDOWN(mlock_onfault)
{
	if (self->locked)
		EXPECT_EQ(0, munlock(self->map, LENGTH));
	if (self->reservation != MAP_FAILED)
		EXPECT_EQ(0, munmap(self->reservation, self->reservation_size));
}

static int lock_onfault(unsigned char *map)
{
	return syscall(SYS_mlock2, map, LENGTH, MLOCK_ONFAULT);
}

TEST_F(mlock_onfault, lock_flags_without_eager_population)
{
	struct snapshot initial, after;
	bool absent;
	int lock_ret;

	ASSERT_TRUE(snapshot(self->map, &initial));
	absent = resident(self->map, self->page_size, false);
	EXPECT_EQ(LENGTH / 1024, initial.size);
	EXPECT_EQ(0, initial.rss);
	EXPECT_FALSE(initial.locked);
	EXPECT_FALSE(initial.onfault);
	EXPECT_TRUE(absent);
	errno = 0;
	lock_ret = lock_onfault(self->map);
	if (lock_ret) {
		if (errno == EPERM || errno == ENOMEM)
			SKIP(return, "mlock2 unavailable or limit reached");
		ASSERT_EQ(0, lock_ret);
	}
	self->locked = true;
	ASSERT_TRUE(snapshot(self->map, &after));
	EXPECT_TRUE(after.locked);
	EXPECT_TRUE(after.onfault);
	EXPECT_EQ(0, after.rss);
	EXPECT_TRUE(resident(self->map, self->page_size, false));
	EXPECT_EQ(self->before + LENGTH / 1024, locked_kb(_metadata));
}

TEST_F(mlock_onfault, eager_lock_after_onfault_unlock)
{
	struct snapshot s;
	unsigned long faulted_rss;
	int lock_ret;

	errno = 0;
	lock_ret = lock_onfault(self->map);
	if (lock_ret) {
		if (errno == EPERM || errno == ENOMEM)
			SKIP(return, "mlock2 unavailable or limit reached");
		ASSERT_EQ(0, lock_ret);
	}
	self->locked = true;
	self->map[0] = 0x39;
	self->map[LENGTH - 1] = 0x71;
	ASSERT_TRUE(snapshot(self->map, &s));
	faulted_rss = s.rss;
	ASSERT_EQ(0, munlock(self->map, LENGTH));
	self->locked = false;
	ASSERT_TRUE(snapshot(self->map, &s));
	EXPECT_FALSE(s.locked);
	EXPECT_FALSE(s.onfault);
	EXPECT_EQ(faulted_rss, s.rss);
	EXPECT_EQ(self->before, locked_kb(_metadata));
	ASSERT_EQ(0, mlock(self->map, LENGTH));
	self->locked = true;
	ASSERT_TRUE(snapshot(self->map, &s));
	EXPECT_TRUE(s.locked);
	EXPECT_FALSE(s.onfault);
	EXPECT_EQ(LENGTH / 1024, s.rss);
	EXPECT_TRUE(resident(self->map, self->page_size, true));
	EXPECT_EQ(self->before + LENGTH / 1024, locked_kb(_metadata));
	EXPECT_EQ(0x39, self->map[0]);
	EXPECT_EQ(0x71, self->map[LENGTH - 1]);
	EXPECT_EQ(0, self->map[LENGTH / 2]);
	ASSERT_EQ(0, munlock(self->map, LENGTH));
	self->locked = false;
	ASSERT_TRUE(snapshot(self->map, &s));
	EXPECT_FALSE(s.locked);
	EXPECT_FALSE(s.onfault);
	EXPECT_EQ(LENGTH / 1024, s.rss);
	EXPECT_EQ(self->before, locked_kb(_metadata));
}

P3S_HARNESS_4KB_MAIN
