// SPDX-License-Identifier: GPL-2.0
/*
 * The four 4K slices of a 16K window of a 4K compat process share one
 * order-0 anonymous folio.  When the window is later split between several
 * VMAs (mprotect, munmap, PR_SET_VMA_ANON_NAME, ...) reverse mapping must
 * preserve every slice. P3S may conservatively keep an exclusive folio
 * resident when its slices span VMAs; partial reclaim is not safe.
 *
 * The test runs in its own memory cgroup and drives reclaim through
 * memory.reclaim.  A window that was never split is the control: once it
 * has gone to swap, each split window must be fully swapped or fully
 * resident, with its contents intact.
 * Root, cgroup v2 memory.reclaim and active swap are required.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>

#include "p3s_slice_test.h"
#include "p3s_harness.h"

#ifndef PR_SET_VMA
#define PR_SET_VMA		0x53564d41
#define PR_SET_VMA_ANON_NAME	0
#endif

#define CGROUP_ROOT	"/sys/fs/cgroup"
#define RECLAIM_ROUNDS	40
#define FIRST_BYTE	0x31

enum split_kind { SPLIT_NONE, SPLIT_MPROTECT, SPLIT_MUNMAP, SPLIT_ANON_NAME };

struct window {
	const char *name;
	enum split_kind kind;
	unsigned char *reservation, *base;
	bool slice_mapped[P3S_SLICES];
	bool usable;
};

static struct window windows[] = {
	{ "control (not split)", SPLIT_NONE },
	{ "split by mprotect", SPLIT_MPROTECT },
	{ "split by munmap", SPLIT_MUNMAP },
	{ "split by PR_SET_VMA_ANON_NAME", SPLIT_ANON_NAME },
};

static char cgroup_path[PATH_MAX];
static char original_cgroup[PATH_MAX];

static void leave_cgroup(void)
{
	char path[PATH_MAX + 32], pid[32];

	if (!cgroup_path[0])
		return;
	snprintf(pid, sizeof(pid), "%d", (int)getpid());
	snprintf(path, sizeof(path), "%s%s/cgroup.procs", CGROUP_ROOT,
		 original_cgroup);
	if (p3s_write_text(path, pid))
		p3s_write_text(CGROUP_ROOT "/cgroup.procs", pid);
	rmdir(cgroup_path);
	cgroup_path[0] = '\0';
}

static bool enter_cgroup(void)
{
	char path[PATH_MAX + 32], pid[32], line[PATH_MAX + 3];
	FILE *self = fopen("/proc/self/cgroup", "re");

	if (!self)
		return false;
	while (fgets(line, sizeof(line), self)) {
		if (!strncmp(line, "0::", 3)) {
			line[strcspn(line, "\n")] = '\0';
			snprintf(original_cgroup, sizeof(original_cgroup), "%s",
				 line + 3);
		}
	}
	fclose(self);
	if (!strcmp(original_cgroup, "/"))
		original_cgroup[0] = '\0';
	snprintf(cgroup_path, sizeof(cgroup_path), CGROUP_ROOT "/p3s_rmap_%d",
		 (int)getpid());
	if (mkdir(cgroup_path, 0755)) {
		cgroup_path[0] = '\0';
		return false;
	}
	snprintf(pid, sizeof(pid), "%d", (int)getpid());
	snprintf(path, sizeof(path), "%s/cgroup.procs", cgroup_path);
	if (p3s_write_text(path, pid)) {
		rmdir(cgroup_path);
		cgroup_path[0] = '\0';
		return false;
	}
	return true;
}

static void reclaim_round(void)
{
	char path[PATH_MAX + 32];

	snprintf(path, sizeof(path), "%s/memory.reclaim", cgroup_path);
	/* -EAGAIN just means less than asked was reclaimed. */
	p3s_write_text(path, "256M");
}

static bool setup_window(struct window *w)
{
	unsigned char *p;
	unsigned int i;

	w->base = map_aligned(NATIVE_PAGE_SIZE, &w->reservation);
	if (w->base == MAP_FAILED)
		return false;
	fill_slices(w->base, FIRST_BYTE);
	for (i = 0; i < P3S_SLICES; i++)
		w->slice_mapped[i] = true;
	p = w->base + 2 * PROCESS_PAGE_SIZE;
	switch (w->kind) {
	case SPLIT_NONE:
		break;
	case SPLIT_MPROTECT:
		if (mprotect(p, PROCESS_PAGE_SIZE, PROT_READ))
			return false;
		break;
	case SPLIT_MUNMAP:
		if (munmap(p, PROCESS_PAGE_SIZE))
			return false;
		w->slice_mapped[2] = false;
		break;
	case SPLIT_ANON_NAME:
		if (prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, (unsigned long)p,
			  PROCESS_PAGE_SIZE, (unsigned long)"p3s_split"))
			return false;
		break;
	}
	return true;
}

/* Number of mapped slices that are still resident. */
static unsigned int resident_slices(const struct window *w)
{
	unsigned int i, nr = 0;

	for (i = 0; i < P3S_SLICES; i++)
		if (w->slice_mapped[i] &&
		    p3s_page_present(w->base + i * PROCESS_PAGE_SIZE))
			nr++;
	return nr;
}

static bool contents_intact(const struct window *w)
{
	unsigned int i;

	for (i = 0; i < P3S_SLICES; i++)
		if (w->slice_mapped[i] &&
		    !all_bytes_are(w->base + i * PROCESS_PAGE_SIZE,
				   PROCESS_PAGE_SIZE, FIRST_BYTE + i))
			return false;
	return true;
}

FIXTURE(split_window) { };

FIXTURE_VARIANT(split_window) {
	unsigned int index;
};

FIXTURE_VARIANT_ADD(split_window, mprotect) { .index = 1 };
FIXTURE_VARIANT_ADD(split_window, munmap) { .index = 2 };
FIXTURE_VARIANT_ADD(split_window, anon_name) { .index = 3 };

FIXTURE_SETUP(split_window)
{
	if (geteuid())
		SKIP(return, "root is required for memory.reclaim");
	if (access(CGROUP_ROOT "/memory.reclaim", W_OK))
		SKIP(return, "cgroup v2 memory.reclaim is unavailable");
	if (!enter_cgroup())
		SKIP(return, "cannot create or enter a test memory cgroup: %s",
		     strerror(errno));
}

FIXTURE_TEARDOWN(split_window)
{
	leave_cgroup();
}

TEST_F(split_window, reclaim_preserves_split_slices)
{
	struct window *w = &windows[variant->index];
	unsigned int round, resident, mapped = 0, slice;
	bool control_out = false;

	ASSERT_TRUE(setup_window(&windows[0]));
	if (!setup_window(w))
		SKIP(return, "%s setup failed: %s", w->name, strerror(errno));

	for (round = 0; round < RECLAIM_ROUNDS && !control_out; round++) {
		reclaim_round();
		control_out = !resident_slices(&windows[0]);
	}
	if (!control_out)
		SKIP(return, "memory.reclaim never swapped out the control window");
	/* A few more rounds give every split window the same chances. */
	for (round = 0; round < 4; round++)
		reclaim_round();

	resident = resident_slices(w);
	for (slice = 0; slice < P3S_SLICES; slice++)
		mapped += w->slice_mapped[slice];
	EXPECT_TRUE(!resident || resident == mapped);
	EXPECT_TRUE(contents_intact(&windows[0]));
	EXPECT_TRUE(contents_intact(w));
}

P3S_HARNESS_4KB_MAIN
