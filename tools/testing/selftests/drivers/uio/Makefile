# SPDX-License-Identifier: GPL-2.0

KDIR ?= $(if $(O),$(O),$(realpath ../../../..))

TEST_GEN_PROGS := uio_mmap_ppps
TEST_PROGS := test-uio-mmap-ppps.sh
UIO_MMAP_PPPS_MODULE := \
	uio_mmap_ppps_module/uio_mmap_ppps_test.ko
TEST_FILES := $(UIO_MMAP_PPPS_MODULE)

include ../../lib.mk

ifneq ($(UIO_MMAP_PPPS_MODULE),)
all: $(UIO_MMAP_PPPS_MODULE)

$(UIO_MMAP_PPPS_MODULE): \
	uio_mmap_ppps_module/uio_mmap_ppps_test.c \
	uio_mmap_ppps_module/Makefile
	+$(MAKE) -C uio_mmap_ppps_module KDIR=$(KDIR)

.PHONY: clean_uio_mmap_ppps_module
clean_uio_mmap_ppps_module:
	+$(MAKE) -C uio_mmap_ppps_module KDIR=$(KDIR) clean

clean: clean_uio_mmap_ppps_module
endif
