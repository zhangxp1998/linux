# SPDX-License-Identifier: GPL-2.0
#
KDIR ?= $(if $(O),$(O),$(realpath ../../../..))

ifneq ($(shell pkg-config --exists alsa && echo 0 || echo 1),0)
$(error Package alsa not found, please install alsa development package or \
	add directory containing `alsa.pc` in PKG_CONFIG_PATH)
endif

CFLAGS += $(shell pkg-config --cflags alsa) $(KHDR_INCLUDES)
LDLIBS += $(shell pkg-config --libs alsa)
ifeq ($(LDLIBS),)
LDLIBS += -lasound
endif
CFLAGS += -L$(OUTPUT) -Wl,-rpath=./

LDLIBS+=-lpthread

OVERRIDE_TARGETS = 1

TEST_GEN_PROGS := mixer-test pcm-test test-pcmtest-driver utimer-test
TEST_GEN_PROGS += snd_pcm_mmap_ppps

TEST_GEN_PROGS_EXTENDED := libatest.so global-timer

TEST_PROGS := test-snd-pcm-mmap-ppps.sh

TEST_FILES := conf.d pcm-test.conf
SND_PCM_MMAP_PPPS_MODULE := \
	snd_pcm_mmap_ppps_module/snd_pcm_mmap_ppps_test.ko
TEST_FILES += $(SND_PCM_MMAP_PPPS_MODULE)

include ../lib.mk

$(OUTPUT)/libatest.so: conf.c alsa-local.h
	$(CC) $(CFLAGS) -shared -fPIC $< $(LDLIBS) -o $@

$(OUTPUT)/%: %.c $(TEST_GEN_PROGS_EXTENDED) alsa-local.h
	$(CC) $(CFLAGS) $< $(LDLIBS) -latest -o $@

ifneq ($(SND_PCM_MMAP_PPPS_MODULE),)
all: $(SND_PCM_MMAP_PPPS_MODULE)

$(SND_PCM_MMAP_PPPS_MODULE): \
	snd_pcm_mmap_ppps_module/snd_pcm_mmap_ppps_test.c \
	snd_pcm_mmap_ppps_module/Makefile
	+$(MAKE) -C snd_pcm_mmap_ppps_module KDIR=$(KDIR)

.PHONY: clean_snd_pcm_mmap_ppps_module
clean_snd_pcm_mmap_ppps_module:
	+$(MAKE) -C snd_pcm_mmap_ppps_module KDIR=$(KDIR) clean

clean: clean_snd_pcm_mmap_ppps_module
endif
