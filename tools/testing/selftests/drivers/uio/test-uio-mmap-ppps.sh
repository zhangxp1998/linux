#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

ksft_skip=4
driver=./uio_mmap_ppps_module/uio_mmap_ppps_test.ko
device=/dev/uio_mmap_ppps
created_device=false

if [ ! -f "$driver" ]; then
	driver=./uio_mmap_ppps_test.ko
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
	rmmod uio_mmap_ppps_test >/dev/null 2>&1 || true
}
trap cleanup EXIT

if ! insmod "$driver"; then
	echo "$0: failed to load $driver"
	exit 1
fi

uio=
for candidate in /sys/class/uio/uio*; do
	if [ -f "$candidate/name" ] &&
	   [ "$(cat "$candidate/name")" = "uio_mmap_ppps" ]; then
		uio=$candidate
		break
	fi
done
if [ -z "$uio" ]; then
	echo "$0: UIO test device was not registered"
	exit 1
fi
if [ ! -e "$device" ]; then
	dev=$(cat "$uio/dev")
	major=${dev%:*}
	minor=${dev#*:}
	mknod "$device" c "$major" "$minor"
	created_device=true
fi

./uio_mmap_ppps
