# SPDX-License-Identifier: GPL-2.0
# Shared build rules for P3S tests in their owning selftest subsystems.

P3S_SELFTESTS_DIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
P3S_MM_DIR := $(P3S_SELFTESTS_DIR)/mm
P3S_MM_HELPERS ?= $(P3S_MM_DIR)/vm_util.c
P3S_C_TARGETS := $(filter-out $(P3S_ASM_PROGS),$(TEST_GEN_PROGS) $(TEST_GEN_FILES))

CFLAGS += -Wall -O2 -I$(top_srcdir) -I$(P3S_SELFTESTS_DIR) \
	-I$(P3S_MM_DIR) -I$(P3S_MM_DIR)/p3s \
	$(EXTRA_CFLAGS) $(KHDR_INCLUDES) $(TOOLS_INCLUDES)
CFLAGS += -Wunreachable-code -U_FORTIFY_SOURCE
LDFLAGS += -Wl,-z,max-page-size=16384
LDLIBS += -lrt -lpthread -lm
LOCAL_HDRS += $(P3S_SELFTESTS_DIR)/kselftest_p3s.h \
	$(P3S_SELFTESTS_DIR)/p3s_harness.h

include $(P3S_SELFTESTS_DIR)/lib.mk

$(addprefix $(OUTPUT)/,$(P3S_C_TARGETS)): $(P3S_MM_HELPERS)
