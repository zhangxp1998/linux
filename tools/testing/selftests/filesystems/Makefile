# SPDX-License-Identifier: GPL-2.0

KDIR ?= $(if $(O),$(O),$(realpath ../../../..))

CFLAGS += $(KHDR_INCLUDES)
TEST_GEN_PROGS := devpts_pts fsopen_name_size_ppps \
	file_dedupe_range_size_ppps kernfs_write_size_ppps
TEST_GEN_FILES := debugfs_str_size_ppps
TEST_GEN_PROGS_EXTENDED := dnotify_test
TEST_PROGS := test-debugfs-str-size-ppps.sh

DEBUGFS_STR_SIZE_PPPS_MODULE := \
	debugfs_str_size_ppps_module/debugfs_str_size_ppps.ko
TEST_FILES := $(DEBUGFS_STR_SIZE_PPPS_MODULE)

include ../lib.mk

all: $(DEBUGFS_STR_SIZE_PPPS_MODULE)

$(DEBUGFS_STR_SIZE_PPPS_MODULE): \
	debugfs_str_size_ppps_module/debugfs_str_size_ppps.c \
	debugfs_str_size_ppps_module/Makefile
	+$(MAKE) -C debugfs_str_size_ppps_module KDIR=$(KDIR)

.PHONY: clean_debugfs_str_size_ppps_module
clean_debugfs_str_size_ppps_module:
	+$(MAKE) -C debugfs_str_size_ppps_module KDIR=$(KDIR) clean

clean: clean_debugfs_str_size_ppps_module
