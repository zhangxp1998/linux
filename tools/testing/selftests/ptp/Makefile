# SPDX-License-Identifier: GPL-2.0
CFLAGS += $(KHDR_INCLUDES)
KDIR ?= $(if $(O),$(O),$(realpath ../../../..))

TEST_GEN_PROGS := testptp vmclock_mmap_ppps
LDLIBS += -lrt
TEST_PROGS = phc.sh
TEST_PROGS += test-vmclock-mmap-ppps.sh
VMCLOCK_MMAP_PPPS_MODULE := \
	vmclock_mmap_ppps_module/vmclock_mmap_ppps_test.ko
TEST_FILES := $(VMCLOCK_MMAP_PPPS_MODULE)

include ../lib.mk

ifneq ($(VMCLOCK_MMAP_PPPS_MODULE),)
all: $(VMCLOCK_MMAP_PPPS_MODULE)

$(VMCLOCK_MMAP_PPPS_MODULE): \
	vmclock_mmap_ppps_module/vmclock_mmap_ppps_test.c \
	vmclock_mmap_ppps_module/Makefile
	+$(MAKE) -C vmclock_mmap_ppps_module KDIR=$(KDIR)

.PHONY: clean_vmclock_mmap_ppps_module
clean_vmclock_mmap_ppps_module:
	+$(MAKE) -C vmclock_mmap_ppps_module KDIR=$(KDIR) clean

clean: clean_vmclock_mmap_ppps_module
endif
