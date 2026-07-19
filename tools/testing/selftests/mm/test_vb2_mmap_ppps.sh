#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

driver=./vb2_mmap_ppps_module/vb2_mmap_ppps_test.ko
device=/dev/vb2_mmap_ppps

if [ ! -f "$driver" ]; then
	driver=./vb2_mmap_ppps_test.ko
fi

cleanup()
{
	rm -f "$device"
	rmmod vb2_mmap_ppps_test >/dev/null 2>&1 || true
}
trap cleanup EXIT

if ! insmod "$driver"; then
	echo "not ok 1 - load videobuf2 mmap fixture"
	exit 1
fi

dev=$(cat /sys/class/misc/vb2_mmap_ppps/dev) || exit 1
major=${dev%:*}
minor=${dev#*:}
rm -f "$device"
mknod "$device" c "$major" "$minor" || exit 1
chmod 0600 "$device"

./vb2_mmap_ppps --native || exit 1
./vb2_mmap_ppps
