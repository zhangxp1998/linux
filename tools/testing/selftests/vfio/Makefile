# SPDX-License-Identifier: GPL-2.0

ARCH ?= $(shell uname -m)
KDIR ?= $(if $(O),$(O),$(realpath ../../..))

ifeq (,$(filter $(ARCH),aarch64 arm64 x86_64))
# Do nothing on unsupported architectures.
include ../lib.mk
else

CFLAGS = $(KHDR_INCLUDES)
TEST_GEN_PROGS := vfio_platform_mmap_ppps
TEST_PROGS := test-vfio-platform-mmap-ppps.sh
VFIO_PLATFORM_MMAP_PPPS_MODULE := \
	vfio_platform_mmap_ppps_module/vfio_platform_mmap_ppps_test.ko
TEST_FILES := $(VFIO_PLATFORM_MMAP_PPPS_MODULE)

include ../lib.mk

all: $(VFIO_PLATFORM_MMAP_PPPS_MODULE)

$(VFIO_PLATFORM_MMAP_PPPS_MODULE): \
	vfio_platform_mmap_ppps_module/vfio_platform_mmap_ppps_test.c \
	vfio_platform_mmap_ppps_module/Makefile
	+$(MAKE) -C vfio_platform_mmap_ppps_module KDIR=$(KDIR)

.PHONY: clean_vfio_platform_mmap_ppps_module
clean_vfio_platform_mmap_ppps_module:
	+$(MAKE) -C vfio_platform_mmap_ppps_module KDIR=$(KDIR) clean

clean: clean_vfio_platform_mmap_ppps_module

endif
