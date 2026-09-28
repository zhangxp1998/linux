# SPDX-License-Identifier: GPL-2.0
# Shared build rules for P3S tests in their owning selftest subsystems.

P3S_SELFTESTS_DIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))

CFLAGS += -Wall -O2 -Wunreachable-code -U_FORTIFY_SOURCE $(EXTRA_CFLAGS)
LDFLAGS += -Wl,-z,max-page-size=16384
LDLIBS += -lrt -lpthread -lm
LOCAL_HDRS += $(P3S_SELFTESTS_DIR)/kselftest_p3s.h \
	$(P3S_SELFTESTS_DIR)/p3s_harness.h

include $(P3S_SELFTESTS_DIR)/lib.mk

CFLAGS += -I$(top_srcdir) -I$(P3S_SELFTESTS_DIR) \
	$(KHDR_INCLUDES) $(TOOLS_INCLUDES)
