#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

ksft_skip=4
driver=./pagewalk_ppps_module/pagewalk_ppps_module.ko
device=/dev/pagewalk_ppps
created_device=false

if [ ! -f "$driver" ]; then
	driver=./pagewalk_ppps_module.ko
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
	rmmod pagewalk_ppps_module >/dev/null 2>&1 || true
}
trap cleanup EXIT

if ! insmod "$driver"; then
	echo "$0: failed to load $driver"
	exit 1
fi
if [ ! -e "$device" ]; then
	dev=$(cat /sys/class/misc/pagewalk_ppps/dev)
	major=${dev%:*}
	minor=${dev#*:}
	mknod "$device" c "$major" "$minor"
	created_device=true
fi

./pagewalk_ppps
