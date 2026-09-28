# SPDX-License-Identifier: GPL-2.0
# P3S tests for this subsystem.
ARCH ?= $(shell uname -m | sed 's/aarch64/arm64/')
TEST_GEN_PROGS := exec_env_p3s exec_user_args_p3s mmap_aslr_p3s stack_aslr_p3s
ifeq ($(ARCH),arm64)
TEST_GEN_PROGS += brk_aslr_p3s elf_4k_align_p3s exec_aslr_p3s \
	exec_stack_random_p3s
P3S_ASM_PROGS := brk_aslr_p3s_sample elf_4k_align_probe \
	exec_aslr_p3s_helper exec_aslr_p3s_interp
TEST_GEN_FILES := $(P3S_ASM_PROGS)
endif

P3S_MM_HELPERS :=
include ../../p3s_build.mk
LOCAL_HDRS += exec_p3s_helpers.h

ifeq ($(ARCH),arm64)
$(OUTPUT)/brk_aslr_p3s_sample: brk_aslr_p3s_sample.S
	$(CC) -nostdlib -static -no-pie -Wl,-z,max-page-size=16384 $< -o $@
$(OUTPUT)/exec_aslr_p3s_helper: exec_aslr_p3s_helper.S
	$(CC) -nostdlib -pie -Wl,-e,_start -Wl,--dynamic-linker=/proc/self/fd/3 -Wl,--build-id=none -Wl,-z,max-page-size=16384 $< -o $@
$(OUTPUT)/exec_aslr_p3s_interp: exec_aslr_p3s_interp.S
	$(CC) -nostdlib -static-pie -Wl,-e,_start -Wl,--build-id=none -Wl,-z,max-page-size=16384 $< -o $@
$(OUTPUT)/elf_4k_align_probe: elf_4k_align_probe.S
	$(CC) -nostdlib -static -Wl,-no-pie -Wl,--build-id=none -Wl,-z,max-page-size=4096 $< -o $@
endif
