// SPDX-License-Identifier: GPL-2.0
/*
 * A 4KB process pinning perf ring-buffer pages beyond perf_event_mlock_kb
 * sees VmPin in /proc/self/status increase in native 16KB page kB units.
 */
#define _GNU_SOURCE

#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "p3s_test.h"
#include "p3s_harness.h"

#define MAX_PIN_RINGS		80
#define FILL_DATA_PAGES		256
#define PROBE_DATA_PAGES	16

struct perf_ring {
	void *map;
	size_t size;
	int fd;
};

static long read_perf_mlock_kb(void)
{
	FILE *file;
	long value = -1;

	file = fopen("/proc/sys/kernel/perf_event_mlock_kb", "re");
	if (!file)
		return -1;
	if (fscanf(file, "%ld", &value) != 1)
		value = -1;
	fclose(file);
	return value;
}

static int open_dummy_ring(size_t data_pages, struct perf_ring *ring)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_SOFTWARE,
		.size = sizeof(attr),
		.config = PERF_COUNT_SW_DUMMY,
		.disabled = 1,
		.exclude_kernel = 1,
		.exclude_hv = 1,
	};
	size_t size = (1 + data_pages) * PROCESS_PAGE_SIZE;
	void *map;
	int err, fd;

	ring->fd = -1;
	ring->map = MAP_FAILED;
	ring->size = 0;

	fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
	if (fd < 0)
		return -errno;

	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		err = -errno;
		close(fd);
		return err;
	}
	ring->fd = fd;
	ring->map = map;
	ring->size = size;
	return 0;
}

static void close_rings(struct perf_ring *rings, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (rings[i].map != MAP_FAILED)
			munmap(rings[i].map, rings[i].size);
		if (rings[i].fd >= 0)
			close(rings[i].fd);
	}
}

static bool perf_pin_unavailable(int err)
{
	return err == EPERM || err == EACCES || err == ENOENT ||
	       err == ENOSYS || err == ENOMEM;
}

TEST(vm_pin_reports_native_page_units)
{
	struct perf_ring rings[MAX_PIN_RINGS + 1];
	unsigned long limit_kernel_pages;
	long mlock_kb, nr_cpus;
	long pin_before, pin_mid, pin_after;
	unsigned int count = 0;
	bool spilled = false;
	int ret;

	mlock_kb = read_perf_mlock_kb();
	nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (mlock_kb < 0 || nr_cpus <= 0)
		SKIP(return, "perf_event_mlock_kb or CPU count unavailable");

	limit_kernel_pages = ((unsigned long)mlock_kb /
			      (NATIVE_PAGE_SIZE / 1024)) *
			     (unsigned long)nr_cpus;
	if (limit_kernel_pages > 4096)
		SKIP(return, "perf_event_mlock_kb limit too large");

	pin_before = p3s_status_kb("VmPin");
	ASSERT_GE(pin_before, 0);

	while (count < MAX_PIN_RINGS) {
		ret = open_dummy_ring(FILL_DATA_PAGES, &rings[count]);
		if (ret < 0) {
			close_rings(rings, count);
			if (perf_pin_unavailable(-ret))
				SKIP(return, "perf pinned_vm unavailable: %s",
				     strerror(-ret));
			errno = -ret;
			ASSERT_EQ(0, ret);
		}
		count++;
		pin_mid = p3s_status_kb("VmPin");
		ASSERT_GE(pin_mid, 0);
		if (pin_mid > pin_before) {
			spilled = true;
			break;
		}
	}
	if (!spilled) {
		close_rings(rings, count);
		SKIP(return, "unable to exceed perf_event_mlock_kb");
	}

	ret = open_dummy_ring(PROBE_DATA_PAGES, &rings[count]);
	if (ret < 0) {
		close_rings(rings, count);
		if (perf_pin_unavailable(-ret))
			SKIP(return, "perf pinned_vm unavailable: %s",
			     strerror(-ret));
		errno = -ret;
		ASSERT_EQ(0, ret);
	}
	count++;

	pin_after = p3s_status_kb("VmPin");
	ksft_print_msg("mlock=%ldkB cpus=%ld pin=%ld->%ld->%ldkB\n",
		       mlock_kb, nr_cpus, pin_before, pin_mid, pin_after);
	EXPECT_EQ(5L * (long)(NATIVE_PAGE_SIZE / 1024), pin_after - pin_mid);

	close_rings(rings, count);
}

P3S_HARNESS_4KB_MAIN
