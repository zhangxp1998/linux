CFLAGS = $(KHDR_INCLUDES)
ARCH ?= $(shell uname -m)
KDIR ?= $(if $(O),$(O),$(realpath ../../../..))

TEST_GEN_PROGS += vfio_dma_mapping_test
TEST_GEN_PROGS += vfio_iommufd_setup_test
TEST_GEN_PROGS += vfio_pci_device_test
TEST_GEN_PROGS += vfio_pci_driver_test

ifneq (,$(filter $(ARCH),aarch64 arm64 x86_64))
TEST_GEN_PROGS += vfio_platform_mmap_ppps
TEST_PROGS += test-vfio-platform-mmap-ppps.sh
VFIO_PLATFORM_MMAP_PPPS_MODULE := \
	vfio_platform_mmap_ppps_module/vfio_platform_mmap_ppps_test.ko
TEST_FILES += $(VFIO_PLATFORM_MMAP_PPPS_MODULE)
endif

TEST_PROGS_EXTENDED := run.sh
include ../lib.mk
include lib/libvfio.mk

CFLAGS += -I$(top_srcdir)/tools/include
CFLAGS += -MD
CFLAGS += $(EXTRA_CFLAGS)

$(TEST_GEN_PROGS): %: %.o $(LIBVFIO_O)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $< $(LIBVFIO_O) $(LDLIBS) -o $@

TEST_GEN_PROGS_O = $(patsubst %, %.o, $(TEST_GEN_PROGS))
TEST_DEP_FILES = $(patsubst %.o, %.d, $(TEST_GEN_PROGS_O) $(LIBVFIO_O))
-include $(TEST_DEP_FILES)

EXTRA_CLEAN += $(TEST_GEN_PROGS_O) $(TEST_DEP_FILES)

ifneq ($(VFIO_PLATFORM_MMAP_PPPS_MODULE),)
all: $(VFIO_PLATFORM_MMAP_PPPS_MODULE)

$(VFIO_PLATFORM_MMAP_PPPS_MODULE): \
		vfio_platform_mmap_ppps_module/vfio_platform_mmap_ppps_test.c \
		vfio_platform_mmap_ppps_module/Makefile
	+$(MAKE) -C vfio_platform_mmap_ppps_module KDIR=$(KDIR)

.PHONY: clean_vfio_platform_mmap_ppps_module
clean_vfio_platform_mmap_ppps_module:
	+$(MAKE) -C vfio_platform_mmap_ppps_module KDIR=$(KDIR) clean

clean: clean_vfio_platform_mmap_ppps_module
endif
