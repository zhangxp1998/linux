# SPDX-License-Identifier: GPL-2.0
# Makefile for mm selftests

LOCAL_HDRS += $(selfdir)/mm/local_config.h $(top_srcdir)/mm/gup_test.h
LOCAL_HDRS += $(selfdir)/mm/mseal_helpers.h
LOCAL_HDRS += $(selfdir)/mm/pagewalk_ppps.h

include local_config.mk

ifeq ($(ARCH),)

ifeq ($(CROSS_COMPILE),)
uname_M := $(shell uname -m 2>/dev/null || echo not)
else
uname_M := $(shell echo $(CROSS_COMPILE) | grep -o '^[a-z0-9]\+')
endif
ARCH ?= $(shell echo $(uname_M) | sed -e 's/aarch64.*/arm64/' -e 's/ppc64.*/powerpc/')
endif

# Without this, failed build products remain, with up-to-date timestamps,
# thus tricking Make (and you!) into believing that All Is Well, in subsequent
# make invocations:
.DELETE_ON_ERROR:

# Avoid accidental wrong builds, due to built-in rules working just a little
# bit too well--but not quite as well as required for our situation here.
#
# In other words, "make $SOME_TEST" is supposed to fail to build at all,
# because this Makefile only supports either "make" (all), or "make /full/path".
# However,  the built-in rules, if not suppressed, will pick up CFLAGS and the
# initial LDLIBS (but not the target-specific LDLIBS, because those are only
# set for the full path target!). This causes it to get pretty far into building
# things despite using incorrect values such as an *occasionally* incomplete
# LDLIBS.
MAKEFLAGS += --no-builtin-rules

CFLAGS = -Wall -O2 -I $(top_srcdir) $(EXTRA_CFLAGS) $(KHDR_INCLUDES) $(TOOLS_INCLUDES)
LDLIBS = -lrt -lpthread -lm

# Some distributions (such as Ubuntu) configure GCC so that _FORTIFY_SOURCE is
# automatically enabled at -O1 or above. This triggers various unused-result
# warnings where functions such as read() or write() are called and their
# return value is not checked. Disable _FORTIFY_SOURCE to silence those
# warnings.
CFLAGS += -U_FORTIFY_SOURCE

KDIR ?= /lib/modules/$(shell uname -r)/build
ifneq (,$(wildcard $(KDIR)/Module.symvers))
GUP_RETRY_PPPS_MODULE := gup_retry_ppps_module/gup_retry_ppps_module.ko
VM_IOMAP_MEMORY_PPPS_MODULE := vm_iomap_memory_ppps_module/vm_iomap_memory_ppps_module.ko
DMA_MMAP_ATTRS_PPPS_MODULE := dma_mmap_attrs_ppps_module/dma_mmap_attrs_ppps_module.ko
DMA_MMAP_PAGES_PPPS_MODULE := dma_mmap_pages_ppps_module/dma_mmap_pages_ppps_module.ko
VM_MAP_PAGES_PPPS_MODULE := vm_map_pages_ppps_module/vm_map_pages_ppps_module.ko
ifneq (,$(shell grep -s '^CONFIG_MAPPING_DIRTY_HELPERS=y$$' $(KDIR)/.config))
PAGEWALK_PPPS_MODULE := pagewalk_ppps_module/pagewalk_ppps_module.ko
endif
endif

TEST_GEN_FILES = cow
TEST_GEN_FILES += selinux_status_mmap_ppps
TEST_GEN_FILES += vm_iomap_memory_ppps
TEST_GEN_FILES += dma_mmap_attrs_ppps
TEST_GEN_FILES += dma_mmap_pages_ppps
ifeq ($(ARCH),arm64)
TEST_GEN_FILES += elf_4k_align_ppps
TEST_GEN_FILES += elf_4k_align_probe
endif
TEST_GEN_FILES += bpf_array_mmap_ppps
TEST_GEN_FILES += bpf_stack_build_id_ppps
TEST_GEN_FILES += compaction_test
TEST_GEN_FILES += gup_longterm
TEST_GEN_FILES += gup_test
TEST_GEN_FILES += hmm-tests
TEST_GEN_FILES += hugetlb-madvise
TEST_GEN_FILES += hugetlb-read-hwpoison
TEST_GEN_FILES += hugetlb-soft-offline
TEST_GEN_FILES += hugepage-mmap
TEST_GEN_FILES += hugepage-mremap
TEST_GEN_FILES += hugepage-shm
TEST_GEN_FILES += hugepage-vmemmap
TEST_GEN_FILES += khugepaged
TEST_GEN_FILES += madv_populate
TEST_GEN_FILES += madvise_remove_ppps
TEST_GEN_FILES += madvise_willneed_ppps
TEST_GEN_FILES += map_fixed_noreplace
TEST_GEN_FILES += map_hugetlb
TEST_GEN_FILES += map_populate
TEST_GEN_FILES += mbind_ppps
TEST_GEN_FILES += mincore_ppps
TEST_GEN_FILES += mmap_action_cleanup_ppps
TEST_GEN_FILES += mmap_hint_ppps
TEST_GEN_FILES += anon_vma_name_ppps
ifneq (,$(filter $(ARCH),arm64 riscv riscv64 x86 x86_64 loongarch32 loongarch64))
TEST_GEN_FILES += memfd_secret
endif
TEST_GEN_FILES += migration
TEST_GEN_FILES += mkdirty
TEST_GEN_FILES += mlock-random-test
TEST_GEN_FILES += mlock2-tests
TEST_GEN_FILES += mrelease_test
TEST_GEN_FILES += mremap_dontunmap
TEST_GEN_FILES += mremap_test
TEST_GEN_FILES += mremap_rlimit_ppps
TEST_GEN_FILES += msync_offset_ppps
TEST_GEN_FILES += mseal_test
TEST_GEN_FILES += seal_elf
TEST_GEN_FILES += on-fault-limit
TEST_GEN_FILES += pagemap_ioctl
TEST_GEN_FILES += pagewalk_ppps
TEST_GEN_FILES += perf_mmap_offset_ppps
TEST_GEN_FILES += process_madvise_ppps
TEST_GEN_FILES += process_vm_access_ppps
TEST_GEN_FILES += remap_pfn_range_ppps
TEST_GEN_FILES += remap_file_pages_ppps
TEST_GEN_FILES += shmem_swap_usage_ppps
TEST_GEN_FILES += thuge-gen
TEST_GEN_FILES += trace_ring_buffer_mmap_ppps
TEST_GEN_FILES += truncate_ppps
TEST_GEN_FILES += transhuge-stress
TEST_GEN_FILES += uffd-stress
TEST_GEN_FILES += uffd-unit-tests
TEST_GEN_FILES += userfaultfd_eof_ppps
TEST_GEN_FILES += userfaultfd_retry_ppps
TEST_GEN_FILES += userfaultfd_shmem_ppps
TEST_GEN_FILES += userfaultfd_zeropage_ppps
TEST_GEN_FILES += vm_map_pages_ppps
TEST_GEN_FILES += split_huge_page_test
TEST_GEN_FILES += ksm_tests
TEST_GEN_FILES += ksm_functional_tests
TEST_GEN_FILES += mdwe_test
TEST_GEN_FILES += hugetlb_fault_after_madv
TEST_GEN_FILES += hugetlb_madv_vs_map
TEST_GEN_FILES += hugetlb_dio
TEST_GEN_FILES += droppable
TEST_GEN_FILES += guard-regions
TEST_GEN_FILES += $(PPPS_TESTS)
ifeq ($(ARCH),arm64)
TEST_GEN_FILES += $(PPPS_TESTS_arm64)
endif

ifneq ($(ARCH),arm64)
TEST_GEN_FILES += soft-dirty
endif

ifeq ($(ARCH),x86_64)
CAN_BUILD_I386 := $(shell ./../x86/check_cc.sh "$(CC)" ../x86/trivial_32bit_program.c -m32)
CAN_BUILD_X86_64 := $(shell ./../x86/check_cc.sh "$(CC)" ../x86/trivial_64bit_program.c)
CAN_BUILD_WITH_NOPIE := $(shell ./../x86/check_cc.sh "$(CC)" ../x86/trivial_program.c -no-pie)

VMTARGETS := protection_keys
VMTARGETS += pkey_sighandler_tests
BINARIES_32 := $(VMTARGETS:%=%_32)
BINARIES_64 := $(VMTARGETS:%=%_64)

ifeq ($(CAN_BUILD_WITH_NOPIE),1)
CFLAGS += -no-pie
endif

ifeq ($(CAN_BUILD_I386),1)
TEST_GEN_FILES += $(BINARIES_32)
endif

ifeq ($(CAN_BUILD_X86_64),1)
TEST_GEN_FILES += $(BINARIES_64)
endif
else

ifneq (,$(filter $(ARCH),arm64 powerpc))
TEST_GEN_FILES += protection_keys
endif

endif

ifneq (,$(filter $(ARCH),arm64 mips64 parisc64 powerpc riscv64 s390x sparc64 x86_64 s390))
TEST_GEN_FILES += va_high_addr_switch
TEST_GEN_FILES += virtual_address_range
TEST_GEN_FILES += write_to_hugetlbfs
endif

TEST_PROGS := run_vmtests.sh

TEST_FILES := test_vmalloc.sh
TEST_FILES += test_hmm.sh
TEST_FILES += va_high_addr_switch.sh
TEST_FILES += charge_reserved_hugetlb.sh
TEST_FILES += hugetlb_reparenting_test.sh
TEST_FILES += test_gup_retry_ppps.sh
TEST_FILES += $(GUP_RETRY_PPPS_MODULE)
TEST_FILES += test_selinux_status_mmap_ppps.sh
TEST_FILES += test_vm_iomap_memory_ppps.sh
TEST_FILES += $(VM_IOMAP_MEMORY_PPPS_MODULE)
TEST_FILES += test_dma_mmap_attrs_ppps.sh
TEST_FILES += $(DMA_MMAP_ATTRS_PPPS_MODULE)
TEST_FILES += test_dma_mmap_pages_ppps.sh
TEST_FILES += $(DMA_MMAP_PAGES_PPPS_MODULE)
TEST_FILES += test_pagewalk_ppps.sh
TEST_FILES += $(PAGEWALK_PPPS_MODULE)
TEST_FILES += test_vm_map_pages_ppps.sh
TEST_FILES += $(VM_MAP_PAGES_PPPS_MODULE)

# required by charge_reserved_hugetlb.sh
TEST_FILES += write_hugetlb_memory.sh

TEST_GEN_MODS_DIR := ppps_modules

include ../lib.mk

ifneq ($(GUP_RETRY_PPPS_MODULE),)
all: $(GUP_RETRY_PPPS_MODULE)

$(GUP_RETRY_PPPS_MODULE): gup_retry_ppps_module/gup_retry_ppps_module.c \
			  gup_retry_ppps_module/Makefile gup_retry_ppps.h
	+$(MAKE) -C gup_retry_ppps_module KDIR=$(KDIR)

.PHONY: clean_gup_retry_ppps_module
clean_gup_retry_ppps_module:
	+$(MAKE) -C gup_retry_ppps_module KDIR=$(KDIR) clean

clean: clean_gup_retry_ppps_module
endif

ifneq ($(VM_IOMAP_MEMORY_PPPS_MODULE),)
all: $(VM_IOMAP_MEMORY_PPPS_MODULE)

$(VM_IOMAP_MEMORY_PPPS_MODULE): vm_iomap_memory_ppps_module/vm_iomap_memory_ppps_module.c \
				       vm_iomap_memory_ppps_module/Makefile \
				       vm_iomap_memory_ppps.h
	+$(MAKE) -C vm_iomap_memory_ppps_module KDIR=$(KDIR)

.PHONY: clean_vm_iomap_memory_ppps_module
clean_vm_iomap_memory_ppps_module:
	+$(MAKE) -C vm_iomap_memory_ppps_module KDIR=$(KDIR) clean

clean: clean_vm_iomap_memory_ppps_module
endif

ifneq ($(DMA_MMAP_ATTRS_PPPS_MODULE),)
all: $(DMA_MMAP_ATTRS_PPPS_MODULE)

$(DMA_MMAP_ATTRS_PPPS_MODULE): dma_mmap_attrs_ppps_module/dma_mmap_attrs_ppps_module.c \
				     dma_mmap_attrs_ppps_module/Makefile \
				     dma_mmap_attrs_ppps.h
	+$(MAKE) -C dma_mmap_attrs_ppps_module KDIR=$(KDIR)

.PHONY: clean_dma_mmap_attrs_ppps_module
clean_dma_mmap_attrs_ppps_module:
	+$(MAKE) -C dma_mmap_attrs_ppps_module KDIR=$(KDIR) clean

clean: clean_dma_mmap_attrs_ppps_module
endif

ifneq ($(DMA_MMAP_PAGES_PPPS_MODULE),)
all: $(DMA_MMAP_PAGES_PPPS_MODULE)

$(DMA_MMAP_PAGES_PPPS_MODULE): dma_mmap_pages_ppps_module/dma_mmap_pages_ppps_module.c \
				     dma_mmap_pages_ppps_module/Makefile \
				     dma_mmap_pages_ppps.h
	+$(MAKE) -C dma_mmap_pages_ppps_module KDIR=$(KDIR)

.PHONY: clean_dma_mmap_pages_ppps_module
clean_dma_mmap_pages_ppps_module:
	+$(MAKE) -C dma_mmap_pages_ppps_module KDIR=$(KDIR) clean

clean: clean_dma_mmap_pages_ppps_module
endif

ifneq ($(VM_MAP_PAGES_PPPS_MODULE),)
all: $(VM_MAP_PAGES_PPPS_MODULE)

$(VM_MAP_PAGES_PPPS_MODULE): vm_map_pages_ppps_module/vm_map_pages_ppps_module.c \
				    vm_map_pages_ppps_module/Makefile \
				    vm_map_pages_ppps.h
	+$(MAKE) -C vm_map_pages_ppps_module KDIR=$(KDIR)

.PHONY: clean_vm_map_pages_ppps_module
clean_vm_map_pages_ppps_module:
	+$(MAKE) -C vm_map_pages_ppps_module KDIR=$(KDIR) clean

clean: clean_vm_map_pages_ppps_module
endif

ifneq ($(PAGEWALK_PPPS_MODULE),)
all: $(PAGEWALK_PPPS_MODULE)

$(PAGEWALK_PPPS_MODULE): pagewalk_ppps_module/pagewalk_ppps_module.c \
				 pagewalk_ppps_module/Makefile pagewalk_ppps.h
	+$(MAKE) -C pagewalk_ppps_module KDIR=$(KDIR)

.PHONY: clean_pagewalk_ppps_module
clean_pagewalk_ppps_module:
	+$(MAKE) -C pagewalk_ppps_module KDIR=$(KDIR) clean

clean: clean_pagewalk_ppps_module
endif

$(TEST_GEN_PROGS): vm_util.c thp_settings.c
$(TEST_GEN_FILES): vm_util.c thp_settings.c

$(OUTPUT)/uffd-stress: uffd-common.c
$(OUTPUT)/uffd-unit-tests: uffd-common.c

# Headers a PPPS test shares with its fixture module or with the other tuple
# tests.  Declaring them per target (instead of in LOCAL_HDRS) keeps a header
# edit from rebuilding the whole directory; the target-specific LOCAL_HDRS
# keeps lib.mk's link rule from passing the header to the compiler.
define ppps_test_hdrs
$(OUTPUT)/$(1): LOCAL_HDRS += $(2)
$(OUTPUT)/$(1): $(2)
endef
$(eval $(call ppps_test_hdrs,fault_in_ppps,fault_in_ppps.h))
$(eval $(call ppps_test_hdrs,folio_within_range_ppps,folio_within_range_ppps.h))
$(eval $(call ppps_test_hdrs,gup_retry_ppps,gup_retry_ppps.h))
$(eval $(call ppps_test_hdrs,iov_iter_ppps,iov_iter_ppps.h))
$(eval $(call ppps_test_hdrs,mmu_notifier_ppps,mmu_notifier_ppps.h))
$(eval $(call ppps_test_hdrs,pagewalk_ppps,pagewalk_ppps.h))
ifeq ($(ARCH),x86_64)
BINARIES_32 := $(patsubst %,$(OUTPUT)/%,$(BINARIES_32))
BINARIES_64 := $(patsubst %,$(OUTPUT)/%,$(BINARIES_64))

define gen-target-rule-32
$(1) $(1)_32: $(OUTPUT)/$(1)_32
.PHONY: $(1) $(1)_32
endef

define gen-target-rule-64
$(1) $(1)_64: $(OUTPUT)/$(1)_64
.PHONY: $(1) $(1)_64
endef

ifeq ($(CAN_BUILD_I386),1)
$(BINARIES_32): CFLAGS += -m32 -mxsave
$(BINARIES_32): LDLIBS += -lrt -ldl -lm
$(BINARIES_32): $(OUTPUT)/%_32: %.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(notdir $^) $(LDLIBS) -o $@
$(foreach t,$(VMTARGETS),$(eval $(call gen-target-rule-32,$(t))))
endif

ifeq ($(CAN_BUILD_X86_64),1)
$(BINARIES_64): CFLAGS += -m64 -mxsave
$(BINARIES_64): LDLIBS += -lrt -ldl
$(BINARIES_64): $(OUTPUT)/%_64: %.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) $(notdir $^) $(LDLIBS) -o $@
$(foreach t,$(VMTARGETS),$(eval $(call gen-target-rule-64,$(t))))
endif

# x86_64 users should be encouraged to install 32-bit libraries
ifeq ($(CAN_BUILD_I386)$(CAN_BUILD_X86_64),01)
all: warn_32bit_failure

warn_32bit_failure:
	@echo "Warning: you seem to have a broken 32-bit build" 2>&1;		\
	echo  "environment. This will reduce test coverage of 64-bit" 2>&1;	\
	echo  "kernels. If you are using a Debian-like distribution," 2>&1;	\
	echo  "try:"; 2>&1;							\
	echo  "";								\
	echo  "  apt-get install gcc-multilib libc6-i386 libc6-dev-i386";	\
	echo  "";								\
	echo  "If you are using a Fedora-like distribution, try:";		\
	echo  "";								\
	echo  "  yum install glibc-devel.*i686";				\
	exit 0;
endif
endif

# IOURING_EXTRA_LIBS may get set in local_config.mk, or it may be left empty.
$(OUTPUT)/cow: LDLIBS += $(IOURING_EXTRA_LIBS)

$(OUTPUT)/gup_longterm: LDLIBS += $(IOURING_EXTRA_LIBS)

$(OUTPUT)/mlock-random-test $(OUTPUT)/memfd_secret: LDLIBS += -lcap

$(OUTPUT)/ksm_tests: LDLIBS += -lnuma

$(OUTPUT)/migration: LDLIBS += -lnuma

$(OUTPUT)/elf_4k_align_probe: elf_4k_align_probe.S
	$(CC) -nostdlib -static -Wl,-no-pie -Wl,--build-id=none \
		-Wl,-z,max-page-size=4096 $< -o $@

local_config.mk local_config.h: check_config.sh
	/bin/sh ./check_config.sh $(CC)

EXTRA_CLEAN += local_config.mk local_config.h

ifeq ($(IOURING_EXTRA_LIBS),)
all: warn_missing_liburing

warn_missing_liburing:
	@echo ; \
	echo "Warning: missing liburing support. Some tests will be skipped." ; \
	echo
endif
