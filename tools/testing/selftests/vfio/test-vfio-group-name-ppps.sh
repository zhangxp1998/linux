#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

ksft_skip=4
driver=./vfio_group_name_ppps_module/vfio_group_name_ppps_test.ko
created_nodes=

if [ ! -f "$driver" ]; then
	driver=./vfio_group_name_ppps_test.ko
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
	for node in $created_nodes; do
		rm -f "$node"
	done
	rmmod vfio_group_name_ppps_test >/dev/null 2>&1 || true
}
trap cleanup EXIT

if ! insmod "$driver"; then
	echo "$0: failed to load the VFIO group test fixture"
	exit 1
fi
mkdir -p /dev/vfio
for group in /sys/class/vfio/*; do
	name=${group##*/}
	node=/dev/vfio/$name
	if [ ! -e "$node" ]; then
		dev=$(cat "$group/dev")
		major=${dev%:*}
		minor=${dev#*:}
		mknod "$node" c "$major" "$minor"
		created_nodes="$created_nodes $node"
	fi
done

./vfio_group_name_ppps
