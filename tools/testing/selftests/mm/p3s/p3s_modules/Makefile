# SPDX-License-Identifier: GPL-2.0
# Kernel-side fixtures for the mm P3S selftests, one misc device each.
ifneq ($(KERNELRELEASE),)
ccflags-y += -I$(srctree)/mm

# Fixtures must use the exports of the tested kernel; do not add exports only
# for tests, and do not turn compiler failures into SKIPs.
include $(src)/../../../p3s_fixture_exports.mk

# mremap
obj-m += mremap_pgoff_p3s_module.o

# GUP and iov_iter
$(eval $(call p3s_export_fixture,gup_retry_p3s_module,pin_user_pages_unlocked))
obj-m += iov_iter_p3s_module.o
$(eval $(call p3s_export_fixture,fault_in_p3s_module,fault_in_readable fault_in_safe_writeable fault_in_writeable))
obj-m += vma_address_end_p3s_module.o
obj-m += mmap_fail_p3s_module.o
obj-m += iov_range_retry_p3s_module.o

# PFN and device mappings
obj-m += vm_iomap_memory_p3s_module.o
obj-m += remap_pfn_cow_p3s_module.o
obj-m += pfn_insert_p3s_module.o
obj-m += vm_map_pages_p3s_module.o
obj-m += vmalloc_align_p3s_module.o

# MMU notifier
ifneq ($(CONFIG_MMU_NOTIFIER),)
$(eval $(call p3s_export_fixture,mmu_notifier_p3s_module,mmu_notifier_register mmu_notifier_unregister))
endif

else # userspace selftest build

include ../../../p3s_fixture_modules.mk
endif
