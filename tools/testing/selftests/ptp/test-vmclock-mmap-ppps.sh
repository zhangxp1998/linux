#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

ksft_skip=4
device=/dev/vmclock0
driver=./vmclock_mmap_ppps_module/vmclock_mmap_ppps_test.ko
created_device=false

if [ ! -f "$driver" ]; then
	driver=./vmclock_mmap_ppps_test.ko
fi
if [ "$(id -u)" -ne 0 ]; then
	echo "$0: must be run as root"
	exit "$ksft_skip"
fi
if [ ! -f "$driver" ]; then
	echo "$0: test module is unavailable"
	exit "$ksft_skip"
fi

cleanup()
{
	if $created_device; then
		rm -f "$device"
	fi
	rmmod vmclock_mmap_ppps_test >/dev/null 2>&1 || true
	rmmod ptp_vmclock >/dev/null 2>&1 || true
	rmmod ptp >/dev/null 2>&1 || true
	rmmod pps_core >/dev/null 2>&1 || true
}
trap cleanup EXIT

if [ -f ./pps_core.ko ] && [ -f ./ptp.ko ] && [ -f ./ptp_vmclock.ko ]; then
	insmod ./pps_core.ko && insmod ./ptp.ko && insmod ./ptp_vmclock.ko ||
		exit 1
elif command -v modprobe >/dev/null 2>&1; then
	modprobe ptp_vmclock || exit 1
else
	echo "$0: vmclock module stack is unavailable"
	exit "$ksft_skip"
fi
if ! insmod "$driver"; then
	echo "$0: failed to load the vmclock test stack"
	exit 1
fi
if [ ! -e "$device" ]; then
	dev=$(cat /sys/class/misc/vmclock0/dev)
	major=${dev%:*}
	minor=${dev#*:}
	mknod "$device" c "$major" "$minor"
	created_device=true
fi

./vmclock_mmap_ppps
