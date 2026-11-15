// SPDX-License-Identifier: GPL-2.0
/*
 * ptrace and /proc/<pid>/mem reach VM_IO|VM_PFNMAP mappings through
 * vm_ops->access, which for PCI sysfs resources is generic_access_phys().
 * It must read the same device bytes as a load through the mapping, in a
 * 4KB process and from a native 16KB reader of a 4KB target.  Only
 * allowlisted read-safe regions (virtio-pci VIRTIO_PCI_CAP_DEVICE_CFG and
 * QEMU EDU register 0) are probed.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define P3S_HARNESS_DEFAULT_NATIVE 1
#define P3S_HARNESS_ROLE child_role
#define P3S_HARNESS_SETUP setup_harness
#include "p3s_harness.h"

#define IORESOURCE_MEM			0x00000200ULL
#define PCI_VENDOR_VIRTIO		0x1af4U
#define PCI_VENDOR_EDU			0x1234U
#define PCI_DEVICE_EDU			0x11e8U
#define PCI_CAP_ID_VNDR			0x09U
#define VIRTIO_PCI_CAP_DEVICE_CFG	4U
#define MAX_READ_BYTES			16U

struct pci_target {
	char path[PATH_MAX];
	unsigned long cap_offset;
	unsigned int read_len;
	unsigned int read_width;
};

struct remote_report {
	uint64_t target_va;
	uint8_t bytes[MAX_READ_BYTES];
};

static struct pci_target selected_target;
static bool selected;

static int read_hex_value(const char *path, unsigned int *value)
{
	FILE *file = fopen(path, "re");
	int matched;

	if (!file)
		return -errno;
	matched = fscanf(file, "%x", value);
	fclose(file);
	return matched == 1 ? 0 : -EINVAL;
}

static bool read_bar_size(const char *bdf, unsigned int wanted_bar,
			  unsigned long long *size_out)
{
	char path[PATH_MAX];
	unsigned long long start, end, flags;
	unsigned int bar;
	FILE *file;
	bool ok = false;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource", bdf);
	file = fopen(path, "re");
	if (!file)
		return false;
	for (bar = 0;
	     bar <= wanted_bar &&
	     fscanf(file, "%llx %llx %llx", &start, &end, &flags) == 3;
	     bar++) {
		if (bar == wanted_bar && (flags & IORESOURCE_MEM) &&
		    end > start) {
			*size_out = end - start + 1;
			ok = true;
			break;
		}
	}
	fclose(file);
	return ok;
}

static uint32_t read_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t read_mmio_u8(const void *addr)
{
#ifdef __aarch64__
	uint8_t val;

	/* Avoid compiler writeback addressing so KVM sets ESR_EL2.ISV=1. */
	asm volatile("ldrb %w0, [%1]" : "=r" (val) : "r" (addr) : "memory");
	return val;
#else
	return __atomic_load_n((const uint8_t *)addr, __ATOMIC_RELAXED);
#endif
}

static uint32_t read_mmio_u32(const void *addr)
{
#ifdef __aarch64__
	uint32_t val;

	asm volatile("ldr %w0, [%1]" : "=r" (val) : "r" (addr) : "memory");
	return val;
#else
	return __atomic_load_n((const uint32_t *)addr, __ATOMIC_RELAXED);
#endif
}

static bool read_direct_bytes(const void *addr, unsigned int width,
			      unsigned int len, uint8_t out[MAX_READ_BYTES])
{
	unsigned int i;

	if (width == 4) {
		uint32_t v = read_mmio_u32(addr);

		memcpy(out, &v, 4);
		return true;
	}
	if (width == 1) {
		const uint8_t *p = (const uint8_t *)addr;

		for (i = 0; i < len; i++)
			out[i] = read_mmio_u8(p + i);
		return true;
	}
	return false;
}

static bool read_proc_mem(int mem_fd, uintptr_t addr, unsigned int width,
			  unsigned int len, uint8_t out[MAX_READ_BYTES])
{
	unsigned int i;

	if (width == 4)
		return pread(mem_fd, out, 4, (off_t)addr) == 4;
	if (width == 1) {
		for (i = 0; i < len; i++) {
			if (pread(mem_fd, &out[i], 1, (off_t)(addr + i)) != 1)
				return false;
		}
		return true;
	}
	return false;
}

static bool any_nonzero(const uint8_t *buf, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		if (buf[i])
			return true;
	return false;
}

static int probe_target(const struct pci_target *target)
{
	uint8_t direct[MAX_READ_BYTES] = {}, via_mem[MAX_READ_BYTES] = {};
	unsigned long bar_window_off;
	unsigned long in_window_off;
	void *map;
	int fd, mem, ret = KSFT_FAIL;

	bar_window_off = target->cap_offset & ~(NATIVE_PAGE_SIZE - 1);
	in_window_off = target->cap_offset - bar_window_off;

	fd = open(target->path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return KSFT_SKIP;
	map = mmap(NULL, NATIVE_PAGE_SIZE, PROT_READ, MAP_SHARED, fd,
		   bar_window_off);
	if (map == MAP_FAILED) {
		close(fd);
		return KSFT_SKIP;
	}
	if (!read_direct_bytes((const char *)map + in_window_off,
			       target->read_width, target->read_len, direct))
		goto out_unmap;
	mem = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
	if (mem < 0)
		goto out_unmap;
	if (!read_proc_mem(mem, (uintptr_t)map + in_window_off,
			   target->read_width, target->read_len, via_mem)) {
		close(mem);
		goto out_unmap;
	}
	close(mem);
	if (memcmp(direct, via_mem, target->read_len))
		goto out_unmap;
	ret = any_nonzero(direct, target->read_len) ? KSFT_PASS : KSFT_SKIP;
out_unmap:
	munmap(map, NATIVE_PAGE_SIZE);
	close(fd);
	return ret;
}

static bool probe_virtio_device_cfg(const char *bdf, struct pci_target *best,
				    bool *have_best)
{
	uint8_t cfg[256];
	char path[PATH_MAX];
	unsigned int steps;
	unsigned int pos;
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", bdf);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	n = pread(fd, cfg, sizeof(cfg), 0);
	close(fd);
	if (n < 0x40 || !(cfg[0x06] & 0x10))
		return false;

	pos = cfg[0x34] & ~3U;
	for (steps = 0; steps < 48 && pos >= 0x40 && pos + 4 <= (size_t)n;
	     steps++) {
		uint8_t cap_vndr = cfg[pos];
		uint8_t cap_next = cfg[pos + 1] & ~3U;
		uint8_t cap_len = cfg[pos + 2];

		if (cap_len < 4 || pos + cap_len > (size_t)n)
			break;
		if (cap_vndr == PCI_CAP_ID_VNDR && cap_len >= 16 &&
		    cfg[pos + 3] == VIRTIO_PCI_CAP_DEVICE_CFG) {
			unsigned int bar = cfg[pos + 4];
			uint32_t offset = read_le32(&cfg[pos + 8]);
			uint32_t length = read_le32(&cfg[pos + 12]);
			unsigned long window_off =
				offset & ~(NATIVE_PAGE_SIZE - 1);
			unsigned long in_window = offset - window_off;
			unsigned long avail = NATIVE_PAGE_SIZE - in_window;
			unsigned long long bar_size = 0;
			struct pci_target cand = {};

			if (bar < 6 && length >= 1 &&
			    read_bar_size(bdf, bar, &bar_size) &&
			    bar_size >= (unsigned long long)window_off +
					NATIVE_PAGE_SIZE) {
				snprintf(cand.path, sizeof(cand.path),
					 "/sys/bus/pci/devices/%s/resource%u",
					 bdf, bar);
				cand.cap_offset = offset;
				cand.read_len = length < MAX_READ_BYTES ?
						length : MAX_READ_BYTES;
				if (cand.read_len > avail)
					cand.read_len = avail;
				cand.read_width = 1;
				if (cand.read_len &&
				    probe_target(&cand) == KSFT_PASS) {
					*best = cand;
					*have_best = true;
					if (in_window >= PROCESS_PAGE_SIZE)
						return true;
				}
			}
		}
		if (!cap_next || cap_next == pos)
			break;
		pos = cap_next;
	}
	return false;
}

static bool select_pci_target(void)
{
	DIR *dir = opendir("/sys/bus/pci/devices");
	struct pci_target fallback = {};
	bool have_fallback = false;
	struct dirent *entry;

	if (!dir)
		return false;
	while ((entry = readdir(dir))) {
		unsigned int vendor = 0, device = 0;
		char value_path[PATH_MAX];

		if (entry->d_name[0] == '.')
			continue;
		snprintf(value_path, sizeof(value_path),
			 "/sys/bus/pci/devices/%s/vendor", entry->d_name);
		if (read_hex_value(value_path, &vendor))
			continue;
		snprintf(value_path, sizeof(value_path),
			 "/sys/bus/pci/devices/%s/device", entry->d_name);
		if (read_hex_value(value_path, &device))
			continue;
		if (vendor == PCI_VENDOR_VIRTIO) {
			if (probe_virtio_device_cfg(entry->d_name, &fallback,
						    &have_fallback)) {
				selected_target = fallback;
				closedir(dir);
				return true;
			}
		} else if (vendor == PCI_VENDOR_EDU &&
			   device == PCI_DEVICE_EDU) {
			unsigned long long bar_size = 0;
			struct pci_target edu = {};

			if (read_bar_size(entry->d_name, 0, &bar_size) &&
			    bar_size >= NATIVE_PAGE_SIZE) {
				snprintf(edu.path, sizeof(edu.path),
					 "/sys/bus/pci/devices/%s/resource0",
					 entry->d_name);
				edu.cap_offset = 0;
				edu.read_len = 4;
				edu.read_width = 4;
				if (!have_fallback &&
				    probe_target(&edu) == KSFT_PASS) {
					fallback = edu;
					have_fallback = true;
				}
			}
		}
	}
	closedir(dir);
	if (have_fallback) {
		selected_target = fallback;
		return true;
	}
	return false;
}

static bool map_bar_window_4kb(const struct pci_target *target, int *fd_out,
			       void **res_out, size_t *res_size_out,
			       uintptr_t *target_va_out)
{
	unsigned long bar_window_off =
		target->cap_offset & ~(NATIVE_PAGE_SIZE - 1);
	unsigned long in_window_off = target->cap_offset - bar_window_off;
	unsigned long slice_shift =
		in_window_off >= PROCESS_PAGE_SIZE ? 0 : PROCESS_PAGE_SIZE;
	size_t res_size = 3 * NATIVE_PAGE_SIZE;
	uintptr_t aligned_16k, map_base;
	void *res, *mapped;
	int fd;

	fd = open(target->path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	res = mmap(NULL, res_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
		   -1, 0);
	if (res == MAP_FAILED) {
		close(fd);
		return false;
	}
	aligned_16k = ((uintptr_t)res + NATIVE_PAGE_SIZE - 1) &
		      ~(NATIVE_PAGE_SIZE - 1);
	map_base = aligned_16k + slice_shift;
	mapped = mmap((void *)map_base, NATIVE_PAGE_SIZE, PROT_READ,
		      MAP_SHARED | MAP_FIXED, fd, bar_window_off);
	if (mapped != (void *)map_base) {
		munmap(res, res_size);
		close(fd);
		return false;
	}
	*fd_out = fd;
	*res_out = res;
	*res_size_out = res_size;
	*target_va_out = map_base + in_window_off;
	return true;
}

static bool parse_target_args(int argc, char **argv, int start,
			      struct pci_target *target)
{
	if (argc != start + 4)
		return false;
	snprintf(target->path, sizeof(target->path), "%s", argv[start]);
	target->cap_offset = strtoul(argv[start + 1], NULL, 0);
	target->read_len = (unsigned int)strtoul(argv[start + 2], NULL, 0);
	target->read_width = (unsigned int)strtoul(argv[start + 3], NULL, 0);
	return target->read_len > 0 && target->read_len <= MAX_READ_BYTES &&
	       (target->read_width == 1 || target->read_width == 4);
}

static int run_4kb_self(const struct pci_target *target)
{
	uint8_t direct[MAX_READ_BYTES] = {}, via_mem[MAX_READ_BYTES] = {};
	uintptr_t target_va = 0;
	size_t res_size = 0;
	void *res = MAP_FAILED;
	int fd = -1, mem = -1, ret = KSFT_FAIL;

	if (!p3s_is_4kb_process())
		return KSFT_SKIP;
	if (!map_bar_window_4kb(target, &fd, &res, &res_size, &target_va))
		return KSFT_FAIL;
	if (!read_direct_bytes((const void *)target_va, target->read_width,
			       target->read_len, direct))
		goto out;
	mem = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
	if (mem < 0)
		goto out;
	if (!read_proc_mem(mem, target_va, target->read_width,
			   target->read_len, via_mem))
		goto out;
	if (memcmp(direct, via_mem, target->read_len)) {
		ksft_print_msg("%s self mismatch at +%#lx\n", target->path,
			       target->cap_offset);
		goto out;
	}
	ret = any_nonzero(direct, target->read_len) ? KSFT_PASS : KSFT_FAIL;
out:
	if (mem >= 0)
		close(mem);
	if (res != MAP_FAILED)
		munmap(res, res_size);
	if (fd >= 0)
		close(fd);
	return ret;
}

static int run_4kb_remote(const struct pci_target *target, int ready_fd,
			  int done_fd)
{
	struct remote_report report = {};
	uintptr_t target_va = 0;
	size_t res_size = 0;
	void *res = MAP_FAILED;
	int fd = -1, ret = KSFT_FAIL;
	char byte;

	if (!p3s_is_4kb_process())
		return KSFT_SKIP;
	if (!map_bar_window_4kb(target, &fd, &res, &res_size, &target_va))
		goto out;
	report.target_va = (uint64_t)target_va;
	if (!read_direct_bytes((const void *)target_va, target->read_width,
			       target->read_len, report.bytes))
		goto out;
	if (!write_full(ready_fd, &report, sizeof(report)))
		goto out;
	close(ready_fd);
	ready_fd = -1;
	(void)read(done_fd, &byte, 1);
	ret = KSFT_PASS;
out:
	if (ready_fd >= 0)
		close(ready_fd);
	close(done_fd);
	if (res != MAP_FAILED)
		munmap(res, res_size);
	if (fd >= 0)
		close(fd);
	return ret;
}

TEST(four_kb_self_proc_mem_matches_bar_load)
{
	char off_buf[32], len_buf[16], width_buf[16];
	int ret;

	if (!selected)
		SKIP(return, "no allowlisted nonzero PCI BAR region");
	snprintf(off_buf, sizeof(off_buf), "%lu", selected_target.cap_offset);
	snprintf(len_buf, sizeof(len_buf), "%u", selected_target.read_len);
	snprintf(width_buf, sizeof(width_buf), "%u", selected_target.read_width);
	ret = p3s_run_child(true, "--4kb-self", selected_target.path,
			    off_buf, len_buf, width_buf, NULL);
	if (ret == KSFT_SKIP)
		SKIP(return, "4KB PCI BAR access unavailable");
	EXPECT_EQ(KSFT_PASS, ret);
}

TEST(native_remote_proc_mem_matches_4kb_bar_load)
{
	char off_buf[32], len_buf[16], width_buf[16];
	char ready_buf[16], done_buf[16], mem_path[64];
	uint8_t remote_bytes[MAX_READ_BYTES] = {};
	struct remote_report report = {};
	int ready[2], done[2], status, mem_fd;
	pid_t child, waited;
	bool got;

	if (!selected)
		SKIP(return, "no allowlisted nonzero PCI BAR region");
	ASSERT_EQ(0, pipe(ready));
	if (pipe(done)) {
		close(ready[0]);
		close(ready[1]);
		ASSERT_TRUE(false);
	}
	snprintf(off_buf, sizeof(off_buf), "%lu", selected_target.cap_offset);
	snprintf(len_buf, sizeof(len_buf), "%u", selected_target.read_len);
	snprintf(width_buf, sizeof(width_buf), "%u", selected_target.read_width);
	snprintf(ready_buf, sizeof(ready_buf), "%d", ready[1]);
	snprintf(done_buf, sizeof(done_buf), "%d", done[0]);

	child = fork();
	if (!child) {
		close(ready[0]);
		close(done[1]);
		p3s_execl(true, NULL, "--4kb-remote", ready_buf, done_buf,
			  selected_target.path, off_buf, len_buf, width_buf,
			  NULL);
		_exit(errno == EINVAL || errno == EPERM ? KSFT_SKIP : KSFT_FAIL);
	}
	close(ready[1]);
	close(done[0]);
	if (child < 0) {
		close(ready[0]);
		close(done[1]);
		ASSERT_GE(child, 0);
	}

	got = read_full(ready[0], &report, sizeof(report));
	close(ready[0]);
	if (!got) {
		close(done[1]);
		do {
			waited = waitpid(child, &status, 0);
		} while (waited < 0 && errno == EINTR);
		ASSERT_EQ(child, waited);
		if (WIFEXITED(status) && WEXITSTATUS(status) == KSFT_SKIP)
			SKIP(return, "4KB child unavailable");
		ASSERT_TRUE(false);
	}

	snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", child);
	mem_fd = open(mem_path, O_RDONLY | O_CLOEXEC);
	EXPECT_GE(mem_fd, 0);
	if (mem_fd >= 0) {
		EXPECT_TRUE(read_proc_mem(mem_fd, (uintptr_t)report.target_va,
					  selected_target.read_width,
					  selected_target.read_len,
					  remote_bytes));
		close(mem_fd);
		EXPECT_EQ(0, memcmp(report.bytes, remote_bytes,
				    selected_target.read_len));
		EXPECT_TRUE(any_nonzero(report.bytes,
					selected_target.read_len));
	}

	close(done[1]);
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	ASSERT_EQ(child, waited);
	EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == KSFT_PASS);
}

static int child_role(int argc, char **argv)
{
	struct pci_target target = {};

	if (argc >= 2 && !strcmp(argv[1], "--4kb-self")) {
		if (!parse_target_args(argc, argv, 2, &target))
			return KSFT_FAIL;
		return run_4kb_self(&target);
	}
	if (argc >= 4 && !strcmp(argv[1], "--4kb-remote")) {
		if (!parse_target_args(argc, argv, 4, &target))
			return KSFT_FAIL;
		return run_4kb_remote(&target, atoi(argv[2]), atoi(argv[3]));
	}
	return -1;
}

static void setup_harness(int *argc, char ***argv)
{
	if (p3s_harness_listing(*argc, *argv))
		return;
	if (geteuid()) {
		ksft_print_header();
		ksft_exit_skip("root is required to map PCI resources\n");
	}
	selected = select_pci_target();
}

P3S_HARNESS_4KB_MAIN
