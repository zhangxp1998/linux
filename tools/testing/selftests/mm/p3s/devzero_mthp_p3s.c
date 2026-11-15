// SPDX-License-Identifier: GPL-2.0
/*
 * A private /dev/zero VMA retains vm_file, but a write fault installs an
 * anonymous folio.  With native 16K pages and 64K mTHP enabled, its PTEs
 * must map that folio, not page frames derived from a file offset.
 *
 * Run only on a disposable or fixed kernel: an unfixed kernel can map and
 * write unrelated physical pages.  Restore the global THP controls on exit.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/mman.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define THP_ROOT	"/sys/kernel/mm/transparent_hugepage"
#define MTHP_SIZE	(64 * 1024UL)
#define BLOCKS		16
#define KPF_ANON	(1ULL << 12)

static bool read_setting(const char *path, char *saved, size_t size)
{
	char line[128];
	const char *open, *close;
	FILE *file = fopen(path, "re");

	if (!file)
		return false;
	if (!fgets(line, sizeof(line), file)) {
		fclose(file);
		return false;
	}
	fclose(file);
	open = strchr(line, '[');
	close = open ? strchr(open, ']') : NULL;
	if (!close || close - open <= 1 || (size_t)(close - open) >= size)
		return false;
	snprintf(saved, size, "%.*s", (int)(close - open - 1), open + 1);
	return true;
}

static bool write_setting(const char *path, const char *value)
{
	size_t len = strlen(value);
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	bool ok = fd >= 0 && write(fd, value, len) == (ssize_t)len;

	if (fd >= 0)
		close(fd);
	return ok;
}

static bool page_flags(uint64_t pfn, uint64_t *flags)
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

struct outcome {
	unsigned int checked, anon, huge, data_errors;
	bool page_info;
};

static struct outcome check_mapping(bool devzero)
{
	const size_t len = BLOCKS * MTHP_SIZE;
	struct outcome result = { .page_info = true };
	unsigned char *raw, *base;
	int fd = -1, flags = MAP_PRIVATE;
	unsigned int block, page;

	if (devzero)
		fd = open("/dev/zero", O_RDWR | O_CLOEXEC);
	else
		flags |= MAP_ANONYMOUS;
	if (devzero && fd < 0) {
		result.page_info = false;
		return result;
	}
	raw = mmap(NULL, len + MTHP_SIZE, PROT_READ | PROT_WRITE,
		   flags, fd, 0);
	if (fd >= 0)
		close(fd);
	if (raw == MAP_FAILED) {
		result.page_info = false;
		return result;
	}
	base = (void *)(((uintptr_t)raw + MTHP_SIZE - 1) &
			 ~(uintptr_t)(MTHP_SIZE - 1));
	if (base > raw)
		munmap(raw, base - raw);
	if (raw + len + MTHP_SIZE > base + len)
		munmap(base + len, raw + len + MTHP_SIZE - (base + len));
	if (madvise(base, len, MADV_HUGEPAGE)) {
		result.page_info = false;
		goto out;
	}
	for (block = 0; block < BLOCKS; block++) {
		unsigned char *p = base + block * MTHP_SIZE;
		uint64_t pfn, bits;

		for (page = 0; page < MTHP_SIZE / NATIVE_PAGE_SIZE; page++)
			p[page * NATIVE_PAGE_SIZE] = block + page + 1;
		for (page = 0; page < MTHP_SIZE / NATIVE_PAGE_SIZE; page++)
			if (p[page * NATIVE_PAGE_SIZE] != block + page + 1)
				result.data_errors++;
		if (!p3s_pfn(p, &pfn) || !page_flags(pfn, &bits)) {
			result.page_info = false;
			break;
		}
		result.checked++;
		result.anon += !!(bits & KPF_ANON);
		result.huge += !!(bits & KPF_THP);
		if (block < 4)
			ksft_print_msg("%s block %u: PFN %#llx flags %#llx\n",
				       devzero ? "devzero" : "control", block,
				       (unsigned long long)pfn,
				       (unsigned long long)bits);
	}
out:
	munmap(base, len);
	return result;
}

FIXTURE(mthp_policy) {
	char saved_global[32];
	char saved_size[32];
	struct outcome control;
	bool restore;
};

FIXTURE_SETUP(mthp_policy)
{
	/* Enable policy in the test body so parent teardown runs on every exit. */
}

static const char *prepare_mthp_policy(struct _test_data_mthp_policy *self)
{
	if (geteuid())
		return "root is required to read PFNs and set mTHP policy";
	if (!read_setting(THP_ROOT "/enabled", self->saved_global,
			  sizeof(self->saved_global)) ||
	    !read_setting(THP_ROOT "/hugepages-64kB/enabled", self->saved_size,
			  sizeof(self->saved_size)))
		return "64K anonymous mTHP is unavailable";
	/* The parent must restore policy if the corruption-sensitive child dies. */
	self->restore = true;
	if (!write_setting(THP_ROOT "/enabled", "always") ||
	    !write_setting(THP_ROOT "/hugepages-64kB/enabled", "always"))
		return "cannot enable 64K mTHP";
	/* Probe PFN visibility before starting the corruption-sensitive case. */
	self->control = check_mapping(false);
	if (!self->control.page_info || self->control.checked != BLOCKS ||
	    !self->control.huge)
		return "control did not allocate visible 64K anon folios";
	return NULL;
}

FIXTURE_TEARDOWN_PARENT(mthp_policy)
{
	if (!self->restore)
		return;
	EXPECT_TRUE(write_setting(THP_ROOT "/hugepages-64kB/enabled",
				  self->saved_size));
	EXPECT_TRUE(write_setting(THP_ROOT "/enabled", self->saved_global));
}

TEST_F(mthp_policy, anonymous_control_uses_own_folios)
{
	const char *reason = prepare_mthp_policy(self);

	if (reason)
		SKIP(return, "%s", reason);
	EXPECT_EQ(BLOCKS, self->control.anon);
	EXPECT_EQ(0, self->control.data_errors);
}

TEST_F(mthp_policy, private_devzero_uses_anonymous_mthp)
{
	const char *reason = prepare_mthp_policy(self);
	struct outcome devzero;

	if (reason)
		SKIP(return, "%s", reason);
	devzero = check_mapping(true);
	ksft_print_msg("private /dev/zero: checked=%u anon=%u mTHP=%u data_errors=%u\n",
		       devzero.checked, devzero.anon, devzero.huge,
		       devzero.data_errors);
	ASSERT_TRUE(devzero.page_info);
	ASSERT_EQ(BLOCKS, devzero.checked);
	EXPECT_EQ(BLOCKS, devzero.anon);
	EXPECT_GT(devzero.huge, 0);
	EXPECT_EQ(0, devzero.data_errors);
}

int main(int argc, char **argv)
{
	p3s_argv0 = argv[0];
	if (argc != 1)
		return EXIT_FAILURE;
	if (getpagesize() != (int)NATIVE_PAGE_SIZE) {
		if (p3s_is_compat_process())
			exec_native(NULL, NULL);
		ksft_print_header();
		ksft_exit_skip("requires native 16K pages\n");
	}
	return p3s_harness_run();
}
