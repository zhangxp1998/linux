# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wall -O2 -pthread $(KHDR_INCLUDES) -I../../.. -I../../../mm/p3s
LDFLAGS += -Wl,-z,max-page-size=16384
TEST_GEN_FILES := binder_alloc_p3s
TEST_PROGS := binder_policy_p3s.sh binder_tail_p3s.sh

include ../../../lib.mk
