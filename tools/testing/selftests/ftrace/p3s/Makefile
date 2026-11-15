# SPDX-License-Identifier: GPL-2.0
# P3S tests for this subsystem.
ARCH ?= $(shell uname -m | sed 's/aarch64/arm64/')
ifeq ($(ARCH),arm64)
TEST_GEN_PROGS := uprobe_p3s
endif

include ../../p3s_build.mk
