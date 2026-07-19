ARCH ?= $(shell uname -m)
KDIR ?= $(if $(O),$(O),$(realpath ../../..))

ifeq (,$(filter $(ARCH),aarch64 arm64 x86_64))
# Do nothing on unsupported architectures
include ../lib.mk
else

CFLAGS = $(KHDR_INCLUDES)
TEST_GEN_PROGS += vfio_dma_mapping_test
TEST_GEN_PROGS += vfio_iommufd_setup_test
TEST_GEN_PROGS += vfio_pci_device_test
TEST_GEN_PROGS += vfio_pci_driver_test
TEST_GEN_PROGS += vfio_platform_mmap_ppps
TEST_PROGS_EXTENDED := run.sh

TEST_FILES += scripts/cleanup.sh
TEST_FILES += scripts/lib.sh
TEST_FILES += scripts/run.sh
TEST_FILES += scripts/setup.sh
TEST_PROGS += test-vfio-platform-mmap-ppps.sh

VFIO_PLATFORM_MMAP_PPPS_MODULE := \
	vfio_platform_mmap_ppps_module/vfio_platform_mmap_ppps_test.ko
TEST_FILES += $(VFIO_PLATFORM_MMAP_PPPS_MODULE)

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
