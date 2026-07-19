#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

ksft_skip=4
driver=./ttm_vm_fault_ppps_module/ttm_vm_fault_ppps_test.ko
device=/dev/ttm_vm_fault_ppps
created_device=false

if [ ! -f "$driver" ]; then
	driver=./ttm_vm_fault_ppps_test.ko
fi
if [ "$(id -u)" -ne 0 ]; then
	echo "$0: must be run as root"
	exit "$ksft_skip"
fi
if ! command -v insmod >/dev/null 2>&1 || [ ! -f "$driver" ]; then
	echo "$0: test module is unavailable"
	exit "$ksft_skip"
fi

cleanup()
{
	if $created_device; then
		rm -f "$device"
	fi
	rmmod ttm_vm_fault_ppps_test >/dev/null 2>&1 || true
	rmmod ttm >/dev/null 2>&1 || true
}
trap cleanup EXIT

if [ -f ./ttm.ko ]; then
	insmod ./ttm.ko || exit 1
elif command -v modprobe >/dev/null 2>&1; then
	modprobe ttm || exit 1
else
	echo "$0: TTM module is unavailable"
	exit "$ksft_skip"
fi
if ! insmod "$driver"; then
	echo "$0: failed to load the TTM test fixture"
	exit 1
fi
if [ ! -e "$device" ]; then
	dev=$(cat /sys/class/misc/ttm_vm_fault_ppps/dev)
	major=${dev%:*}
	minor=${dev#*:}
	mknod "$device" c "$major" "$minor"
	created_device=true
fi

./ttm_vm_fault_ppps
