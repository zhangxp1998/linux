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
TEST_GEN_PROGS += mbind_p3s munmap_p3s print_vma_addr_p3s vma_merge_devzero_p3s vma_merge_file_slice_p3s vma_merge_next_pgoff_p3s vma_range_slice_p3s

# Exercise sliced mremap and file-offset moves
TEST_GEN_PROGS += mremap_anon_holes_p3s mremap_commit_p3s mremap_file_slice_p3s mremap_hint_p3s mremap_rlimit_p3s mremap_slice_cow_p3s mremap_split_p3s remap_file_pages_p3s swap_slice_mremap_p3s
TEST_GEN_FILES += mremap_pgoff_p3s
TEST_PROGS += mremap_pgoff_p3s.sh

# Check anonymous COW, fork and reverse mappings
TEST_GEN_PROGS += anon_rmap_split_window_p3s dontneed_refault_p3s fork_dontfork_slice_p3s fork_page_size_p3s hole_anon_p3s packed_anon_p3s zero_sibling_mlock_p3s

TEST_FILES := p3s_run_module.sh
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
$(foreach t,mremap_anon_holes_p3s dontneed_refault_p3s hole_anon_p3s packed_anon_p3s anon_rmap_split_window_p3s,   $(eval $(call p3s_test_hdrs,$(t),p3s_slice_test.h)))
