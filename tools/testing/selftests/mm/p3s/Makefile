# SPDX-License-Identifier: GPL-2.0
# Page-size contract tests plus mixed-geometry P3S selftests for arm64.

ifeq ($(ARCH),)
ifeq ($(CROSS_COMPILE),)
uname_M := $(shell uname -m 2>/dev/null || echo not)
else
uname_M := $(shell echo $(CROSS_COMPILE) | grep -o '^[a-z0-9]\+')
endif
ARCH ?= $(shell echo $(uname_M) | sed -e 's/aarch64.*/arm64/' -e 's/ppc64.*/powerpc/')
endif

.DELETE_ON_ERROR:
MAKEFLAGS += --no-builtin-rules
CFLAGS = -Wall -O2 -I$(top_srcdir) -I.. -I../.. $(EXTRA_CFLAGS) $(KHDR_INCLUDES) $(TOOLS_INCLUDES)
CFLAGS += -Wunreachable-code -U_FORTIFY_SOURCE
LDLIBS = -lrt -lpthread -lm
# The launcher and its MM tests start as native 16K processes.
LDFLAGS += -Wl,-z,max-page-size=16384

# Each feature patch registers its own binaries in this directory.
TEST_GEN_PROGS :=
TEST_GEN_FILES :=
TEST_PROGS :=


TEST_FILES := 
LOCAL_HDRS += ../../kselftest_p3s.h ../../p3s_harness.h
P3S_MM_TESTS := $(TEST_GEN_PROGS) $(TEST_GEN_FILES)
include ../../lib.mk
P3S_MM_TARGETS := $(addprefix $(OUTPUT)/,$(P3S_MM_TESTS))
$(P3S_MM_TARGETS): ../vm_util.c ../thp_settings.c
