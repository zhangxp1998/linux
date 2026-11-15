# SPDX-License-Identifier: GPL-2.0
# Shared build rules for P3S tests in their owning selftest subsystems.

P3S_SELFTESTS_DIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
ifneq ($(P3S_USE_MM_UTIL),)
ifeq ($(origin P3S_C_TARGETS),undefined)
P3S_C_TARGETS := $(filter-out $(P3S_ASM_PROGS),$(TEST_GEN_PROGS) $(TEST_GEN_FILES))
endif
endif

CFLAGS += -Wall -O2 -Wunreachable-code -U_FORTIFY_SOURCE $(EXTRA_CFLAGS)
LDFLAGS += -Wl,-z,max-page-size=16384
LDLIBS += -lrt -lpthread -lm
LOCAL_HDRS += $(P3S_SELFTESTS_DIR)/kselftest_p3s.h \
	$(P3S_SELFTESTS_DIR)/p3s_harness.h

include $(P3S_SELFTESTS_DIR)/lib.mk

CFLAGS += -I$(top_srcdir) -I$(P3S_SELFTESTS_DIR) \
	$(KHDR_INCLUDES) $(TOOLS_INCLUDES)
ifneq ($(P3S_USE_MM_UTIL),)
CFLAGS += -I$(P3S_SELFTESTS_DIR)/mm -I$(P3S_SELFTESTS_DIR)/mm/p3s
endif

ifneq ($(P3S_USE_MM_UTIL),)
$(addprefix $(OUTPUT)/,$(P3S_C_TARGETS)): $(P3S_SELFTESTS_DIR)/mm/vm_util.c
endif
