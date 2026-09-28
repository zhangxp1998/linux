# SPDX-License-Identifier: GPL-2.0
# Page-size contract tests plus mixed-geometry P3S selftests for arm64.

ifeq ($(ARCH),)
ifeq ($(CROSS_COMPILE),)
uname_M := $(shell uname -m 2>/dev/null || echo not)
else
uname_M := $(shell echo $(notdir $(CROSS_COMPILE)) | grep -o '^[a-z0-9]\+')
endif
ARCH ?= $(shell echo $(uname_M) | sed -e 's/aarch64.*/arm64/' -e 's/ppc64.*/powerpc/')
endif

.DELETE_ON_ERROR:
MAKEFLAGS += --no-builtin-rules

# Each feature patch registers its own binaries in this directory.
TEST_GEN_PROGS :=
TEST_GEN_FILES :=
TEST_PROGS :=

# Validate native and process page-size contracts
TEST_GEN_PROGS += page_size_contract_p3s

# Check process-page mmap placement and stack guards
TEST_GEN_PROGS += mmap_hint_p3s stack_guard_gap_p3s

# Check VMA slice offsets, merges and unmaps
TEST_GEN_PROGS += anon_vma_name_p3s mbind_p3s munmap_p3s print_vma_addr_p3s vma_merge_devzero_p3s vma_merge_file_slice_p3s vma_merge_next_pgoff_p3s vma_range_slice_p3s

# Exercise sliced mremap and file-offset moves
TEST_GEN_PROGS += mremap_anon_holes_p3s mremap_commit_p3s mremap_file_slice_p3s mremap_hint_p3s mremap_rlimit_p3s mremap_slice_cow_p3s mremap_split_p3s remap_file_pages_p3s swap_slice_mremap_p3s
TEST_GEN_FILES += mremap_pgoff_p3s
TEST_PROGS += mremap_pgoff_p3s.sh

# Check anonymous COW, fork and reverse mappings
TEST_GEN_PROGS += anon_rmap_split_window_p3s dontneed_refault_p3s fork_dontfork_slice_p3s fork_page_size_p3s hole_anon_p3s packed_anon_p3s zero_sibling_mlock_p3s

# Exercise GUP and iov_iter slice boundaries
TEST_GEN_FILES += fault_in_p3s vma_address_end_p3s gup_retry_p3s iov_iter_p3s iov_range_retry_p3s mmap_fail_p3s page_range_p3s

TEST_PROGS += fault_in_p3s.sh gup_retry_p3s.sh iov_iter_p3s.sh iov_range_retry_p3s.sh mmap_fail_p3s.sh page_range_p3s.sh vma_address_end_p3s.sh

# Check remote MM access and process release
TEST_GEN_PROGS += mixed_context_switch_p3s mixed_mm_access_p3s process_madvise_p3s process_mrelease_p3s process_vm_access_p3s process_vm_packed_p3s remote_write_dontneed_p3s

# Exercise file and shmem slice mappings
TEST_GEN_PROGS += file_cow_p3s fork_filemap_p3s msync_offset_p3s shmem_mmap_offset_p3s shmem_swap_usage_p3s thp_mmap_offset_p3s truncate_p3s truncate_tail_p3s
TEST_GEN_FILES += file_cow_refs_p3s
TEST_GEN_PROGS += rmap_file_referenced_p3s
TEST_PROGS += file_cow_refs_p3s.sh
TEST_GEN_PROGS += shmem_thp_mmap_p3s

# Check page locking across folio slices
TEST_GEN_PROGS += mlock_file_slices_p3s mlock_large_file_p3s mlock_onfault_p3s mlock_reclaim_p3s

# Check madvise at process-page boundaries
TEST_GEN_PROGS += madvise_free_holes_p3s madvise_free_p3s madvise_guard_p3s madvise_remove_p3s
TEST_GEN_PROGS += madv_cold_large_file_p3s madvise_willneed_p3s

# Check reclaim and swap at slice boundaries
TEST_GEN_PROGS += anon_pageout_p3s packed_swap_p3s partial_anon_reclaim_p3s swap_badpages_loop_p3s swap_header_badpages_p3s swap_header_p3s swap_pss_p3s swap_vma_readahead_p3s swapoff_shared_slice_p3s
TEST_GEN_FILES += memory_failure_anon_p3s mglru_p3s
TEST_PROGS += mglru_p3s.sh

# Check userfaultfd copy, move and remote operations
TEST_GEN_PROGS += uffd_copy_tuple_p3s uffd_move_packed_p3s uffd_move_policy_p3s uffd_move_swap_window_p3s uffd_sigbus_anon_p3s userfaultfd_mixed_p3s userfaultfd_poison_remote_p3s userfaultfd_remap_p3s userfaultfd_remote_p3s userfaultfd_shmem_p3s userfaultfd_unregister_remote_p3s userfaultfd_wp_remote_p3s

TEST_FILES := p3s_run_module.sh p3s_run_debugfs.sh
KDIR ?= $(if $(O),$(O),/lib/modules/$(shell uname -r)/build)
P3S_KERNEL_SOURCE ?= $(if $(wildcard $(KDIR)/source),$(realpath $(KDIR)/source),$(KDIR))
ifneq (,$(wildcard $(KDIR)/Module.symvers))
ifneq (,$(wildcard $(P3S_KERNEL_SOURCE)/include/linux/p3s.h))
TEST_GEN_MODS_DIR := p3s_modules
endif
endif
P3S_USE_MM_UTIL := 1

include ../../p3s_build.mk

# Shared headers used by userspace tests and their fixture modules.
define p3s_test_hdrs
$(OUTPUT)/$(1): LOCAL_HDRS += $(2)
$(OUTPUT)/$(1): $(2)
endef
$(eval $(call p3s_test_hdrs,fault_in_p3s,fault_in_p3s.h))
$(eval $(call p3s_test_hdrs,vma_address_end_p3s,vma_address_end_p3s.h))
$(eval $(call p3s_test_hdrs,gup_retry_p3s,gup_retry_p3s.h))
$(eval $(call p3s_test_hdrs,iov_iter_p3s,iov_iter_p3s.h))
$(eval $(call p3s_test_hdrs,iov_range_retry_p3s,iov_range_retry_p3s.h))
$(eval $(call p3s_test_hdrs,page_range_p3s,iov_iter_p3s.h page_range_p3s.h))
$(eval $(call p3s_test_hdrs,file_cow_refs_p3s,iov_iter_p3s.h page_range_p3s.h))
$(foreach t,mremap_anon_holes_p3s process_vm_packed_p3s madvise_free_holes_p3s uffd_move_packed_p3s dontneed_refault_p3s file_cow_p3s hole_anon_p3s packed_anon_p3s packed_swap_p3s uffd_copy_tuple_p3s uffd_move_swap_window_p3s remote_write_dontneed_p3s anon_rmap_split_window_p3s uffd_move_policy_p3s anon_pageout_p3s partial_anon_reclaim_p3s swap_pss_p3s swap_vma_readahead_p3s swapoff_shared_slice_p3s,   $(eval $(call p3s_test_hdrs,$(t),p3s_slice_test.h)))
