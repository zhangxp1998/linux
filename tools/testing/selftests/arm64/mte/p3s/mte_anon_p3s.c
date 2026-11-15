// SPDX-License-Identifier: GPL-2.0
/*
 * MTE tags on a 4KB process's packed anonymous tuples survive a punched hole,
 * fork COW, pageout, a same-phase mremap, partial PROT_MTE, private shmem COW,
 * UFFDIO_COPY into a hole, and remote ptrace PEEKMTETAGS/POKEMTETAGS across
 * 4KB slices at non-zero folio offsets, while reused hole slices stay untagged.
 */
#define _GNU_SOURCE

#include <asm/hwcap.h>
#include <asm/ptrace.h>
#include <linux/mman.h>
#include <linux/prctl.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include "p3s_slice_test.h"
#include "p3s_harness.h"

#define MTE_GRANULE 16UL
#define TAG_SHIFT 56
#define TAG_MASK (UINT64_C(0xf) << TAG_SHIFT)
#define PTRACE_SLICE_TAGS (2 * (PROCESS_PAGE_SIZE / MTE_GRANULE))

static void *tag_pointer(const void *pointer, unsigned int tag)
{
	return (void *)(((uintptr_t)pointer & ~TAG_MASK) |
			((uint64_t)(tag & 0xf) << TAG_SHIFT));
}

static void store_tag(void *address, unsigned int tag)
{
	void *tagged = tag_pointer(address, tag);

	asm volatile(".arch_extension memtag\n\t"
		     "stg %0, [%0]"
		     : : "r" (tagged) : "memory");
}

static unsigned int load_tag(void *address)
{
	uintptr_t tagged;

	asm volatile(".arch_extension memtag\n\t"
		     "ldg %0, [%1]"
		     : "=r" (tagged) : "r" (address));
	return (tagged >> TAG_SHIFT) & 0xf;
}

static void tag_slice(unsigned char *slice, unsigned int tag)
{
	unsigned long offset;

	for (offset = 0; offset < PROCESS_PAGE_SIZE; offset += MTE_GRANULE)
		store_tag(slice + offset, tag);
}

static bool check_slice_tags(unsigned char *slice, unsigned int expected)
{
	unsigned long offset;

	for (offset = 0; offset < PROCESS_PAGE_SIZE; offset += MTE_GRANULE)
		if (load_tag(slice + offset) != expected)
			return false;
	return true;
}

static unsigned char *map_mte_aligned(size_t size,
				      unsigned char **reservation,
				      size_t *reservation_size)
{
	*reservation_size = size + NATIVE_PAGE_SIZE;
	return map_aligned_prot(size, PROT_READ | PROT_WRITE | PROT_MTE,
				reservation);
}

static void run_mte_pageout(struct __test_metadata *_metadata, bool hole)
{
	unsigned char *reservation = NULL, *base, *tagged;
	size_t reservation_size, expected_swap;
	uint64_t probe = 0;
	bool have_pfn;

	if (!p3s_swap_available())
		SKIP(return, "no active swap");

	base = map_mte_aligned(NATIVE_PAGE_SIZE, &reservation, &reservation_size);
	ASSERT_NE(MAP_FAILED, base);
	memset(base, 0x51, NATIVE_PAGE_SIZE);
	have_pfn = p3s_pfn(base, &probe);
	tag_slice(base + 2 * PROCESS_PAGE_SIZE, 6);
	tagged = tag_pointer(base + 2 * PROCESS_PAGE_SIZE, 6);
	if (hole && madvise(base + 2 * PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			    MADV_DONTNEED)) {
		munmap(reservation, reservation_size);
		ASSERT_EQ(0, -1) TH_LOG("MADV_DONTNEED failed: %s",
					strerror(errno));
	}
	expected_swap = hole ? 3 * PROCESS_PAGE_SIZE : NATIVE_PAGE_SIZE;
	if (!pageout_reaches_swap(base, NATIVE_PAGE_SIZE, expected_swap) ||
	    !p3s_page_swapped(base)) {
		munmap(reservation, reservation_size);
		ASSERT_TRUE(false) TH_LOG("pageout failed to swap %zu bytes",
					  expected_swap);
	}

	if (hole) {
		uint64_t pfn0 = 0, pfn1 = 0, pfn2 = 0, pfn3 = 0;
		unsigned char data = 0, tag = 0;
		bool clean = true, joined = false;
		unsigned int attempt;

		/*
		 * Write the present slices first so the folio is swapped back
		 * in, its swap slot released and the tags saved at pageout
		 * restored, stale hole tags included.  A read fault still uses
		 * the zero page; write the hole afterwards to promote it into
		 * that same tagged folio with zero data and zero tags.
		 */
		base[0] = 0x52;
		base[PROCESS_PAGE_SIZE] = 0x52;
		base[3 * PROCESS_PAGE_SIZE] = 0x52;
		if (!have_pfn) {
			data = base[2 * PROCESS_PAGE_SIZE];
			base[2 * PROCESS_PAGE_SIZE] = 0;
			tag = load_tag(base + 2 * PROCESS_PAGE_SIZE);
			EXPECT_EQ(0, data);
			EXPECT_EQ(0U, tag);
			munmap(reservation, reservation_size);
			SKIP(return, "PFNs unavailable");
		}
		for (attempt = 0; attempt < 100; attempt++) {
			data = base[2 * PROCESS_PAGE_SIZE];
			base[2 * PROCESS_PAGE_SIZE] = 0;
			tag = load_tag(base + 2 * PROCESS_PAGE_SIZE);
			clean = !data && !tag;
			joined = p3s_pfn(base, &pfn0) &&
				p3s_pfn(base + 2 * PROCESS_PAGE_SIZE, &pfn2) &&
				pfn0 == pfn2;
			if (!clean || joined)
				break;
			if (madvise(base + 2 * PROCESS_PAGE_SIZE,
				    PROCESS_PAGE_SIZE, MADV_DONTNEED))
				break;
			usleep(1000);
		}
		p3s_pfn(base + PROCESS_PAGE_SIZE, &pfn1);
		p3s_pfn(base + 3 * PROCESS_PAGE_SIZE, &pfn3);
		if (!clean || !joined) {
			ksft_print_msg("hole-after-pageout data=%#x tag=%u\n",
				       data, tag);
			ksft_print_msg("hole PFNs=%#llx,%#llx,%#llx,%#llx\n",
				       (unsigned long long)pfn0,
				       (unsigned long long)pfn1,
				       (unsigned long long)pfn2,
				       (unsigned long long)pfn3);
		}
		EXPECT_TRUE(clean);
		EXPECT_TRUE(joined);
	} else {
		EXPECT_EQ(0x51, tagged[0]);
		EXPECT_EQ(6U, load_tag(base + 2 * PROCESS_PAGE_SIZE));
	}
	munmap(reservation, reservation_size);
}

FIXTURE(mte_anon) {
};

FIXTURE_SETUP(mte_anon)
{
	unsigned long control;

	if (!(getauxval(AT_HWCAP2) & HWCAP2_MTE))
		SKIP(return, "MTE is not available");
	control = PR_TAGGED_ADDR_ENABLE | PR_MTE_TCF_SYNC |
		  (UINT64_C(0xfffe) << PR_MTE_TAG_SHIFT);
	ASSERT_EQ(0, prctl(PR_SET_TAGGED_ADDR_CTRL, control, 0, 0, 0));
}

FIXTURE_TEARDOWN(mte_anon)
{
}

TEST_F(mte_anon, tuple_sharing_and_tagged_hole_refault)
{
	unsigned char *reservation = NULL, *base, *tagged1, *tagged2;
	size_t reservation_size;
	uint64_t pfn[P3S_SLICES];

	base = map_mte_aligned(NATIVE_PAGE_SIZE, &reservation, &reservation_size);
	ASSERT_NE(MAP_FAILED, base);
	memset(base, 0x31, NATIVE_PAGE_SIZE);
	tag_slice(base + PROCESS_PAGE_SIZE, 5);
	tag_slice(base + 2 * PROCESS_PAGE_SIZE, 6);
	tagged1 = tag_pointer(base + PROCESS_PAGE_SIZE, 5);
	tagged2 = tag_pointer(base + 2 * PROCESS_PAGE_SIZE, 6);
	EXPECT_EQ(0x31, tagged1[0]);
	EXPECT_EQ(0x31, tagged2[0]);
	ASSERT_EQ(0, madvise(base + PROCESS_PAGE_SIZE, 2 * PROCESS_PAGE_SIZE,
			     MADV_DONTNEED));
	/* Read-then-write exercises zero-page COW sibling folio reuse. */
	EXPECT_EQ(0, base[PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0U, load_tag(base + PROCESS_PAGE_SIZE));
	base[PROCESS_PAGE_SIZE] = 0x32;
	EXPECT_EQ(0x32, base[PROCESS_PAGE_SIZE]);
	EXPECT_TRUE(check_slice_tags(base + PROCESS_PAGE_SIZE, 0));
	/* Write-directly exercises anonymous fault sibling folio reuse. */
	base[2 * PROCESS_PAGE_SIZE] = 0x33;
	EXPECT_EQ(0x33, base[2 * PROCESS_PAGE_SIZE]);
	EXPECT_TRUE(all_bytes_are(base + 2 * PROCESS_PAGE_SIZE + 1,
				  PROCESS_PAGE_SIZE - 1, 0));
	EXPECT_TRUE(check_slice_tags(base + 2 * PROCESS_PAGE_SIZE, 0));
	if (!read_pfns(base, pfn)) {
		munmap(reservation, reservation_size);
		ASSERT_TRUE(false);
	}
	if (!pfn[0]) {
		munmap(reservation, reservation_size);
		SKIP(return, "PFNs unavailable");
	}
	EXPECT_TRUE(same_pfn(pfn));
	munmap(reservation, reservation_size);
}

TEST_F(mte_anon, tags_survive_tuple_cow)
{
	unsigned char *reservation = NULL, *base, *tagged;
	size_t reservation_size;
	uint64_t pfn[P3S_SLICES];
	int ready[2], done[2], status;
	char byte;
	pid_t child;
	bool ok;

	base = map_mte_aligned(NATIVE_PAGE_SIZE, &reservation, &reservation_size);
	ASSERT_NE(MAP_FAILED, base);
	ASSERT_EQ(0, pipe(ready));
	if (pipe(done)) {
		close(ready[0]);
		close(ready[1]);
		munmap(reservation, reservation_size);
		ASSERT_EQ(0, -1);
	}
	memset(base, 0x41, NATIVE_PAGE_SIZE);
	tag_slice(base + PROCESS_PAGE_SIZE, 5);
	tagged = tag_pointer(base + PROCESS_PAGE_SIZE, 5);
	child = fork();
	if (!child) {
		close(ready[0]);
		close(done[1]);
		if (write(ready[1], "R", 1) != 1 ||
		    read(done[0], &byte, 1) != 1)
			_exit(1);
		_exit(tagged[0] == 0x41 &&
		      load_tag(base + PROCESS_PAGE_SIZE) == 5 ? 0 : 1);
	}
	close(ready[1]);
	close(done[0]);
	if (child < 0) {
		close(ready[0]);
		close(done[1]);
		munmap(reservation, reservation_size);
		ASSERT_GE(child, 0);
	}
	EXPECT_EQ(1, read(ready[0], &byte, 1));
	base[0] = 0x61;
	EXPECT_EQ(0x41, tagged[0]);
	EXPECT_EQ(5U, load_tag(base + PROCESS_PAGE_SIZE));
	EXPECT_EQ(1, write(done[1], "D", 1));
	EXPECT_EQ(child, waitpid(child, &status, 0));
	EXPECT_TRUE(WIFEXITED(status) && !WEXITSTATUS(status));
	ok = read_pfns(base, pfn);
	close(ready[0]);
	close(done[1]);
	if (!ok) {
		munmap(reservation, reservation_size);
		ASSERT_TRUE(ok);
	}
	if (!pfn[0]) {
		munmap(reservation, reservation_size);
		SKIP(return, "PFNs unavailable");
	}
	EXPECT_TRUE(same_pfn(pfn));
	munmap(reservation, reservation_size);
}

TEST_F(mte_anon, tags_survive_pageout)
{
	run_mte_pageout(_metadata, false);
}

TEST_F(mte_anon, hole_tags_clear_after_pageout)
{
	run_mte_pageout(_metadata, true);
}

TEST_F(mte_anon, shifted_mremap_preserves_tags)
{
	unsigned char *src_reservation = NULL, *src;
	unsigned char *dst_reservation, *dst;
	size_t src_size;
	void *moved;

	src = map_mte_aligned(NATIVE_PAGE_SIZE, &src_reservation, &src_size);
	dst_reservation = mmap(NULL, 3 * NATIVE_PAGE_SIZE, PROT_NONE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (src == MAP_FAILED || dst_reservation == MAP_FAILED) {
		if (src_reservation)
			munmap(src_reservation, src_size);
		if (dst_reservation != MAP_FAILED)
			munmap(dst_reservation, 3 * NATIVE_PAGE_SIZE);
		ASSERT_TRUE(false);
	}
	dst = (unsigned char *)(((uintptr_t)dst_reservation + NATIVE_PAGE_SIZE - 1) &
				~(NATIVE_PAGE_SIZE - 1)) + PROCESS_PAGE_SIZE;
	memset(src, 0x62, NATIVE_PAGE_SIZE);
	tag_slice(src + PROCESS_PAGE_SIZE, 7);
	errno = 0;
	moved = mremap(src, NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	EXPECT_EQ(MAP_FAILED, moved);
	EXPECT_EQ(EINVAL, errno);
	EXPECT_EQ(7U, load_tag(src + PROCESS_PAGE_SIZE));
	dst -= PROCESS_PAGE_SIZE;
	moved = mremap(src, NATIVE_PAGE_SIZE, NATIVE_PAGE_SIZE,
		       MREMAP_MAYMOVE | MREMAP_FIXED, dst);
	EXPECT_EQ(dst, moved);
	if (moved == dst) {
		unsigned char *tagged = tag_pointer(dst + PROCESS_PAGE_SIZE, 7);

		EXPECT_EQ(0x62, tagged[0]);
		EXPECT_EQ(7U, load_tag(dst + PROCESS_PAGE_SIZE));
	}
	munmap(src_reservation, src_size);
	munmap(dst_reservation, 3 * NATIVE_PAGE_SIZE);
}

TEST_F(mte_anon, partial_prot_mte_keeps_mixed_tuple)
{
	unsigned char *reservation = NULL, *base;
	size_t reservation_size;

	reservation_size = 2 * NATIVE_PAGE_SIZE;
	reservation = mmap(NULL, reservation_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, reservation);
	base = (unsigned char *)(((uintptr_t)reservation + NATIVE_PAGE_SIZE - 1) &
				 ~(NATIVE_PAGE_SIZE - 1));
	memset(base, 0x71, NATIVE_PAGE_SIZE);
	EXPECT_EQ(0, mprotect(base, 2 * PROCESS_PAGE_SIZE,
			      PROT_READ | PROT_WRITE | PROT_MTE));
	EXPECT_EQ(0x71, base[0]);
	EXPECT_EQ(0, madvise(base + 3 * PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			     MADV_DONTNEED));
	EXPECT_EQ(0, base[3 * PROCESS_PAGE_SIZE]);
	munmap(reservation, reservation_size);
}

TEST_F(mte_anon, private_shmem_cow_preserves_tags)
{
	unsigned char *shared = MAP_FAILED, *private = MAP_FAILED, *tagged;
	unsigned int i;
	int fd;

	fd = memfd_create("p3s-mte-file-cow", MFD_CLOEXEC);
	ASSERT_GE(fd, 0);
	if (ftruncate(fd, NATIVE_PAGE_SIZE)) {
		close(fd);
		ASSERT_EQ(0, -1);
	}
	shared = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_MTE,
		      MAP_SHARED, fd, 0);
	private = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_MTE,
		       MAP_PRIVATE, fd, 0);
	if (shared == MAP_FAILED || private == MAP_FAILED) {
		if (shared != MAP_FAILED)
			munmap(shared, NATIVE_PAGE_SIZE);
		if (private != MAP_FAILED)
			munmap(private, NATIVE_PAGE_SIZE);
		close(fd);
		ASSERT_TRUE(false);
	}
	memset(shared, 0x31, NATIVE_PAGE_SIZE);
	tag_slice(shared + PROCESS_PAGE_SIZE, 9);
	tagged = tag_pointer(private + PROCESS_PAGE_SIZE, 9);
	EXPECT_EQ(0x31, tagged[0]);
	tagged[0] = 0x79;
	for (i = 0; i < P3S_SLICES; i++)
		if (i != 1)
			private[i * PROCESS_PAGE_SIZE] = 0x70 + i;
	/* Private file COW need not pack all slices into one anonymous folio. */
	EXPECT_EQ(0x79, tagged[0]);
	EXPECT_EQ(9U, load_tag(private + PROCESS_PAGE_SIZE));
	for (i = 0; i < P3S_SLICES; i++)
		if (i != 1)
			EXPECT_EQ(0U, load_tag(private + i * PROCESS_PAGE_SIZE));
	munmap(shared, NATIVE_PAGE_SIZE);
	munmap(private, NATIVE_PAGE_SIZE);
	close(fd);
}

TEST_F(mte_anon, uffd_copy_into_hole_clears_tags)
{
	unsigned char *reservation = NULL, *base, *src = MAP_FAILED;
	size_t reservation_size;
	uint64_t pfn0 = 0, pfn2 = 0;
	int uffd;

	base = map_mte_aligned(NATIVE_PAGE_SIZE, &reservation, &reservation_size);
	ASSERT_NE(MAP_FAILED, base);
	/* Populate the tuple before arming MISSING: nobody serves faults here. */
	memset(base, 0x81, NATIVE_PAGE_SIZE);
	tag_slice(base + PROCESS_PAGE_SIZE, 5);
	tag_slice(base + 2 * PROCESS_PAGE_SIZE, 7);
	uffd = uffd_open_register(base, NATIVE_PAGE_SIZE);
	if (uffd < 0) {
		munmap(reservation, reservation_size);
		SKIP(return, "userfaultfd unavailable");
	}
	src = mmap(NULL, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (src == MAP_FAILED) {
		close(uffd);
		munmap(reservation, reservation_size);
		ASSERT_NE(MAP_FAILED, src);
	}
	memset(src, 0x82, PROCESS_PAGE_SIZE);
	/*
	 * Punch a hole into the tagged tuple and let UFFDIO_COPY fill it.
	 * The copied slice must join the existing folio with zero tags while
	 * the neighbouring slice keeps its own tags.
	 */
	EXPECT_EQ(0, madvise(base + 2 * PROCESS_PAGE_SIZE, PROCESS_PAGE_SIZE,
			     MADV_DONTNEED));
	EXPECT_EQ((long)PROCESS_PAGE_SIZE,
		  uffd_copy(uffd, base + 2 * PROCESS_PAGE_SIZE, src,
			    PROCESS_PAGE_SIZE));
	EXPECT_EQ(0x82, base[2 * PROCESS_PAGE_SIZE]);
	EXPECT_EQ(0U, load_tag(base + 2 * PROCESS_PAGE_SIZE));
	EXPECT_EQ(5U, load_tag(base + PROCESS_PAGE_SIZE));
	if (!p3s_pfn(base, &pfn0)) {
		munmap(src, PROCESS_PAGE_SIZE);
		close(uffd);
		munmap(reservation, reservation_size);
		SKIP(return, "PFNs unavailable");
	}
	ASSERT_TRUE(p3s_pfn(base + 2 * PROCESS_PAGE_SIZE, &pfn2));
	EXPECT_EQ(pfn0, pfn2);
	munmap(src, PROCESS_PAGE_SIZE);
	close(uffd);
	munmap(reservation, reservation_size);
}

TEST_F(mte_anon, ptrace_mte_tags_across_slices)
{
	unsigned char tags[PTRACE_SLICE_TAGS];
	unsigned char *addr = NULL;
	struct iovec iov = {
		.iov_base = tags,
		.iov_len = sizeof(tags),
	};
	int pipefd[2], status, fd;
	unsigned int i;
	bool got_addr;
	pid_t child;

	fd = memfd_create("p3s-mte-ptrace", MFD_CLOEXEC);
	ASSERT_GE(fd, 0);
	if (ftruncate(fd, 2 * NATIVE_PAGE_SIZE) || pipe2(pipefd, O_CLOEXEC)) {
		close(fd);
		ASSERT_TRUE(false);
	}

	child = fork();
	if (!child) {
		unsigned char *reservation = NULL, *full, *mapped;
		unsigned int slice;
		bool ok;

		close(pipefd[0]);
		full = mmap(NULL, 2 * NATIVE_PAGE_SIZE,
			    PROT_READ | PROT_WRITE | PROT_MTE, MAP_SHARED,
			    fd, 0);
		if (full == MAP_FAILED)
			_exit(1);
		for (slice = 0; slice < 2 * P3S_SLICES; slice++) {
			memset(full + slice * PROCESS_PAGE_SIZE, 0x20 + slice,
			       PROCESS_PAGE_SIZE);
			tag_slice(full + slice * PROCESS_PAGE_SIZE, 1 + slice);
		}
		munmap(full, 2 * NATIVE_PAGE_SIZE);

		mapped = map_aligned_prot(2 * PROCESS_PAGE_SIZE, PROT_NONE,
					  &reservation);
		if (mapped == MAP_FAILED ||
		    mmap(mapped, 2 * PROCESS_PAGE_SIZE,
			 PROT_READ | PROT_WRITE | PROT_MTE,
			 MAP_SHARED | MAP_FIXED, fd,
			 3 * PROCESS_PAGE_SIZE) != mapped)
			_exit(1);

		if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0)
			_exit(errno == EPERM ? KSFT_SKIP : 2);
		if (!write_full(pipefd[1], &mapped, sizeof(mapped)))
			_exit(3);
		close(pipefd[1]);
		raise(SIGSTOP);

		ok = check_slice_tags(mapped, 9) &&
			check_slice_tags(mapped + PROCESS_PAGE_SIZE, 10);
		munmap(reservation, 2 * PROCESS_PAGE_SIZE + NATIVE_PAGE_SIZE);
		full = mmap(NULL, 2 * NATIVE_PAGE_SIZE,
			    PROT_READ | PROT_WRITE | PROT_MTE, MAP_SHARED,
			    fd, 0);
		ok = ok && full != MAP_FAILED &&
			check_slice_tags(full, 1) &&
			check_slice_tags(full + 3 * PROCESS_PAGE_SIZE, 9) &&
			check_slice_tags(full + 4 * PROCESS_PAGE_SIZE, 10);
		if (full != MAP_FAILED)
			munmap(full, 2 * NATIVE_PAGE_SIZE);
		close(fd);
		_exit(ok ? 0 : 4);
	}

	close(pipefd[1]);
	if (child < 0) {
		close(pipefd[0]);
		close(fd);
		ASSERT_GE(child, 0);
	}
	got_addr = read_full(pipefd[0], &addr, sizeof(addr));
	close(pipefd[0]);
	ASSERT_EQ(child, waitpid(child, &status, 0));
	if (!got_addr && WIFEXITED(status) && WEXITSTATUS(status) == KSFT_SKIP) {
		close(fd);
		SKIP(return, "ptrace PTRACE_TRACEME unavailable");
	}
	if (!got_addr || !WIFSTOPPED(status)) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		close(fd);
		ASSERT_TRUE(got_addr);
		ASSERT_TRUE(WIFSTOPPED(status));
	}

	memset(tags, 0, sizeof(tags));
	if (ptrace(PTRACE_PEEKMTETAGS, child, addr, &iov)) {
		int saved = errno;

		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		close(fd);
		if (saved == EIO || saved == EOPNOTSUPP)
			SKIP(return, "PTRACE_PEEKMTETAGS unavailable");
		errno = saved;
		ASSERT_EQ(0, -1) TH_LOG("PTRACE_PEEKMTETAGS: %s",
					strerror(saved));
	}
	EXPECT_EQ(sizeof(tags), iov.iov_len);
	for (i = 0; i < PROCESS_PAGE_SIZE / MTE_GRANULE; i++)
		EXPECT_EQ(4U, tags[i]);
	for (i = PROCESS_PAGE_SIZE / MTE_GRANULE; i < sizeof(tags); i++)
		EXPECT_EQ(5U, tags[i]);

	memset(tags, 9, PROCESS_PAGE_SIZE / MTE_GRANULE);
	memset(tags + PROCESS_PAGE_SIZE / MTE_GRANULE, 10,
	       PROCESS_PAGE_SIZE / MTE_GRANULE);
	iov.iov_len = sizeof(tags);
	if (ptrace(PTRACE_POKEMTETAGS, child, addr, &iov)) {
		int saved = errno;

		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		close(fd);
		errno = saved;
		ASSERT_EQ(0, -1) TH_LOG("PTRACE_POKEMTETAGS: %s",
					strerror(saved));
	}
	EXPECT_EQ(sizeof(tags), iov.iov_len);
	if (ptrace(PTRACE_CONT, child, NULL, NULL)) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		close(fd);
		ASSERT_EQ(0, -1);
	}
	ASSERT_EQ(child, waitpid(child, &status, 0));
	EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	close(fd);
}

P3S_HARNESS_4KB_MAIN
