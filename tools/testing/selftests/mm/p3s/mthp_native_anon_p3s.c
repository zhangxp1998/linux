// SPDX-License-Identifier: GPL-2.0
/*
 * A P3S kernel must keep the native 16K anonymous fault path identical to
 * upstream.  With an mTHP size enabled (sysfs hugepages-<size>kB/enabled),
 * an anonymous write fault in a native process may allocate a large folio;
 * the fault must then map the whole folio, i.e. every 16K page of the
 * aligned block is present right after a single write, backed by contiguous
 * PFNs.  Mapping only the faulting page of a large folio corrupts the folio's
 * mapcount, refcount and rmap index.
 *
 * Root is required to enable the mTHP size; the previous setting is
 * restored.  4K compat processes never get large anonymous folios and are
 * not tested here.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <limits.h>
#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define THP_ROOT	"/sys/kernel/mm/transparent_hugepage"
#define BLOCKS		16


static bool read_line(const char *path, char *buf, size_t size)
{
	FILE *file = fopen(path, "re");
	bool ok;

	if (!file)
		return false;
	ok = fgets(buf, size, file) != NULL;
	fclose(file);
	if (ok)
		buf[strcspn(buf, "\n")] = '\0';
	return ok;
}

static bool write_str(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t len = strlen(value), ret;

	if (fd < 0)
		return false;
	ret = write(fd, value, len);
	close(fd);
	return ret == len;
}

/* "always inherit [madvise] never" -> "madvise". */
static void bracketed(const char *line, char *out, size_t size)
{
	const char *open = strchr(line, '['), *close;

	out[0] = '\0';
	if (!open || !(close = strchr(open, ']')))
		return;
	snprintf(out, size, "%.*s", (int)(close - open - 1), open + 1);
}

static long fault_alloc_count(const char *stats_path)
{
	char line[64];

	if (!read_line(stats_path, line, sizeof(line)))
		return -1;
	return atol(line);
}

/* Smallest anonymous mTHP size above the native page the kernel offers. */
static bool kpageflags(uint64_t pfn, uint64_t *flags)
{
	int fd = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
	bool ok;

	if (fd < 0)
		return false;
	ok = pread(fd, flags, sizeof(*flags), pfn * sizeof(*flags)) ==
	     sizeof(*flags);
	close(fd);
	return ok;
}

static unsigned long pick_size(void)
{
	unsigned long best = 0, kb;
	struct dirent *entry;
	DIR *dir = opendir(THP_ROOT);

	if (!dir)
		return 0;
	while ((entry = readdir(dir))) {
		if (sscanf(entry->d_name, "hugepages-%lukB", &kb) != 1)
			continue;
		char path[PATH_MAX];

		if (kb * 1024 <= NATIVE_PAGE_SIZE || kb * 1024 > 64 * NATIVE_PAGE_SIZE)
			continue;
		/* Sizes without an anonymous "enabled" control do not count. */
		snprintf(path, sizeof(path), THP_ROOT "/%s/enabled",
			 entry->d_name);
		if (access(path, W_OK))
			continue;
		if (!best || kb < best)
			best = kb;
	}
	closedir(dir);
	return best * 1024;
}

FIXTURE(mthp_native) {
	char enabled_path[PATH_MAX];
	char saved_setting[32];
	bool restore;
	unsigned long folio_size;
	unsigned long pages_per_folio;
	unsigned int checked;
	unsigned int mapped_whole;
	unsigned int partial;
	unsigned int unknown;
	bool data_ok;
};

FIXTURE_SETUP(mthp_native)
{
	/* Global policy changes belong in the test body for parent teardown. */
}

static int probe_native_folios(struct __test_metadata *_metadata,
			       struct _test_data_mthp_native *self)
{
	unsigned long block, i;
	unsigned char *raw, *area;
	long before, after;
	char stats_path[PATH_MAX], line[128];

	if (geteuid())
		return 1;
	self->folio_size = pick_size();
	if (!self->folio_size)
		return 1;
	self->pages_per_folio = self->folio_size / NATIVE_PAGE_SIZE;
	snprintf(self->enabled_path, sizeof(self->enabled_path),
		 THP_ROOT "/hugepages-%lukB/enabled", self->folio_size / 1024);
	snprintf(stats_path, sizeof(stats_path),
		 THP_ROOT "/hugepages-%lukB/stats/anon_fault_alloc",
		 self->folio_size / 1024);
	if (!read_line(self->enabled_path, line, sizeof(line)))
		return 1;
	bracketed(line, self->saved_setting, sizeof(self->saved_setting));
	if (!self->saved_setting[0])
		return -1;
	/* The parent restores policy even if the test child crashes. */
	self->restore = true;
	if (!write_str(self->enabled_path, "always"))
		return 1;
	raw = mmap(NULL, (BLOCKS + 1) * self->folio_size,
		   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
		   -1, 0);
	if (raw == MAP_FAILED)
		return -1;
	area = (unsigned char *)(((uintptr_t)raw + self->folio_size - 1) &
				 ~(self->folio_size - 1));
	madvise(area, BLOCKS * self->folio_size, MADV_HUGEPAGE);
	before = fault_alloc_count(stats_path);
	for (block = 0; block < BLOCKS; block++)
		area[block * self->folio_size] = 0x40 + block;
	after = fault_alloc_count(stats_path);
	/* Keep the system-wide policy changed for as short as possible. */
	if (!write_str(self->enabled_path, self->saved_setting))
		return -1;
	self->restore = false;
	TH_LOG("anon_fault_alloc %ld -> %ld", before, after);
	if (before < 0 || after <= before) {
		munmap(raw, (BLOCKS + 1) * self->folio_size);
		return 1;
	}
	for (block = 0; block < BLOCKS; block++) {
		unsigned char *page = area + block * self->folio_size;
		uint64_t first = 0, pfn, flags;
		unsigned int present = 0;
		bool contiguous = true;

		for (i = 0; i < self->pages_per_folio; i++) {
			if (!p3s_pfn(page + i * NATIVE_PAGE_SIZE, &pfn))
				continue;
			if (!present)
				first = pfn - i;
			else if (pfn != first + i)
				contiguous = false;
			present++;
		}
		self->checked++;
		if (present == self->pages_per_folio && contiguous &&
		    !(first % self->pages_per_folio)) {
			self->mapped_whole++;
			continue;
		}
		if (!p3s_pfn(page, &pfn) || !kpageflags(pfn, &flags)) {
			self->unknown++;
			continue;
		}
		if (flags & (KPF_COMPOUND_HEAD | KPF_COMPOUND_TAIL | KPF_THP)) {
			TH_LOG("block %lu: %u of %lu pages mapped, pfn %#llx flags %#llx",
			       block, present, self->pages_per_folio,
			       (unsigned long long)pfn,
			       (unsigned long long)flags);
			self->partial++;
		}
	}
	for (block = 0; block < BLOCKS; block++)
		for (i = 1; i < self->pages_per_folio; i++)
			area[block * self->folio_size + i * NATIVE_PAGE_SIZE] = 0x80 + i;
	self->data_ok = true;
	for (block = 0; block < BLOCKS; block++) {
		unsigned char *page = area + block * self->folio_size;

		if (page[0] != 0x40 + block)
			self->data_ok = false;
		for (i = 1; i < self->pages_per_folio; i++)
			if (page[i * NATIVE_PAGE_SIZE] != 0x80 + i)
				self->data_ok = false;
	}
	munmap(raw, (BLOCKS + 1) * self->folio_size);
	return 0;
}

FIXTURE_TEARDOWN_PARENT(mthp_native)
{
	if (self->restore)
		EXPECT_TRUE(write_str(self->enabled_path, self->saved_setting));
}

TEST_F(mthp_native, fault_maps_entire_native_folio)
{
	int result = probe_native_folios(_metadata, self);

	if (result > 0)
		SKIP(return, "native anonymous mTHP prerequisites unavailable");
	ASSERT_EQ(0, result);
	TH_LOG("%u blocks: %u whole, %u partial, %u unknown",
	       self->checked, self->mapped_whole, self->partial,
	       self->unknown);
	EXPECT_EQ(0, self->partial);
	EXPECT_EQ(0, self->unknown);
	EXPECT_GT(self->mapped_whole, 0);
}

TEST_F(mthp_native, every_page_preserves_written_data)
{
	int result = probe_native_folios(_metadata, self);

	if (result > 0)
		SKIP(return, "native anonymous mTHP prerequisites unavailable");
	ASSERT_EQ(0, result);
	EXPECT_TRUE(self->data_ok);
}

int main(int argc, char **argv)
{
	p3s_argv0 = argv[0];
	if (argc != 1)
		return EXIT_FAILURE;
	if (getpagesize() != (int)NATIVE_PAGE_SIZE) {
		if (p3s_is_compat_process())
			exec_native(NULL, NULL);
		return KSFT_SKIP;
	}
	return p3s_harness_run();
}
