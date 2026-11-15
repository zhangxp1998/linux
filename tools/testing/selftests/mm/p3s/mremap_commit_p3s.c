// SPDX-License-Identifier: GPL-2.0
/*
 * Committed_AS accounts bytes independently of the process page size and of
 * VMA fragmentation, and strict overcommit enforces the same byte limit.
 */
#define _GNU_SOURCE

#include <sys/mman.h>
#include <unistd.h>

#include "p3s_harness.h"

#define ACCOUNT_SIZE (16UL * 1024)
#define ACCOUNT_SLICES (ACCOUNT_SIZE / PROCESS_PAGE_SIZE)
/*
 * Strict overcommit is enforced against a per-CPU approximation of
 * Committed_AS, so the limit is set well above the admitted mapping and the
 * rejected one overshoots it by far more than that slack.
 */
#define STRICT_MARGIN (64UL * 1024 * 1024)
#define STRICT_OVERSIZE (4UL * STRICT_MARGIN)
#define CHARGE_ATTEMPTS 5

static const char *const sysctl_paths[] = {
	"/proc/sys/vm/user_reserve_kbytes",
	"/proc/sys/vm/admin_reserve_kbytes",
	"/proc/sys/vm/overcommit_kbytes",
	"/proc/sys/vm/overcommit_ratio",
	"/proc/sys/vm/overcommit_memory",
};
enum {
	USER_RESERVE,
	ADMIN_RESERVE,
	OVERCOMMIT_KBYTES,
	OVERCOMMIT_RATIO,
	OVERCOMMIT_MEMORY,
};

FIXTURE(overcommit) {
	unsigned long original[ARRAY_SIZE(sysctl_paths)];
	bool restore;
};

FIXTURE_SETUP(overcommit)
{
}

static bool write_sysctl(const char *path, unsigned long value)
{
	char buffer[32];
	ssize_t length;
	int fd;

	length = snprintf(buffer, sizeof(buffer), "%lu\n", value);
	if (length <= 0 || (size_t)length >= sizeof(buffer))
		return false;
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	if (write(fd, buffer, length) != length) {
		close(fd);
		return false;
	}
	return !close(fd);
}

static bool restore_one_sysctl(FIXTURE_DATA(overcommit) *self,
			       unsigned int index)
{
	if (write_sysctl(sysctl_paths[index], self->original[index]))
		return true;
	fprintf(stderr, "restore %s failed: %s\n",
		sysctl_paths[index], strerror(errno));
	return false;
}

static bool restore_sysctls(FIXTURE_DATA(overcommit) *self)
{
	bool restored = true;

	if (!self->restore)
		return true;
	/* The two limit knobs reset each other. Restore the active one last. */
	restored &= write_sysctl(sysctl_paths[OVERCOMMIT_MEMORY], 1);
	restored &= restore_one_sysctl(self, USER_RESERVE);
	restored &= restore_one_sysctl(self, ADMIN_RESERVE);
	if (self->original[OVERCOMMIT_KBYTES]) {
		restored &= restore_one_sysctl(self, OVERCOMMIT_RATIO);
		restored &= restore_one_sysctl(self, OVERCOMMIT_KBYTES);
	} else {
		restored &= restore_one_sysctl(self, OVERCOMMIT_KBYTES);
		restored &= restore_one_sysctl(self, OVERCOMMIT_RATIO);
	}
	restored &= restore_one_sysctl(self, OVERCOMMIT_MEMORY);
	if (restored)
		self->restore = false;
	return restored;
}

FIXTURE_TEARDOWN_PARENT(overcommit)
{
	EXPECT_TRUE(restore_sysctls(self));
}

static bool save_sysctls(FIXTURE_DATA(overcommit) *self)
{
	FILE *file;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sysctl_paths); i++) {
		file = fopen(sysctl_paths[i], "re");
		if (!file)
			return false;
		if (fscanf(file, "%lu", &self->original[i]) != 1) {
			fclose(file);
			return false;
		}
		fclose(file);
	}
	self->restore = true;
	return true;
}

static unsigned long meminfo_kb(struct __test_metadata *_metadata,
				const char *field, bool required)
{
	unsigned long value = 0;
	size_t field_len = strlen(field);
	char *line = NULL;
	size_t capacity = 0;
	FILE *meminfo;

	meminfo = fopen("/proc/meminfo", "re");
	ASSERT_NE(meminfo, NULL);
	while (getline(&line, &capacity, meminfo) >= 0) {
		if (!strncmp(line, field, field_len) && line[field_len] == ':' &&
		    sscanf(line + field_len + 1, " %lu kB", &value) == 1)
			break;
	}
	free(line);
	fclose(meminfo);
	if (required)
		ASSERT_NE(0, value) TH_LOG("missing %s in /proc/meminfo", field);
	return value;
}

static unsigned long committed_kb(struct __test_metadata *_metadata)
{
	return meminfo_kb(_metadata, "Committed_AS", true);
}

/*
 * Committed_AS is system wide; a reading that is still changing when read
 * twice is being moved by another process, so wait for it to settle.
 */
static unsigned long stable_committed_kb(struct __test_metadata *_metadata)
{
	unsigned long value = committed_kb(_metadata);
	int i;

	for (i = 0; i < 100; i++) {
		unsigned long again = committed_kb(_metadata);

		if (again == value)
			return value;
		value = again;
		usleep(1000);
	}
	return value;
}

struct charges {
	unsigned long before;
	unsigned long after_map;
	unsigned long after_expand;
	unsigned long after_unmap;
	long initial;
	long expansion;
	long uncharge;
};

static long delta_kb(unsigned long after, unsigned long before)
{
	return (long)after - (long)before;
}

/*
 * Map, expand and unmap 16K, recording the Committed_AS
 * deltas.  Another process may commit memory in between, so the caller
 * retries when the deltas do not match; the syscalls themselves must never
 * fail.
 */
static void charge_sequence(struct __test_metadata *_metadata, struct charges *c)
{
	void *mapping;
	void *expanded;

	c->before = stable_committed_kb(_metadata);
	mapping = mmap(NULL, ACCOUNT_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	c->after_map = stable_committed_kb(_metadata);
	expanded = mremap(mapping, ACCOUNT_SIZE, 2 * ACCOUNT_SIZE,
			  MREMAP_MAYMOVE);
	ASSERT_NE(expanded, MAP_FAILED);
	c->after_expand = stable_committed_kb(_metadata);
	ASSERT_EQ(0, munmap(expanded, 2 * ACCOUNT_SIZE));
	c->after_unmap = stable_committed_kb(_metadata);
	c->initial = delta_kb(c->after_map, c->before);
	c->expansion = delta_kb(c->after_expand, c->after_map);
	c->uncharge = delta_kb(c->after_expand, c->after_unmap);
}

static bool charges_match(const struct charges *c)
{
	return c->initial == ACCOUNT_SIZE / 1024 &&
	       c->expansion == ACCOUNT_SIZE / 1024 &&
	       c->uncharge == 2 * ACCOUNT_SIZE / 1024;
}

struct split_uncharge {
	long initial;
	long slice[ACCOUNT_SLICES];
	long total;
};

static void split_uncharge_sequence(struct __test_metadata *_metadata,
				    struct split_uncharge *s)
{
	unsigned long before, after_map, previous, after;
	char *mapping;
	int i;

	before = stable_committed_kb(_metadata);
	mapping = mmap(NULL, ACCOUNT_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	after_map = stable_committed_kb(_metadata);
	previous = after_map;
	for (i = 0; i < ACCOUNT_SLICES; i++) {
		ASSERT_EQ(0, munmap(mapping + i * PROCESS_PAGE_SIZE,
				    PROCESS_PAGE_SIZE));
		after = stable_committed_kb(_metadata);
		s->slice[i] = delta_kb(previous, after);
		previous = after;
	}
	s->initial = delta_kb(after_map, before);
	s->total = delta_kb(after_map, previous);
}

static bool split_matches(const struct split_uncharge *s)
{
	int i;

	if (s->initial != ACCOUNT_SIZE / 1024 ||
	    s->total != ACCOUNT_SIZE / 1024)
		return false;
	for (i = 0; i < ACCOUNT_SLICES; i++)
		if (s->slice[i] != PROCESS_PAGE_SIZE / 1024)
			return false;
	return true;
}

struct split_protect {
	long charge[ACCOUNT_SLICES];
	long uncharge[ACCOUNT_SLICES];
	long charged_total;
	long uncharged_total;
};

static void split_protect_sequence(struct __test_metadata *_metadata,
				   struct split_protect *s)
{
	unsigned long before, previous, after_charges, after;
	char *reservation, *mapping;
	int i;

	/* Keep both sides PROT_NONE so mprotect cannot merge with a neighbor. */
	reservation = mmap(NULL, 4 * NATIVE_PAGE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(reservation, MAP_FAILED);
	mapping = (char *)(((uintptr_t)reservation + 2 * NATIVE_PAGE_SIZE - 1) &
			   ~(uintptr_t)(NATIVE_PAGE_SIZE - 1));
	mapping = mmap(mapping, ACCOUNT_SIZE, PROT_READ,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT_NE(mapping, MAP_FAILED);
	before = stable_committed_kb(_metadata);
	previous = before;
	for (i = 0; i < ACCOUNT_SLICES; i++) {
		ASSERT_EQ(0, mprotect(mapping + i * PROCESS_PAGE_SIZE,
				      PROCESS_PAGE_SIZE,
				      PROT_READ | PROT_WRITE));
		after = stable_committed_kb(_metadata);
		s->charge[i] = delta_kb(after, previous);
		previous = after;
	}
	after_charges = previous;
	for (i = 0; i < ACCOUNT_SLICES; i++) {
		ASSERT_EQ(0, mprotect(mapping + i * PROCESS_PAGE_SIZE,
				      PROCESS_PAGE_SIZE, PROT_READ));
		after = stable_committed_kb(_metadata);
		s->uncharge[i] = delta_kb(previous, after);
		previous = after;
	}
	s->charged_total = delta_kb(after_charges, before);
	s->uncharged_total = delta_kb(after_charges, previous);
	ASSERT_EQ(0, munmap(reservation, 4 * NATIVE_PAGE_SIZE));
}

static bool protect_slices_match(const struct split_protect *s)
{
	int i;

	for (i = 0; i < ACCOUNT_SLICES; i++)
		if (s->charge[i] != PROCESS_PAGE_SIZE / 1024 ||
		    s->uncharge[i] != PROCESS_PAGE_SIZE / 1024)
			return false;
	return true;
}

TEST(mremap_committed_as_tracks_bytes)
{
	struct charges c;
	int attempt;

	committed_kb(_metadata); /* Warm up the meminfo reader's heap. */
	for (attempt = 0; attempt < CHARGE_ATTEMPTS; attempt++) {
		charge_sequence(_metadata, &c);
		if (charges_match(&c))
			break;
	}
	TH_LOG("initial=%ld expansion=%ld uncharge=%ld kB",
	       c.initial, c.expansion, c.uncharge);
	EXPECT_EQ((long)ACCOUNT_SIZE / 1024, c.initial);
	EXPECT_EQ((long)ACCOUNT_SIZE / 1024, c.expansion);
	EXPECT_EQ((long)(2 * ACCOUNT_SIZE) / 1024, c.uncharge);
}

TEST(split_munmap_uncharges_each_slice)
{
	struct split_uncharge split;
	int attempt;

	committed_kb(_metadata);
	for (attempt = 0; attempt < CHARGE_ATTEMPTS; attempt++) {
		split_uncharge_sequence(_metadata, &split);
		if (split_matches(&split))
			break;
	}
	EXPECT_EQ((long)ACCOUNT_SIZE / 1024, split.initial);
	EXPECT_EQ((long)ACCOUNT_SIZE / 1024, split.total);
	for (int i = 0; i < ACCOUNT_SLICES; i++)
		EXPECT_EQ((long)PROCESS_PAGE_SIZE / 1024, split.slice[i]);
}

TEST(split_mprotect_charges_each_slice)
{
	struct split_protect protect;
	int attempt;

	committed_kb(_metadata);
	for (attempt = 0; attempt < CHARGE_ATTEMPTS; attempt++) {
		split_protect_sequence(_metadata, &protect);
		if (protect.charged_total == ACCOUNT_SIZE / 1024 &&
		    protect.uncharged_total == ACCOUNT_SIZE / 1024 &&
		    protect_slices_match(&protect))
			break;
	}
	EXPECT_EQ((long)ACCOUNT_SIZE / 1024, protect.charged_total);
	EXPECT_EQ((long)ACCOUNT_SIZE / 1024, protect.uncharged_total);
	for (int i = 0; i < ACCOUNT_SLICES; i++) {
		EXPECT_EQ((long)PROCESS_PAGE_SIZE / 1024, protect.charge[i]);
		EXPECT_EQ((long)PROCESS_PAGE_SIZE / 1024, protect.uncharge[i]);
	}
}

TEST_F(overcommit, strict_uses_byte_limit)
{
	unsigned long before, swap_kb, limit_kb, headroom_kb;
	unsigned long map_len, oversized_len;
	void *mapping, *expanded;
	int strict_errno;

	if (geteuid())
		SKIP(return, "changing overcommit sysctls needs root");
	ASSERT_TRUE(save_sysctls(self));
	ASSERT_TRUE(write_sysctl(sysctl_paths[USER_RESERVE], 0));
	ASSERT_TRUE(write_sysctl(sysctl_paths[ADMIN_RESERVE], 0));
	before = stable_committed_kb(_metadata);
	swap_kb = meminfo_kb(_metadata, "SwapTotal", false);
	limit_kb = before + STRICT_MARGIN / 1024;
	limit_kb = limit_kb > swap_kb ? limit_kb - swap_kb : 1;
	ASSERT_TRUE(write_sysctl(sysctl_paths[OVERCOMMIT_KBYTES], limit_kb));
	ASSERT_TRUE(write_sysctl(sysctl_paths[OVERCOMMIT_MEMORY], 2));
	limit_kb = meminfo_kb(_metadata, "CommitLimit", true);
	before = stable_committed_kb(_metadata);
	if (limit_kb <= before + 16 * 1024)
		SKIP(return, "strict commit headroom is unsuitable for this host");
	headroom_kb = limit_kb - before;
	map_len = (headroom_kb / 2 * 1024) & ~(PROCESS_PAGE_SIZE - 1);
	mapping = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, mapping);
	limit_kb = meminfo_kb(_metadata, "CommitLimit", false);
	before = stable_committed_kb(_metadata);
	oversized_len = STRICT_OVERSIZE;
	if (limit_kb > before)
		oversized_len += (limit_kb - before) * 1024;
	errno = 0;
	expanded = mmap(NULL, oversized_len, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	strict_errno = errno;
	EXPECT_EQ(MAP_FAILED, expanded);
	EXPECT_EQ(ENOMEM, strict_errno);
	EXPECT_EQ(0, munmap(mapping, map_len));
	if (expanded != MAP_FAILED)
		munmap(expanded, oversized_len);
}

P3S_HARNESS_4KB_MAIN
