# SPDX-License-Identifier: GPL-2.0
CFLAGS += -static -O3 -Wl,-no-as-needed -Wall $(KHDR_INCLUDES)
LDFLAGS += -Wl,-z,max-page-size=16384

TEST_GEN_PROGS = dmabuf-heap dmabuf_heap_mmap_ppps

include ../lib.mk
