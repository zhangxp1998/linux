# SPDX-License-Identifier: GPL-2.0
# Page-size contract tests plus mixed-geometry P3S selftests for arm64.

ifeq ($(ARCH),)
ifeq ($(CROSS_COMPILE),)
uname_M := $(shell uname -m 2>/dev/null || echo not)
else
uname_M := $(shell echo $(CROSS_COMPILE) | grep -o '^[a-z0-9]\+')
endif
ARCH ?= $(shell echo $(uname_M) | sed -e 's/aarch64.*/arm64/' -e 's/ppc64.*/powerpc/')
endif

.DELETE_ON_ERROR:
MAKEFLAGS += --no-builtin-rules
CFLAGS = -Wall -O2 -I$(top_srcdir) -I.. -I../.. $(EXTRA_CFLAGS) $(KHDR_INCLUDES) $(TOOLS_INCLUDES)
CFLAGS += -Wunreachable-code -U_FORTIFY_SOURCE
LDLIBS = -lrt -lpthread -lm
# The launcher and its MM tests start as native 16K processes.
LDFLAGS += -Wl,-z,max-page-size=16384

# Each feature patch registers its own binaries in this directory.
TEST_GEN_PROGS :=
TEST_GEN_FILES :=
TEST_PROGS :=

# Validate native and process page-size contracts
TEST_GEN_PROGS += page_size_contract_p3s

# Check process-page mmap placement and stack guards
TEST_GEN_PROGS += mmap_hint_p3s mmap_page_zero_p3s stack_guard_gap_p3s

# Check VMA slice offsets, merges and unmaps
TEST_GEN_PROGS += \
	devzero_slice_p3s mbind_p3s munmap_p3s print_vma_addr_p3s \
	vma_merge_devzero_p3s vma_merge_file_slice_p3s vma_merge_next_pgoff_p3s \
	vma_range_slice_p3s


TEST_FILES := 
LOCAL_HDRS += ../../kselftest_p3s.h ../../p3s_harness.h
P3S_MM_TESTS := $(TEST_GEN_PROGS) $(TEST_GEN_FILES)
include ../../lib.mk
P3S_MM_TARGETS := $(addprefix $(OUTPUT)/,$(P3S_MM_TESTS))
$(P3S_MM_TARGETS): ../vm_util.c ../thp_settings.c
$(OUTPUT)/anon_vma_name_p3s $(OUTPUT)/iov_iter_p3s \
$(OUTPUT)/mremap_pgoff_p3s: LDFLAGS += -pie
