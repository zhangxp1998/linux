# SPDX-License-Identifier: GPL-2.0
# P3S tests for this subsystem.
ARCH ?= $(shell uname -m | sed 's/aarch64/arm64/')
ifeq ($(ARCH),arm64)
TEST_GEN_PROGS := mte_anon_p3s
endif

P3S_USE_MM_UTIL := 1
include ../../../p3s_build.mk
