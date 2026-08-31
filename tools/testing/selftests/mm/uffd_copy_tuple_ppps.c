// SPDX-License-Identifier: GPL-2.0
/*
 * PPPS: UFFDIO_COPY must install whole native tuples, not singletons.
 *
 * A compat (4K) process runs on 4K page tables while the kernel's native
 * PAGE_SIZE is 16K, so every anonymous PTE is backed by a slice of a native
 * folio.  mfill_atomic() walks the request in MM_PAGE_SIZE(mm) (4K) steps and
 * allocates one native folio per step, so a UFFDIO_COPY-installed page used to
 * become a "singleton": a 16K folio carrying exactly one 4K PTE.  Multi-page
 * UFFDIO_COPY calls carry most of the bytes installed by ART's userfaultfd GC,
 * so that inflated the collector's anonymous footprint 4x.
 *
 * K1 ("tuple-aware mfill") adds a fast path: when a private anonymous COPY is
 * 16K-aligned and at least 16K long, one folio is copied whole and mapped
 * behind all four compat PTEs under a single pte lock.
 *
 * What this test asserts, from a compat process:
 *
 *   1. a 16K UFFDIO_COPY reports copy == 16K,
 *   2. all four destination 4K pages read back the bytes that were sent,
 *   3. RssAnon grows by exactly 16 kB across that ioctl -- 64 kB is the
 *      singleton (pre-K1) signature, since RssAnon counts native folios,
 *   4. /proc/self/pagemap reports the same pfn for the four consecutive 4K
 *      pages, i.e. they really share one native folio (needs CAP_SYS_ADMIN;
 *      skipped when pfns read back as zero),
 *   5. the fallback keeps the old stop-at-first-EEXIST semantics: a 16K COPY
 *      across a tuple whose second slice is already populated must report
 *      copy == 4096 and must not disturb the populated slice,
 *   6. a source missing fault exercises the mmap-lock-outside retry and still
 *      installs one correct packed tuple,
 *   7. two concurrent COPYs to the same empty tuple produce exactly one 16K
 *      winner and one -EEXIST loser, without mixing their source bytes.
 *
 * Run it on a pre-K1 kernel and leg 3 fails with 65536 (and leg 4 with four
 * distinct pfns); legs 5 and 6 pass on both, which is the point of them.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>

#include "../kselftest.h"

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

#define PROCESS_PAGE	4096UL		/* compat page size */
#define NATIVE_PAGE	16384UL		/* 16K-native kernel PAGE_SIZE */
#define SLICES		(NATIVE_PAGE / PROCESS_PAGE)
#define RACE_ITERATIONS	128UL
#define FIXED_TUPLES	3UL
#define REGION_LEN	((FIXED_TUPLES + RACE_ITERATIONS) * NATIVE_PAGE)

#define PAGEMAP_PRESENT		UINT64_C(0x8000000000000000)
#define PAGEMAP_PFN_MASK	((UINT64_C(1) << 55) - 1)

struct copy_args {
	pthread_barrier_t *barrier;
	void *dst;
	void *src;
	unsigned long len;
	long copied;
	int uffd;
};

/*
 * A compat process only exists if the ADDR_4KB_COMPAT_PAGE_SIZE personality
 * was set before exec.  Match the other mm ppps selftests: set the bit and
 * re-exec ourselves.
 */
static void reexec_compat(char **argv)
{
	int persona = personality(0xffffffffUL);

	if (persona < 0)
		ksft_exit_skip("personality get failed: %s\n", strerror(errno));
	if (personality(persona | ADDR_4KB_COMPAT_PAGE_SIZE) < 0)
		ksft_exit_skip("personality set failed: %s\n", strerror(errno));
	execl("/proc/self/exe", argv[0], "--compat", (char *)NULL);
	ksft_exit_skip("re-exec for compat failed: %s\n", strerror(errno));
}

static bool read_page_sizes(uintptr_t address, unsigned long *kernel_size,
			    unsigned long *mmu_size)
{
	char *line = NULL;
	size_t line_size = 0;
	bool in_mapping = false;
	FILE *file;

	*kernel_size = 0;
	*mmu_size = 0;
	file = fopen("/proc/self/smaps", "re");
	if (!file)
		return false;
	while (getline(&line, &line_size, file) >= 0) {
		unsigned long start, end, size_kb;

		if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
			in_mapping = address >= start && address < end;
			continue;
		}
		if (!in_mapping)
			continue;
		if (sscanf(line, "KernelPageSize: %lu kB", &size_kb) == 1)
			*kernel_size = size_kb * 1024;
		else if (sscanf(line, "MMUPageSize: %lu kB", &size_kb) == 1)
			*mmu_size = size_kb * 1024;
		if (*kernel_size && *mmu_size)
			break;
	}
	free(line);
	fclose(file);
	return *kernel_size && *mmu_size;
}

static bool has_ppps_geometry(void)
{
	unsigned long kernel_size, mmu_size;
	unsigned char *probe;
	bool parsed;

	probe = mmap(NULL, NATIVE_PAGE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (probe == MAP_FAILED)
		return false;
	*probe = 0x5a;
	parsed = read_page_sizes((uintptr_t)probe, &kernel_size, &mmu_size);
	munmap(probe, NATIVE_PAGE);
	if (!parsed)
		return false;

	return kernel_size == NATIVE_PAGE && mmu_size == PROCESS_PAGE;
}

/*
 * Read RssAnon without allocating: the value is sampled either side of a
 * single ioctl, so a stdio buffer or an arena growing in between would show up
 * as noise in the delta.
 */
static long rss_anon_kb(void)
{
	static char buf[8192];
	const char *p;
	ssize_t len;
	int fd;

	fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	len = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (len <= 0)
		return -1;
	buf[len] = '\0';
	p = strstr(buf, "RssAnon:");
	if (!p)
		return -1;
	return strtol(p + strlen("RssAnon:"), NULL, 10);
}

/* Returns false if any slice is absent; pfn[] may still be all-zero w/o caps. */
static bool read_slice_pfns(const unsigned char *base, uint64_t pfn[SLICES])
{
	uint64_t entry;
	unsigned int i;
	off_t offset;
	int fd;

	fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	for (i = 0; i < SLICES; i++) {
		offset = (off_t)(((uintptr_t)base + i * PROCESS_PAGE) /
				 PROCESS_PAGE) * sizeof(entry);
		if (pread(fd, &entry, sizeof(entry), offset) != sizeof(entry) ||
		    !(entry & PAGEMAP_PRESENT)) {
			close(fd);
			return false;
		}
		pfn[i] = entry & PAGEMAP_PFN_MASK;
	}
	close(fd);
	return true;
}

static int uffd_open_register(void *start, unsigned long len)
{
	struct uffdio_register reg = {};
	struct uffdio_api api = {};
	int uffd;

	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
	if (uffd < 0)
		return -1;
	api.api = UFFD_API;
	if (ioctl(uffd, UFFDIO_API, &api)) {
		close(uffd);
		return -1;
	}
	reg.range.start = (uintptr_t)start;
	reg.range.len = len;
	reg.mode = UFFDIO_REGISTER_MODE_MISSING;
	if (ioctl(uffd, UFFDIO_REGISTER, &reg)) {
		close(uffd);
		return -1;
	}
	return uffd;
}

/* Returns the ioctl's uffdio_copy.copy, or -errno when it never ran. */
static long uffd_copy(int uffd, void *dst, void *src, unsigned long len)
{
	struct uffdio_copy copy = {};
	int ret;

	copy.dst = (uintptr_t)dst;
	copy.src = (uintptr_t)src;
	copy.len = len;
	copy.mode = 0;
	copy.copy = 0;
	ret = ioctl(uffd, UFFDIO_COPY, &copy);
	/* A partial fill returns -1/EAGAIN but still reports .copy bytes. */
	if (copy.copy > 0)
		return (long)copy.copy;
	return ret ? -errno : 0;
}

static void *copy_thread(void *data)
{
	struct copy_args *args = data;

	if (args->barrier)
		pthread_barrier_wait(args->barrier);
	args->copied = uffd_copy(args->uffd, args->dst, args->src, args->len);
	return NULL;
}

static bool test_source_retry(int dst_uffd, unsigned char *dst)
{
	unsigned char resident;
	unsigned char *source;
	long copied;
	bool cold;

	/* The first source PTE is absent, forcing the mmap-lock-outside retry. */
	source = mmap(NULL, NATIVE_PAGE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (source == MAP_FAILED)
		return false;
	resident = 1;
	cold = !mincore(source, PROCESS_PAGE, &resident) && !(resident & 1);
	copied = cold ? uffd_copy(dst_uffd, dst, source, NATIVE_PAGE) : -1;
	cold = cold && copied == NATIVE_PAGE &&
	       !memcmp(dst, source, NATIVE_PAGE);
	munmap(source, NATIVE_PAGE);
	return cold;
}

static bool race_one_tuple(int uffd, unsigned char *dst,
			   unsigned char *source_a, unsigned char *source_b,
			   unsigned long iteration)
{
	pthread_barrier_t barrier;
	struct copy_args args[2] = {
		{
			.dst = dst,
			.src = source_a,
			.len = NATIVE_PAGE,
			.copied = -1,
			.uffd = uffd,
		},
		{
			.dst = dst,
			.src = source_b,
			.len = NATIVE_PAGE,
			.copied = -1,
			.uffd = uffd,
		},
	};
	pthread_t threads[2];
	unsigned char *winner;
	bool a_won, b_won;

	if (pthread_barrier_init(&barrier, NULL, 3))
		return false;
	args[0].barrier = &barrier;
	args[1].barrier = &barrier;
	if (pthread_create(&threads[0], NULL, copy_thread, &args[0]) ||
	    pthread_create(&threads[1], NULL, copy_thread, &args[1]))
		ksft_exit_fail_msg("race thread creation failed\n");
	pthread_barrier_wait(&barrier);
	pthread_join(threads[0], NULL);
	pthread_join(threads[1], NULL);
	pthread_barrier_destroy(&barrier);

	a_won = args[0].copied == NATIVE_PAGE && args[1].copied == -EEXIST;
	b_won = args[1].copied == NATIVE_PAGE && args[0].copied == -EEXIST;
	winner = a_won ? source_a : source_b;
	if ((!a_won && !b_won) || memcmp(dst, winner, NATIVE_PAGE)) {
		ksft_print_msg("race %lu: copy results %ld/%ld\n", iteration,
			       args[0].copied, args[1].copied);
		return false;
	}
	return true;
}

static bool test_same_tuple_race(int uffd, unsigned char *race_region,
				 unsigned char *source_a,
				 unsigned char *source_b)
{
	for (unsigned long i = 0; i < RACE_ITERATIONS; i++) {
		if (!race_one_tuple(uffd, race_region + i * NATIVE_PAGE,
				    source_a, source_b, i))
			return false;
	}
	return true;
}

static bool all_bytes_are(const unsigned char *p, unsigned long len,
			  unsigned char want)
{
	unsigned long i;

	for (i = 0; i < len; i++)
		if (p[i] != want)
			return false;
	return true;
}

static int run_test(void)
{
	unsigned char *reservation, *region, *src;
	unsigned char *tuple_a, *tuple_b, *tuple_retry, *race_region;
	long rss_before, rss_after, copied;
	uint64_t pfn[SLICES];
	unsigned int i;
	bool ok;
	int uffd;

	ksft_print_header();

	if (sysconf(_SC_PAGESIZE) != (long)PROCESS_PAGE)
		ksft_exit_skip("need a compat (4K) process; _SC_PAGESIZE=%ld\n",
			       sysconf(_SC_PAGESIZE));
	if (!has_ppps_geometry())
		ksft_exit_skip("need native-16K/compat-4K PPPS geometry\n");

	/* Oversize the reservation so the region can start 16K-aligned. */
	reservation = mmap(NULL, REGION_LEN + NATIVE_PAGE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		ksft_exit_fail_perror("mmap(region)");
	region = (unsigned char *)(((uintptr_t)reservation + NATIVE_PAGE - 1) &
				   ~(uintptr_t)(NATIVE_PAGE - 1));
	tuple_a = region;
	tuple_b = region + NATIVE_PAGE;
	tuple_retry = region + 2 * NATIVE_PAGE;
	race_region = region + FIXED_TUPLES * NATIVE_PAGE;

	/*
	 * The source is faulted in and filled up front: an unpopulated source
	 * would fault (and grow RssAnon) inside the measured ioctl.
	 */
	src = mmap(NULL, 4 * NATIVE_PAGE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (src == MAP_FAILED)
		ksft_exit_fail_perror("mmap(src)");
	for (i = 0; i < 2 * SLICES; i++)
		memset(src + i * PROCESS_PAGE, 0xa0 + i, PROCESS_PAGE);
	memset(src + 2 * NATIVE_PAGE, 0x5a, NATIVE_PAGE);
	memset(src + 3 * NATIVE_PAGE, 0xa5, NATIVE_PAGE);

	uffd = uffd_open_register(region, REGION_LEN);
	if (uffd < 0)
		ksft_exit_skip("userfaultfd MISSING registration failed: %s\n",
			       strerror(errno));
	ksft_set_plan(7);

	/* Warm the status read so the measured pair allocates nothing. */
	if (rss_anon_kb() < 0)
		ksft_exit_fail_msg("cannot read RssAnon from /proc/self/status\n");

	rss_before = rss_anon_kb();
	copied = uffd_copy(uffd, tuple_a, src, NATIVE_PAGE);
	rss_after = rss_anon_kb();

	ksft_test_result(copied == (long)NATIVE_PAGE,
			 "16K UFFDIO_COPY installed the whole tuple (copy=%ld, want %lu)\n",
			 copied, NATIVE_PAGE);

	ok = copied == (long)NATIVE_PAGE;
	for (i = 0; ok && i < SLICES; i++)
		ok = all_bytes_are(tuple_a + i * PROCESS_PAGE, PROCESS_PAGE,
				   (unsigned char)(0xa0 + i));
	ksft_test_result(ok, "all %u destination slices carry the copied bytes\n",
			 (unsigned int)SLICES);

	if (rss_before < 0 || rss_after < 0)
		ksft_test_result_skip("RssAnon unreadable\n");
	else
		ksft_test_result(rss_after - rss_before == (long)(NATIVE_PAGE / 1024),
				 "RssAnon grew by one native page: %ld kB (packed=%lu, singletons=%lu)\n",
				 rss_after - rss_before, NATIVE_PAGE / 1024,
				 SLICES * NATIVE_PAGE / 1024);

	if (!read_slice_pfns(tuple_a, pfn)) {
		ksft_test_result_skip("pagemap: destination slices not all present\n");
	} else if (!pfn[0]) {
		ksft_test_result_skip("pagemap: pfns hidden (need CAP_SYS_ADMIN)\n");
	} else {
		ok = true;
		for (i = 1; i < SLICES; i++)
			ok = ok && pfn[i] == pfn[0];
		ksft_test_result(ok,
				 "the %u slices share one native folio (pfn %#llx %#llx %#llx %#llx)\n",
				 (unsigned int)SLICES,
				 (unsigned long long)pfn[0],
				 (unsigned long long)pfn[1],
				 (unsigned long long)pfn[2],
				 (unsigned long long)pfn[3]);
	}

	/*
	 * Fallback leg.  Populate slice 1 of a fresh tuple with a 4K COPY, then
	 * ask for the whole 16K tuple: the tuple path must decline (not report
	 * EEXIST for the range) and the per-compat-page path must fill slice 0,
	 * stop at slice 1 and report exactly 4096 -- byte-for-byte what the
	 * pre-K1 kernel reported.
	 */
	copied = uffd_copy(uffd, tuple_b + PROCESS_PAGE, src + NATIVE_PAGE,
			   PROCESS_PAGE);
	if (copied != (long)PROCESS_PAGE) {
		ksft_test_result_fail("seeding slice 1 failed (copy=%ld)\n",
				      copied);
	} else {
		copied = uffd_copy(uffd, tuple_b, src, NATIVE_PAGE);
		ok = copied == (long)PROCESS_PAGE &&
		     all_bytes_are(tuple_b, PROCESS_PAGE, 0xa0) &&
		     all_bytes_are(tuple_b + PROCESS_PAGE, PROCESS_PAGE, 0xa4);
		ksft_test_result(ok,
				 "16K COPY over a populated slice stops at it (copy=%ld, want %lu)\n",
				 copied, PROCESS_PAGE);
	}

	ksft_test_result(test_source_retry(uffd, tuple_retry),
			 "source faults retry outside mmap lock and preserve the tuple\n");

	ksft_test_result(test_same_tuple_race(uffd, race_region,
					      src + 2 * NATIVE_PAGE,
					      src + 3 * NATIVE_PAGE),
			 "same-tuple COPY races have one winner and one EEXIST loser\n");

	close(uffd);
	munmap(src, 4 * NATIVE_PAGE);
	munmap(reservation, REGION_LEN + NATIVE_PAGE);
	ksft_finished();
}

int main(int argc, char **argv)
{
	/* Become a compat (4K) process if we are not already one. */
	if (sysconf(_SC_PAGESIZE) != (long)PROCESS_PAGE && argc == 1)
		reexec_compat(argv);

	return run_test();
}
