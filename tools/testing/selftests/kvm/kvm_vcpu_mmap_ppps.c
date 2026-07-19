// SPDX-License-Identifier: GPL-2.0
/*
 * A 4K compat process sees the VCPU mmap size, run page, coalesced-MMIO page
 * and dirty ring in 4K process-page units, maps each at its 4K offset, and
 * the kernel honors the 4K coalesced-ring capacity.
 */
#define _GNU_SOURCE

#include <linux/kvm.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>

#include "kselftest_ppps.h"


#define USER_PAGE_SIZE 4096UL
#define MAPPING_SIZE (2 * USER_PAGE_SIZE)
#define DIRTY_RING_SIZE (64 * 1024)

static int run_test(void)
{
	struct kvm_enable_cap cap = {
		.cap = KVM_CAP_DIRTY_LOG_RING_ACQ_REL,
		.args[0] = DIRTY_RING_SIZE,
	};
	unsigned long host_page_size;
	unsigned long native_pages;
	unsigned long start_pgoff;
	void *mapping;
	int vcpu_fd;
	int vm_fd;
	int kvm_fd;
	int ret;

	ksft_print_header();
	ksft_set_plan(8);
	ksft_test_result(sysconf(_SC_PAGESIZE) == USER_PAGE_SIZE,
			 "process uses 4K pages\n");

	kvm_fd = ppps_open_fixture_or_skip("/dev/kvm", O_RDWR);
	ksft_test_result(kvm_fd >= 0, "open /dev/kvm\n");

	ret = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
	ksft_test_result(ret == KVM_API_VERSION, "KVM API is available\n");
	if (ret != KVM_API_VERSION)
		ksft_exit_fail_msg("unexpected KVM API version: %d\n", ret);

	host_page_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0) /
			 (KVM_COALESCED_MMIO_PAGE_OFFSET + 1);
	ksft_test_result(host_page_size >= USER_PAGE_SIZE,
			 "query the host VCPU mmap page size\n");
	if (host_page_size < USER_PAGE_SIZE)
		ksft_exit_fail_msg("invalid VCPU mmap size: %lu\n",
				   host_page_size);

	ret = ioctl(kvm_fd, KVM_CHECK_EXTENSION,
		    KVM_CAP_DIRTY_LOG_RING_ACQ_REL);
	ksft_test_result(ret >= DIRTY_RING_SIZE,
			 "KVM dirty ring supports a 64K ring\n");
	if (ret < DIRTY_RING_SIZE)
		ksft_exit_skip("KVM dirty ring is unavailable\n");

	vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
	if (vm_fd >= 0)
		ret = ioctl(vm_fd, KVM_ENABLE_CAP, &cap);
	else
		ret = -1;
	ksft_test_result(vm_fd >= 0 && ret == 0,
			 "create a VM with a dirty ring\n");
	if (vm_fd < 0 || ret)
		ksft_exit_fail_msg("dirty-ring VM creation failed: %s\n",
				   strerror(errno));

	vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
	ksft_test_result(vcpu_fd >= 0, "create a VCPU\n");
	if (vcpu_fd < 0)
		ksft_exit_fail_msg("VCPU creation failed: %s\n",
				   strerror(errno));

	native_pages = (MAPPING_SIZE + host_page_size - 1) / host_page_size;
	start_pgoff = KVM_DIRTY_LOG_PAGE_OFFSET - native_pages;
	mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE,
		       vcpu_fd, start_pgoff * host_page_size);
	ksft_test_result(mapping != MAP_FAILED,
			 "map a private VCPU range ending before the dirty ring\n");
	if (mapping != MAP_FAILED)
		munmap(mapping, MAPPING_SIZE);

	close(vcpu_fd);
	close(vm_fd);
	close(kvm_fd);
	ksft_finished();
}

PPPS_COMPAT_MAIN(run_test)
