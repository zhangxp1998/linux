# SPDX-License-Identifier: GPL-2.0
# P3S tests for this subsystem.
ifeq ($(ARCH),)
ifeq ($(CROSS_COMPILE),)
uname_M := $(shell uname -m 2>/dev/null || echo not)
else
uname_M := $(shell echo $(notdir $(CROSS_COMPILE)) | grep -o '^[a-z0-9]\+')
endif
ARCH ?= $(shell echo $(uname_M) | sed -e 's/aarch64.*/arm64/' -e 's/ppc64.*/powerpc/')
endif
TEST_GEN_PROGS := exec_env_p3s exec_user_args_p3s mmap_page_zero_p3s
ifeq ($(ARCH),arm64)
TEST_GEN_PROGS += elf_4k_align_p3s \
	exec_stack_random_p3s
P3S_ASM_PROGS := elf_4k_align_probe
TEST_GEN_FILES := $(P3S_ASM_PROGS)
endif

LOCAL_HDRS += exec_p3s_helpers.h
include ../../p3s_build.mk

ifeq ($(ARCH),arm64)
$(OUTPUT)/elf_4k_align_probe: elf_4k_align_probe.S
	$(CC) $(ASFLAGS) $(KHDR_INCLUDES) $(CPPFLAGS) $(USERLDFLAGS) $(TARGET_ARCH) -nostdlib -static -Wl,-no-pie -Wl,--build-id=none -Wl,-z,max-page-size=4096 $< -o $@
endif
