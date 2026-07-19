# SPDX-License-Identifier: GPL-2.0

ARCH ?= $(shell uname -m)

ifeq (,$(filter $(ARCH),aarch64 arm64 x86_64))
# Do nothing on unsupported architectures.
include ../lib.mk
else

CFLAGS = $(KHDR_INCLUDES)
TEST_GEN_PROGS := vfio_platform_mmap_ppps
TEST_PROGS := vfio_platform_mmap_ppps.sh
TEST_FILES := ../ppps/ppps_run_module.sh
TEST_GEN_MODS_DIR := ppps_modules

include ../lib.mk

endif
