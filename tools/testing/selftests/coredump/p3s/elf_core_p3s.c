// SPDX-License-Identifier: GPL-2.0
/*
 * An ELF core dumped by a 4KB process keeps its 4KB VMAs aligned and intact
 * in PT_LOAD (including all four 4KB slices of a 16KB anonymous VMA), omits a
 * mapping that starts at file offset 4KB, and encodes 4KB page units in
 * NT_FILE.
 */
#define _GNU_SOURCE

#include <elf.h>
#include <limits.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "kselftest_p3s.h"
#include "p3s_harness.h"

#define TEST_VMAS		4
#define TEST_BASE		0x50000000UL
#define TEST_STRIDE		0x10000UL
#define PACKED_TEST_BASE	0x58000000UL
#define FILE_TEST_BASE		0x60000000UL

static bool write_file(const char *path, const char *contents, size_t length)
{
	ssize_t written;
	int fd;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	written = write(fd, contents, length);
	close(fd);
	return written == (ssize_t)length;
}

static bool create_offset_file(const char *path, bool *created)
{
	int fd;

	fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0700);
	if (fd < 0)
		return false;
	*created = true;
	if (fchmod(fd, 0700) || ftruncate(fd, 2 * PROCESS_PAGE_SIZE)) {
		close(fd);
		return false;
	}
	close(fd);
	return true;
}

static void crash_with_default_signal(void)
{
	struct sigaction action = {
		.sa_handler = SIG_DFL,
	};
	sigset_t signals;

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSEGV, &action, NULL))
		_exit(126);
	sigemptyset(&signals);
	sigaddset(&signals, SIGSEGV);
	if (sigprocmask(SIG_UNBLOCK, &signals, NULL))
		_exit(126);
	syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGSEGV);
	_exit(126);
}

static char *read_core_pattern(size_t *length)
{
	char *pattern = NULL;
	size_t capacity = 0;
	ssize_t size;
	FILE *file;

	file = fopen("/proc/sys/kernel/core_pattern", "re");
	if (!file)
		return NULL;
	size = getline(&pattern, &capacity, file);
	fclose(file);
	if (size < 0) {
		free(pattern);
		return NULL;
	}
	*length = size;
	return pattern;
}

static void crash_with_test_vmas(const char *offset_file)
{
	struct rlimit limit = {
		.rlim_cur = RLIM_INFINITY,
		.rlim_max = RLIM_INFINITY,
	};
	unsigned int index;
	void *mapping;
	int fd;

	if (setrlimit(RLIMIT_CORE, &limit) ||
	    prctl(PR_SET_DUMPABLE, 1) ||
	    !write_file("/proc/self/coredump_filter", "0x19\n", 5))
		_exit(120);
	fd = open(offset_file, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		_exit(121);
	mapping = mmap((void *)FILE_TEST_BASE, PROCESS_PAGE_SIZE, PROT_READ,
		       MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, PROCESS_PAGE_SIZE);
	close(fd);
	if (mapping != (void *)FILE_TEST_BASE)
		_exit(122);
	for (index = 0; index < TEST_VMAS; index++) {
		void *target = (void *)(TEST_BASE + index * TEST_STRIDE);

		mapping = mmap(target, PROCESS_PAGE_SIZE, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			       -1, 0);
		if (mapping != target)
			_exit(123);
		memset(mapping, 0x40 + index, PROCESS_PAGE_SIZE);
	}
	mapping = mmap((void *)PACKED_TEST_BASE, NATIVE_PAGE_SIZE,
		       PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
		       -1, 0);
	if (mapping != (void *)PACKED_TEST_BASE)
		_exit(125);
	for (index = 0; index < P3S_SLICES; index++)
		memset((char *)mapping + index * PROCESS_PAGE_SIZE,
		       0x50 + index, PROCESS_PAGE_SIZE);
	crash_with_default_signal();
	_exit(124);
}

struct elf_note_header {
	uint32_t namesz;
	uint32_t descsz;
	uint32_t type;
};

static bool inspect_nt_file(int fd, uint64_t note_offset, uint64_t note_size,
			    bool *found,
			    uint64_t *page_size, uint64_t *file_offset)
{
	off_t end = note_offset + note_size;
	off_t pos = note_offset;

	while (pos + (off_t)sizeof(struct elf_note_header) <= end) {
		struct elf_note_header note;
		off_t desc_pos;
		off_t next;
		uint64_t *desc;
		uint64_t count;
		uint64_t index;

		if (pread(fd, &note, sizeof(note), pos) != sizeof(note))
			return false;
		desc_pos = (pos + sizeof(note) + note.namesz + 3) & ~3ULL;
		next = (desc_pos + note.descsz + 3) & ~3ULL;
		if (desc_pos < pos || next < desc_pos || next > end)
			return false;
		pos = next;
		if (note.type != NT_FILE || note.descsz < 2 * sizeof(*desc))
			continue;
		desc = malloc(note.descsz);
		if (!desc)
			return false;
		if (pread(fd, desc, note.descsz, desc_pos) != (ssize_t)note.descsz) {
			free(desc);
			return false;
		}
		count = desc[0];
		if (count > (note.descsz / sizeof(*desc) - 2) / 3) {
			free(desc);
			return false;
		}
		for (index = 0; index < count; index++) {
			uint64_t *entry = &desc[2 + 3 * index];

			if (entry[0] != FILE_TEST_BASE)
				continue;
			*found = true;
			*page_size = desc[1];
			*file_offset = entry[2];
			break;
		}
		free(desc);
	}
	return true;
}

static bool segment_slice_matches(int fd, off_t offset, unsigned char expected)
{
	unsigned char contents[PROCESS_PAGE_SIZE];
	unsigned long byte;

	if (pread(fd, contents, sizeof(contents), offset) != sizeof(contents))
		return false;
	for (byte = 0; byte < sizeof(contents); byte++)
		if (contents[byte] != expected)
			return false;
	return true;
}

static bool inspect_core(const char *path, unsigned int *found,
			 unsigned int *misaligned, bool *offset_segment_found,
			 uint64_t *offset_dump_size, bool *nt_file_found,
			 uint64_t *note_page_size, uint64_t *note_file_offset,
			 bool *packed_found, bool *contents_match)
{
	Elf64_Ehdr header;
	Elf64_Phdr phdr;
	unsigned int index;
	int fd;

	*found = 0;
	*misaligned = 0;
	*offset_segment_found = false;
	*offset_dump_size = 0;
	*nt_file_found = false;
	*note_page_size = 0;
	*note_file_offset = 0;
	*packed_found = false;
	*contents_match = true;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	if (read(fd, &header, sizeof(header)) != sizeof(header) ||
	    memcmp(header.e_ident, ELFMAG, SELFMAG) ||
	    header.e_phentsize != sizeof(phdr))
		goto error;
	for (index = 0; index < header.e_phnum; index++) {
		unsigned long target_index;

		if (pread(fd, &phdr, sizeof(phdr),
			  header.e_phoff + index * sizeof(phdr)) != sizeof(phdr))
			goto error;
		if (phdr.p_type == PT_NOTE &&
		    !inspect_nt_file(fd, phdr.p_offset, phdr.p_filesz,
				     nt_file_found, note_page_size,
				     note_file_offset))
			goto error;
		if (phdr.p_type == PT_LOAD && phdr.p_vaddr == FILE_TEST_BASE) {
			*offset_segment_found = true;
			*offset_dump_size = phdr.p_filesz;
		}
		if (phdr.p_type == PT_LOAD && phdr.p_vaddr == PACKED_TEST_BASE) {
			unsigned int slice;

			*packed_found = true;
			if (phdr.p_filesz != NATIVE_PAGE_SIZE) {
				*contents_match = false;
			} else {
				for (slice = 0; slice < P3S_SLICES; slice++) {
					if (!segment_slice_matches(fd,
							phdr.p_offset +
							slice * PROCESS_PAGE_SIZE,
							0x50 + slice)) {
						*contents_match = false;
						break;
					}
				}
			}
		}
		if (phdr.p_type != PT_LOAD || phdr.p_vaddr < TEST_BASE ||
		    phdr.p_vaddr >= TEST_BASE + TEST_VMAS * TEST_STRIDE)
			continue;
		target_index = (phdr.p_vaddr - TEST_BASE) / TEST_STRIDE;
		if (phdr.p_vaddr != TEST_BASE + target_index * TEST_STRIDE)
			continue;
		(*found)++;
		if (phdr.p_filesz < PROCESS_PAGE_SIZE ||
		    !segment_slice_matches(fd, phdr.p_offset,
					   0x40 + target_index))
			*contents_match = false;
		if (!phdr.p_align ||
		    phdr.p_offset % phdr.p_align != phdr.p_vaddr % phdr.p_align)
			(*misaligned)++;
	}
	close(fd);
	return true;

error:
	close(fd);
	return false;
}

FIXTURE(elf_core) {
	char offset_path[PATH_MAX];
	char core_path[PATH_MAX];
	char pattern[PATH_MAX];
	char saved_pattern[PATH_MAX];
	size_t saved_pattern_length;
	bool offset_file_created;
	bool restore_pattern;
	bool crash_signaled;
	bool inspected;
	unsigned int found;
	unsigned int misaligned;
	bool offset_segment_found;
	uint64_t offset_dump_size;
	bool nt_file_found;
	uint64_t note_page_size;
	uint64_t note_file_offset;
	bool packed_found;
	bool contents_match;
};

FIXTURE_SETUP(elf_core)
{
}

/* Restore the global pattern even if the core-producing child fails. */
FIXTURE_TEARDOWN_PARENT(elf_core)
{
	if (self->restore_pattern) {
		size_t current_length = 0;
		char *current = read_core_pattern(&current_length);
		bool unchanged = current &&
			current_length == self->saved_pattern_length &&
			!memcmp(current, self->saved_pattern, current_length);

		free(current);
		if (!unchanged)
			EXPECT_TRUE(write_file("/proc/sys/kernel/core_pattern",
					self->saved_pattern,
					self->saved_pattern_length));
	}
	if (self->core_path[0] && unlink(self->core_path) && errno != ENOENT)
		EXPECT_TRUE(false) TH_LOG("cannot unlink %s: %s",
					   self->core_path, strerror(errno));
	if (self->offset_file_created && unlink(self->offset_path))
		EXPECT_TRUE(false) TH_LOG("cannot unlink %s: %s",
					   self->offset_path, strerror(errno));
}

/* 0: inspected; 1: temporary core_pattern cannot be installed; -1: error. */
static int prepare_core(struct _test_data_elf_core *self)
{
	const char *tmpdir = getenv("TMPDIR");
	char *saved_pattern;
	size_t saved_length = 0;
	pid_t pid, waited;
	int status;

	if (!tmpdir || !tmpdir[0])
		tmpdir = "/tmp";
	if (snprintf(self->offset_path, sizeof(self->offset_path),
		     "%s/elf-core-p3s-offset-%d", tmpdir, getpid()) >=
	    sizeof(self->offset_path))
		return -1;
	if (!create_offset_file(self->offset_path, &self->offset_file_created))
		return -1;
	saved_pattern = read_core_pattern(&saved_length);
	if (!saved_pattern)
		return -1;
	if (saved_length > sizeof(self->saved_pattern)) {
		free(saved_pattern);
		return -1;
	}
	memcpy(self->saved_pattern, saved_pattern, saved_length);
	self->saved_pattern_length = saved_length;
	free(saved_pattern);
	self->restore_pattern = true;
	if (snprintf(self->pattern, sizeof(self->pattern),
		     "%s/elf-core-p3s-%d-%%p", tmpdir, getpid()) >=
	    sizeof(self->pattern))
		return -1;
	if (!write_file("/proc/sys/kernel/core_pattern", self->pattern,
			strlen(self->pattern)))
		return 1;
	pid = fork();
	if (!pid)
		crash_with_test_vmas(self->offset_path);
	if (pid < 0)
		return -1;
	if (snprintf(self->core_path, sizeof(self->core_path),
		     "%s/elf-core-p3s-%d-%d", tmpdir, getpid(), pid) >=
	    sizeof(self->core_path))
		return -1;
	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited != pid)
		return -1;
	self->crash_signaled = WIFSIGNALED(status) &&
		WTERMSIG(status) == SIGSEGV;
	self->inspected = inspect_core(self->core_path, &self->found,
				       &self->misaligned,
				       &self->offset_segment_found,
				       &self->offset_dump_size,
				       &self->nt_file_found,
				       &self->note_page_size,
				       &self->note_file_offset,
				       &self->packed_found,
				       &self->contents_match);
	if (!self->crash_signaled || !self->inspected)
		return -1;
	return 0;
}

TEST_F(elf_core, four_k_vmas_keep_pt_load_alignment)
{
	int rc = prepare_core(self);

	if (rc == 1)
		SKIP(return, "cannot install temporary core_pattern");
	ASSERT_EQ(0, rc);
	ASSERT_TRUE(self->inspected);
	EXPECT_EQ(TEST_VMAS, self->found);
	EXPECT_EQ(0, self->misaligned);
}

TEST_F(elf_core, pt_load_data_preserves_each_vma)
{
	int rc = prepare_core(self);

	if (rc == 1)
		SKIP(return, "cannot install temporary core_pattern");
	ASSERT_EQ(0, rc);
	ASSERT_TRUE(self->inspected);
	EXPECT_EQ(TEST_VMAS, self->found);
	EXPECT_TRUE(self->packed_found);
	EXPECT_TRUE(self->contents_match);
}

TEST_F(elf_core, offset_4k_mapping_is_not_dumped)
{
	int rc = prepare_core(self);

	if (rc == 1)
		SKIP(return, "cannot install temporary core_pattern");
	ASSERT_EQ(0, rc);
	ASSERT_TRUE(self->inspected);
	EXPECT_TRUE(self->offset_segment_found);
	EXPECT_EQ(0, self->offset_dump_size);
}

TEST_F(elf_core, nt_file_uses_4k_file_offset_units)
{
	int rc = prepare_core(self);

	if (rc == 1)
		SKIP(return, "cannot install temporary core_pattern");
	ASSERT_EQ(0, rc);
	ASSERT_TRUE(self->inspected);
	EXPECT_TRUE(self->nt_file_found);
	EXPECT_EQ(PROCESS_PAGE_SIZE, self->note_page_size);
	EXPECT_EQ(1, self->note_file_offset);
}

P3S_HARNESS_4KB_MAIN
