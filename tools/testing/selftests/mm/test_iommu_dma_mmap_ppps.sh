#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

module=./iommu_dma_mmap_ppps_module/iommu_dma_mmap_ppps_module.ko
module_name=iommu_dma_mmap_ppps_module
found=

cleanup()
{
	rmmod "$module_name"
}

if [ "$(id -u)" -ne 0 ]; then
	echo "SKIP: root privileges are required"
	exit 4
fi

for device in /sys/bus/pci/devices/*; do
	if [ "$(cat "$device/vendor" 2>/dev/null)" = "0x1234" ] &&
	   [ "$(cat "$device/device" 2>/dev/null)" = "0x11e8" ] &&
	   [ -e "$device/iommu_group" ]; then
		found=1
		break
	fi
done
if [ -z "$found" ]; then
	echo "SKIP: an IOMMU-mapped QEMU EDU device is required"
	exit 4
fi

if ! insmod "$module"; then
	echo "FAIL: could not load $module"
	exit 1
fi
trap cleanup EXIT
./iommu_dma_mmap_ppps
