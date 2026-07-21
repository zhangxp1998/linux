#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

ksft_skip=4
script_dir=$(dirname "$0")
module="$script_dir/debugfs_str_size_ppps_module/debugfs_str_size_ppps.ko"

if [ "$(id -u)" -ne 0 ]; then
	echo "skip: root privileges are required"
	exit "$ksft_skip"
fi

mounted=0
if ! grep -qs ' /sys/kernel/debug debugfs ' /proc/mounts; then
	mount -t debugfs none /sys/kernel/debug || exit "$ksft_skip"
	mounted=1
fi

insmod "$module" || exit "$ksft_skip"
"$script_dir/debugfs_str_size_ppps"
rc=$?
rmmod debugfs_str_size_ppps
if [ "$mounted" -eq 1 ]; then
	umount /sys/kernel/debug
fi
exit "$rc"
