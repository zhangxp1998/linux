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
TEST_GEN_PROGS += munmap_p3s print_vma_addr_p3s vma_merge_devzero_p3s vma_merge_file_slice_p3s vma_merge_next_pgoff_p3s vma_range_slice_p3s

P3S_USE_MM_UTIL := 1

include ../../p3s_build.mk
