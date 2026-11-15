// SPDX-License-Identifier: GPL-2.0
/*
 * swapon(2) of an area formatted with a 4K swap header converts the header
 * to native 16K units before validating it.  The bad-page list lives in the
 * header page, so its length must be checked against the header page before
 * header conversion walks, sorts and rewrites it, and the walk stays
 * within the flexible badpages[] array of union swap_header (aosp/4368002,
 * sizing __MAX_SWAP_BADPAGES entries in a 16K header page instead of a
 * legacy one-element badpages[1] declaration).
 *
 * Two headers are tried, each must be rejected with EINVAL without a kernel
 * report: one with two bad pages (a valid count; regular swap files simply
 * may not have bad pages) and one whose bad-page list runs 16 bytes past the
 * 16K page cache folio holding the header.  The header file is exactly one
 * native page long so that folio cannot be larger; an overrun then lands in
 * a neighbouring physical page, which KASAN reports (test skips when the
 * kernel was built without CONFIG_KASAN).  Root is required; set
 * P3S_SWAP_HEADER_DIR to a directory on a filesystem that supports swap
 * files (default: the current directory).
 */
#define _GNU_SOURCE

#include <limits.h>
#include <sys/klog.h>
#include <sys/syscall.h>

#include "p3s_harness.h"

#define SWAP_VERSION_OFFSET		1024
#define SWAP_LAST_PAGE_OFFSET		1028
#define SWAP_BADPAGES_OFFSET		1032
#define SWAP_BADPAGE_LIST_OFFSET	1536
#define SWAP_MAGIC			"SWAPSPACE2"
#define SWAP_MAGIC_SIZE			10
/* Entries that fit in a native 16K header page. */
#define NATIVE_BADPAGE_SLOTS \
	((NATIVE_PAGE_SIZE - SWAP_BADPAGE_LIST_OFFSET) / sizeof(uint32_t))

#define SYSLOG_ACTION_READ_ALL		3
#define SYSLOG_ACTION_SIZE_BUFFER	10

static char swap_path[PATH_MAX];

static void store_u32(unsigned char *buffer, size_t offset, uint32_t value)
{
	memcpy(buffer + offset, &value, sizeof(value));
}

static bool write_header(uint32_t nr_badpages)
{
	unsigned char *header = calloc(1, NATIVE_PAGE_SIZE);
	uint32_t i, slots;
	int fd;
	bool ok = false;

	if (!header)
		return false;
	unlink(swap_path);
	fd = open(swap_path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0)
		goto out;
	store_u32(header, SWAP_VERSION_OFFSET, 1);
	store_u32(header, SWAP_LAST_PAGE_OFFSET, 0x10000);
	store_u32(header, SWAP_BADPAGES_OFFSET, nr_badpages);
	/* Distinct entries so deduplication cannot shrink the list. */
	slots = nr_badpages < NATIVE_BADPAGE_SLOTS ? nr_badpages :
		NATIVE_BADPAGE_SLOTS;
	for (i = 0; i < slots; i++)
		store_u32(header, SWAP_BADPAGE_LIST_OFFSET + i * 4,
			  (i + 1) * 4);
	/* 4K header: the signature sits at the end of the first 4K. */
	memcpy(header + PROCESS_PAGE_SIZE - SWAP_MAGIC_SIZE, SWAP_MAGIC,
	       SWAP_MAGIC_SIZE);
	ok = pwrite(fd, header, NATIVE_PAGE_SIZE, 0) == NATIVE_PAGE_SIZE &&
	     !fsync(fd);
	close(fd);
out:
	free(header);
	return ok;
}

/* Count kernel log lines that look like a memory-safety report. */
static int kernel_reports(void)
{
	int size = klogctl(SYSLOG_ACTION_SIZE_BUFFER, NULL, 0), len, count = 0;
	char *log, *line, *save = NULL;

	if (size <= 0)
		return -1;
	log = malloc(size + 1);
	if (!log)
		return -1;
	len = klogctl(SYSLOG_ACTION_READ_ALL, log, size);
	if (len < 0) {
		free(log);
		return -1;
	}
	log[len] = '\0';
	for (line = strtok_r(log, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		if (strstr(line, "BUG: KASAN") || strstr(line, "BUG: unable") ||
		    strstr(line, "Unable to handle kernel") ||
		    strstr(line, "Internal error") || strstr(line, "UBSAN"))
			count++;
	}
	free(log);
	return count;
}

struct attempt_result {
	int before, after, ret, error;
	bool wrote;
};

static struct attempt_result attempt(uint32_t nr_badpages)
{
	struct attempt_result result = {};

	result.wrote = write_header(nr_badpages);
	if (!result.wrote)
		goto out;
	result.before = kernel_reports();
	errno = 0;
	result.ret = syscall(SYS_swapon, swap_path, 0);
	result.error = errno;
	if (!result.ret)
		syscall(SYS_swapoff, swap_path);
	result.after = kernel_reports();
out:
	unlink(swap_path);
	return result;
}

static void set_swap_path(void)
{
	const char *dir = getenv("P3S_SWAP_HEADER_DIR");

	snprintf(swap_path, sizeof(swap_path), "%s/p3s_badpages_%d.swap",
		 dir ? dir : ".", (int)getpid());
}

static bool check_result(struct __test_metadata *_metadata,
			 uint32_t nr_badpages)
{
	struct attempt_result result = attempt(nr_badpages);

	TH_LOG("badpages %u: swapon = %d (%s), reports %d -> %d",
	       nr_badpages, result.ret, strerror(result.error),
	       result.before, result.after);
	return result.wrote && result.ret == -1 && result.error == EINVAL &&
	       result.before >= 0 && result.after == result.before;
}

static bool kasan_enabled(void)
{
	char line[256];
	FILE *file;
	bool found = false;

	if (!access("/proc/config.gz", R_OK)) {
		file = popen("gzip -dc /proc/config.gz 2>/dev/null", "r");
		if (file) {
			bool saw_line = false;

			while (fgets(line, sizeof(line), file)) {
				saw_line = true;
				if (!strcmp(line, "CONFIG_KASAN=y\n"))
					found = true;
			}
			pclose(file);
			if (saw_line)
				return found;
		}
	}
	if (!access("/sys/module/kasan", F_OK))
		return true;
	file = fopen("/proc/kallsyms", "re");
	if (file) {
		while (fgets(line, sizeof(line), file)) {
			if (strstr(line, "kasan_report")) {
				found = true;
				break;
			}
		}
		fclose(file);
	}
	return found;
}

#define REQUIRE_CLEAN_LOG() do { \
	if (geteuid()) \
		SKIP(return, "root is required for swapon(2)"); \
	if (!kasan_enabled()) \
		SKIP(return, "kernel built without CONFIG_KASAN"); \
	int reports = kernel_reports(); \
	if (reports < 0) \
		SKIP(return, "cannot read the kernel log"); \
	if (reports > 0) \
		SKIP(return, "kernel log already holds a KASAN/BUG report"); \
} while (0)

TEST(invalid_badpage_lists_rejected_without_memory_report)
{
	REQUIRE_CLEAN_LOG();
	set_swap_path();
	ASSERT_TRUE(check_result(_metadata, 2));
	/* A buggy overrun may corrupt a neighbouring page, so try it last. */
	ASSERT_TRUE(check_result(_metadata, NATIVE_BADPAGE_SLOTS + 4));
}

P3S_HARNESS_4KB_MAIN
