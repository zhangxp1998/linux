# SPDX-License-Identifier: GPL-2.0
# P3S tests for this subsystem.
ARCH ?= $(shell uname -m | sed 's/aarch64/arm64/')
TEST_GEN_PROGS := exec_env_p3s exec_user_args_p3s 
ifeq ($(ARCH),arm64)
TEST_GEN_PROGS += elf_4k_align_p3s \
	exec_stack_random_p3s
P3S_ASM_PROGS := elf_4k_align_probe
TEST_GEN_FILES := $(P3S_ASM_PROGS)
endif

P3S_MM_HELPERS :=
include ../../p3s_build.mk
LOCAL_HDRS += exec_p3s_helpers.h

ifeq ($(ARCH),arm64)
$(OUTPUT)/elf_4k_align_probe: elf_4k_align_probe.S
	$(CC) -nostdlib -static -Wl,-no-pie -Wl,--build-id=none -Wl,-z,max-page-size=4096 $< -o $@
endif
