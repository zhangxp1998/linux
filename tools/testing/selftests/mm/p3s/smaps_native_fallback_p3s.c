// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
/*
 * A native 16K process accounts a file page in smaps from its mapcount, as on
 * a native kernel: every PTE that maps the page, native or 4K compat, counts
 * as one sharer of the whole page, and the native reader never walks the
 * reverse map. Compat views use the same total PTE mapcount, with each
 * contribution capped at 4K. All mappings use initialized, owned memfd pages and
 * pipe-synchronized peers.
 */
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define P3S_HARNESS_ROLE worker_role
#define P3S_HARNESS_DEFAULT_NATIVE 1
#include "p3s_harness.h"
#include "vm_util.h"

#define LENGTH (8 * NATIVE_PAGE_SIZE)
#define TARGET (3 * NATIVE_PAGE_SIZE)

struct stats {
	unsigned long rss, pss, shared, private;
};

struct view {
	pid_t pid;
	uintptr_t address;
};

struct worker {
	pid_t pid;
	int command, reply;
	struct view view;
};

static void worker_require(bool ok, const char *what)
{
	if (!ok) {
		perror(what);
		_exit(1);
	}
}

static bool receive(int fd, void *buf, size_t len)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	return poll(&pfd, 1, 10000) == 1 && read_full(fd, buf, len);
}

static bool get_stats(struct view view, struct stats *s)
{
	char path[64], *line = NULL;
	unsigned long lo, hi, value;
	size_t cap = 0;
	bool found = false;
	FILE *f;

	memset(s, 0, sizeof(*s));
	snprintf(path, sizeof(path), "/proc/%d/smaps", view.pid);
	f = fopen(path, "re");
	if (!f)
		return false;
	while (getline(&line, &cap, f) > 0) {
		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2) {
			if (found)
				break;
			found = view.address >= lo && view.address < hi;
		} else if (found) {
			if (sscanf(line, "Rss: %lu kB", &value) == 1)
				s->rss = value;
			else if (sscanf(line, "Pss: %lu kB", &value) == 1)
				s->pss = value;
			else if (sscanf(line, "Shared_Clean: %lu kB", &value) == 1 ||
				 sscanf(line, "Shared_Dirty: %lu kB", &value) == 1)
				s->shared += value;
			else if (sscanf(line, "Private_Clean: %lu kB", &value) == 1 ||
				 sscanf(line, "Private_Dirty: %lu kB", &value) == 1)
				s->private += value;
		}
	}
	free(line);
	fclose(f);
	return found;
}

static bool exclusive(struct view view, unsigned long page_size, bool expected)
{
	uint64_t entry = 0;
	char path[64];
	int fd;
	bool ok;

	snprintf(path, sizeof(path), "/proc/%d/pagemap", view.pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return false;
	ok = pread(fd, &entry, sizeof(entry), view.address / page_size * 8) == 8;
	close(fd);
	return ok && (entry & PM_PRESENT) && !!(entry & PM_MMAP_EXCLUSIVE) == expected;
}

/*
 * Native Pss of the mapping when its page at TARGET has @mappers mappings and
 * the other pages are private: smaps sums Pss with 12 fractional bits and
 * prints kB.
 */
static unsigned long native_pss(int mappers)
{
	uint64_t pss = (uint64_t)(LENGTH - NATIVE_PAGE_SIZE) << 12;

	pss += ((uint64_t)NATIVE_PAGE_SIZE << 12) / mappers;
	return pss >> 22;
}

static unsigned long native_shared(int mappers)
{
	return mappers > 1 ? NATIVE_PAGE_SIZE / 1024 : 0;
}

static bool native_matches(unsigned char *base, unsigned long pss,
			   unsigned long shared)
{
	struct stats s;
	struct view view = { getpid(), (uintptr_t)base };
	struct view target = { getpid(), (uintptr_t)base + TARGET };
	bool ok = get_stats(view, &s);

	if (!ok)
		return false;
	ok = s.rss == LENGTH / 1024 && s.pss == pss &&
		s.shared == shared && s.private == LENGTH / 1024 - shared &&
		exclusive(view, NATIVE_PAGE_SIZE, true) &&
		exclusive(target, NATIVE_PAGE_SIZE, !shared);
	if (!ok)
		fprintf(stderr, "native: Rss=%lu Pss=%lu Shared=%lu Private=%lu kB, expected Pss=%lu Shared=%lu\n",
			s.rss, s.pss, s.shared, s.private, pss, shared);
	return ok;
}

static int worker_main(int fd, int input, int output, int slice, int nr,
		       bool cow, bool native)
{
	size_t length = native ? NATIVE_PAGE_SIZE :
		(cow ? 2 * NATIVE_PAGE_SIZE : nr * PROCESS_PAGE_SIZE);
	unsigned char *map;
	struct view view;
	int release = -1, status;
	pid_t descendant = 0;
	char cmd;

	alarm(40);
	if ((unsigned long)getpagesize() !=
	    (native ? NATIVE_PAGE_SIZE : PROCESS_PAGE_SIZE))
		return 1;
	map = mmap(NULL, length, PROT_READ | PROT_WRITE,
		   cow ? MAP_PRIVATE : MAP_SHARED, fd,
		   TARGET + slice * PROCESS_PAGE_SIZE);
	worker_require(map != MAP_FAILED, "worker mmap");
	/* Compat VMAs reject THP advice because they are already non-THP. */
	if (madvise(map, length, MADV_NOHUGEPAGE))
		worker_require(!native && errno == EINVAL, "worker no THP");
	for (int i = 0; i < nr; i++)
		worker_require(*(volatile unsigned char *)(map + i * PROCESS_PAGE_SIZE) ==
			       0x40 + TARGET / PROCESS_PAGE_SIZE + slice + i,
			       "owned data");
	/* Force vma_needs_copy(): clean file PTEs must be copied on fork. */
	if (cow)
		map[NATIVE_PAGE_SIZE] = 0xee;
	view = (struct view) { getpid(), (uintptr_t)map };
	worker_require(write_full(output, &view, sizeof(view)), "worker ready");
	while (read_full(input, &cmd, 1)) {
		if (cmd == 'E')
			_exit(0); /* Exercise exit's teardown, not an explicit munmap. */
		if (cmd == 'Q')
			break;
		if (cmd == 'R') {
			void *dest = mmap(NULL, length, PROT_NONE,
					 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

			worker_require(dest != MAP_FAILED, "reserve mremap destination");
			map = mremap(map, length, length,
				     MREMAP_MAYMOVE | MREMAP_FIXED, dest);
			worker_require(map == dest, "move mapping");
		} else if (cmd == 'P') {
			worker_require(length > PROCESS_PAGE_SIZE && !cow,
				       "partial unmap size");
			worker_require(!munmap(map + length - PROCESS_PAGE_SIZE,
					       PROCESS_PAGE_SIZE),
				       "partial unmap");
			length -= PROCESS_PAGE_SIZE;
		} else if (cmd == 'F') {
			int hold[2], ready[2];

			worker_require(cow && !descendant && !pipe(hold) &&
				       !pipe(ready), "fork setup");
			descendant = fork();
			worker_require(descendant >= 0, "compat fork");
			if (!descendant) {
				close(hold[1]);
				close(ready[0]);
				close(input);
				close(output);
				/* Do not fault the file PTEs: fork must have copied them. */
				worker_require(write_full(ready[1], "r", 1),
					       "descendant ready");
				close(ready[1]);
				read_full(hold[0], &cmd, 1);
				_exit(0);
			}
			close(hold[0]);
			close(ready[1]);
			worker_require(receive(ready[0], &cmd, 1),
				       "wait descendant");
			close(ready[0]);
			release = hold[1];
			worker_require(!munmap(map, length),
				       "unmap original after fork");
			length = 0;
			view.pid = descendant;
		} else {
			return 1;
		}
		view.address = (uintptr_t)map;
		worker_require(write_full(output, &view, sizeof(view)),
			       "worker acknowledgement");
	}
	if (length)
		worker_require(!munmap(map, length), "worker unmap");
	if (descendant) {
		close(release);
		worker_require(waitpid(descendant, &status, 0) == descendant &&
			       WIFEXITED(status) && !WEXITSTATUS(status),
			       "descendant exit");
	}
	return 0;
}

static bool start_worker_mode(int fd, int slice, int nr, bool cow,
			      bool native, struct worker *w)
{
	int command[2], reply[2];

	memset(w, 0, sizeof(*w));
	w->command = -1;
	w->reply = -1;
	if (pipe(command))
		return false;
	if (pipe(reply)) {
		close(command[0]);
		close(command[1]);
		return false;
	}
	w->pid = fork();
	if (w->pid < 0) {
		close(command[0]);
		close(command[1]);
		close(reply[0]);
		close(reply[1]);
		return false;
	}
	if (!w->pid) {
		char args[7][24];
		int values[] = { fd, command[0], reply[1], slice, nr, cow, native };

		close(command[1]);
		close(reply[0]);
		for (unsigned int i = 0; i < 7; i++)
			snprintf(args[i], sizeof(args[i]), "%d", values[i]);
		p3s_execl(!native, NULL, "--worker", args[0], args[1], args[2],
			   args[3], args[4], args[5], args[6], NULL);
		_exit(1);
	}
	close(command[0]);
	close(reply[1]);
	w->command = command[1];
	w->reply = reply[0];
	if (!receive(w->reply, &w->view, sizeof(w->view))) {
		close(w->command);
		close(w->reply);
		waitpid(w->pid, NULL, 0);
		return false;
	}
	return true;
}

static bool start_worker(int fd, int slice, int nr, bool cow, struct worker *w)
{
	return start_worker_mode(fd, slice, nr, cow, false, w);
}

static bool command_worker(struct worker *w, char cmd)
{
	return write_full(w->command, &cmd, 1) &&
	       receive(w->reply, &w->view, sizeof(w->view));
}

static bool stop_worker(struct worker *w, char cmd)
{
	int status;
	bool wrote = write_full(w->command, &cmd, 1);

	close(w->command);
	close(w->reply);
	return wrote && waitpid(w->pid, &status, 0) == w->pid &&
	       WIFEXITED(status) && !WEXITSTATUS(status);
}

/* Use the same native-page PSS share on each side, capped per PTE. */
static unsigned long compat_pss(unsigned int mapcount, unsigned int nr)
{
	uint64_t pss = (uint64_t)NATIVE_PAGE_SIZE << 12;

	pss /= mapcount;
	if (pss > (PROCESS_PAGE_SIZE << 12))
		pss = PROCESS_PAGE_SIZE << 12;
	return (pss * nr) >> 22;
}

static bool compat_matches(struct worker *w, int nr, int pss, bool shared)
{
	struct stats s;
	bool ok = get_stats(w->view, &s);

	if (!ok)
		return false;
	ok = s.rss == nr * 4UL && s.pss == (unsigned long)pss &&
	     s.shared == (shared ? nr * 4UL : 0) &&
	     s.private == (shared ? 0 : nr * 4UL) &&
	     exclusive(w->view, PROCESS_PAGE_SIZE, !shared);
	if (!ok)
		fprintf(stderr, "compat: Rss=%lu Pss=%lu Shared=%lu Private=%lu kB, expected %d slices Pss=%d Shared=%d\n",
			s.rss, s.pss, s.shared, s.private, nr, pss, shared);
	return ok;
}

static bool mixed_matches(unsigned char *base, int native, int compat)
{
	return native_matches(base, native_pss(native + compat),
			      native_shared(native + compat));
}

/* Run the same sharing transition with a different number of native peers. */
static bool single_compat_count(int fd, unsigned char *base, int n)
{
	struct worker natives[31], one, two;
	bool ok = true;

	for (int i = 0; i < n - 1; i++) {
		if (!start_worker_mode(fd, 0, 1, false, true, &natives[i])) {
			for (int j = 0; j < i; j++)
				stop_worker(&natives[j], 'E');
			return false;
		}
	}
	/* Use the last slice: the native formula must not assume slice zero. */
	if (!start_worker(fd, 3, 1, false, &one)) {
		for (int i = 0; i < n - 1; i++)
			stop_worker(&natives[i], 'E');
		return false;
	}
	ok &= mixed_matches(base, n, 1);
	ok &= compat_matches(&one, 1, compat_pss(n + 1, 1), true);
	ok &= command_worker(&one, 'R');
	ok &= mixed_matches(base, n, 1);
	if (!start_worker(fd, 3, 1, false, &two)) {
		stop_worker(&one, 'E');
		for (int i = 0; i < n - 1; i++)
			stop_worker(&natives[i], 'E');
		return false;
	}
	ok &= mixed_matches(base, n, 2);
	ok &= compat_matches(&one, 1, compat_pss(n + 2, 1), true);
	ok &= compat_matches(&two, 1, compat_pss(n + 2, 1), true);
	ok &= stop_worker(&two, 'E');
	ok &= mixed_matches(base, n, 1);
	ok &= stop_worker(&one, 'E');
	ok &= native_matches(base, native_pss(n), native_shared(n));
	for (int i = 0; i < n - 1; i++)
		ok &= stop_worker(&natives[i], 'E');
	ok &= native_matches(base, 128, 0);
	return ok;
}

static bool four_peers_on_slices(int fd, unsigned char *base, bool disjoint)
{
	struct worker w[4];
	bool ok = true;

	for (int i = 0; i < 4; i++) {
		if (!start_worker(fd, disjoint ? i : 0, 1, false, &w[i])) {
			for (int j = 0; j < i; j++)
				stop_worker(&w[j], 'E');
			return false;
		}
	}
	ok &= native_matches(base, native_pss(5), native_shared(5));
	if (!disjoint)
		for (int i = 0; i < 4; i++)
			ok &= compat_matches(&w[i], 1, compat_pss(5, 1), true);
	for (int left = 3; left >= 0; left--) {
		ok &= stop_worker(&w[left], 'E');
		if (!disjoint)
			ok &= native_matches(base, native_pss(1 + left),
					     native_shared(1 + left));
	}
	ok &= native_matches(base, 128, 0);
	return ok;
}

static bool forked_private_file_pte(int fd, unsigned char *base, int nr)
{
	struct worker w;
	int count = nr + 1;
	bool ok = true;

	if (!start_worker(fd, 0, nr, true, &w))
		return false;
	ok &= native_matches(base, native_pss(count), native_shared(count));
	ok &= command_worker(&w, 'F');
	ok &= native_matches(base, native_pss(count), native_shared(count));
	ok &= stop_worker(&w, 'Q');
	ok &= native_matches(base, 128, 0);
	return ok;
}

static bool compat_only_exclusive(int fd, unsigned char *base, int slice)
{
	struct worker w;
	bool ok;

	if (munmap(base, LENGTH))
		return false;
	if (!start_worker(fd, slice, 1, false, &w))
		return false;
	ok = compat_matches(&w, 1, 4, false);
	ok &= stop_worker(&w, 'E');
	return ok;
}

FIXTURE(smaps_native)
{
	int fd;
	unsigned char *base;
};

FIXTURE_SETUP(smaps_native)
{
	self->fd = memfd_create("smaps-native-fallback", 0);
	ASSERT_GE(self->fd, 0);
	ASSERT_EQ(0, ftruncate(self->fd, LENGTH));
	self->base = mmap(NULL, LENGTH, PROT_READ | PROT_WRITE, MAP_SHARED,
			  self->fd, 0);
	ASSERT_NE(self->base, MAP_FAILED);
	ASSERT_EQ(0, madvise(self->base, LENGTH, MADV_NOHUGEPAGE));
	for (unsigned int i = 0; i < LENGTH / PROCESS_PAGE_SIZE; i++)
		memset(self->base + i * PROCESS_PAGE_SIZE, 0x40 + i,
		       PROCESS_PAGE_SIZE);
}

FIXTURE_TEARDOWN(smaps_native)
{
	if (self->base != MAP_FAILED)
		munmap(self->base, LENGTH);
	if (self->fd >= 0)
		close(self->fd);
}

TEST_F(smaps_native, native_only)
{
	EXPECT_TRUE(native_matches(self->base, 128, 0));
}

TEST_F(smaps_native, one_native_and_one_compat)
{
	EXPECT_TRUE(single_compat_count(self->fd, self->base, 1));
}

TEST_F(smaps_native, two_native_and_one_compat)
{
	EXPECT_TRUE(single_compat_count(self->fd, self->base, 2));
}

TEST_F(smaps_native, three_native_and_one_compat)
{
	EXPECT_TRUE(single_compat_count(self->fd, self->base, 3));
}

TEST_F_TIMEOUT(smaps_native, thirty_two_native_and_one_compat, 120)
{
	EXPECT_TRUE(single_compat_count(self->fd, self->base, 32));
}

TEST_F(smaps_native, four_peers_same_slice)
{
	EXPECT_TRUE(four_peers_on_slices(self->fd, self->base, false));
}

TEST_F(smaps_native, four_peers_disjoint_slices)
{
	EXPECT_TRUE(four_peers_on_slices(self->fd, self->base, true));
}

TEST_F(smaps_native, forked_private_file_tuple)
{
	EXPECT_TRUE(forked_private_file_pte(self->fd, self->base, 4));
}

TEST_F(smaps_native, forked_private_file_single_pte)
{
	EXPECT_TRUE(forked_private_file_pte(self->fd, self->base, 1));
}

TEST_F(smaps_native, compat_only_slice_zero_exclusive)
{
	EXPECT_TRUE(compat_only_exclusive(self->fd, self->base, 0));
}

TEST_F(smaps_native, compat_only_slice_one_exclusive)
{
	EXPECT_TRUE(compat_only_exclusive(self->fd, self->base, 1));
}

TEST_F(smaps_native, compat_only_slice_two_exclusive)
{
	EXPECT_TRUE(compat_only_exclusive(self->fd, self->base, 2));
}

TEST_F(smaps_native, compat_only_slice_three_exclusive)
{
	EXPECT_TRUE(compat_only_exclusive(self->fd, self->base, 3));
}

static int worker_role(int argc, char **argv)
{
	if (argc != 9 || strcmp(argv[1], "--worker"))
		return -1;
	return worker_main(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
			   atoi(argv[5]), atoi(argv[6]), atoi(argv[7]),
			   atoi(argv[8]));
}

P3S_HARNESS_4KB_MAIN
