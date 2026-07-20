#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

ksft_skip=4
driver=./mmu_notifier_ppps_module/mmu_notifier_ppps_module.ko
device=/dev/mmu_notifier_ppps
cgroup=/sys/fs/cgroup/ppps-mmu-notifier
created_device=false

if [ ! -f "$driver" ]; then
	driver=./mmu_notifier_ppps_module.ko
fi
if [ "$(id -u)" -ne 0 ]; then
	echo "$0: must be run as root"
	exit "$ksft_skip"
fi
if ! command -v insmod >/dev/null 2>&1 || [ ! -f "$driver" ]; then
	echo "$0: test module is unavailable"
	exit "$ksft_skip"
fi
if [ ! -e /sys/kernel/debug/lru_gen ] || [ ! -e /sys/fs/cgroup/cgroup.procs ]; then
	echo "$0: MGLRU debugfs or cgroup v2 is unavailable"
	exit "$ksft_skip"
fi
if [ "$(awk 'NR > 1 { count++ } END { print count + 0 }' /proc/swaps)" -eq 0 ]; then
	echo "$0: swap is required to drive anonymous-memory reclaim"
	exit "$ksft_skip"
fi

cleanup()
{
	if $created_device; then
		rm -f "$device"
	fi
	rmmod mmu_notifier_ppps_module >/dev/null 2>&1 || true
	rmdir "$cgroup" >/dev/null 2>&1 || true
}
trap cleanup EXIT

if ! insmod "$driver"; then
	echo "$0: failed to load $driver"
	exit 1
fi
if [ ! -e "$device" ]; then
	dev=$(cat /sys/class/misc/mmu_notifier_ppps/dev)
	major=${dev%:*}
	minor=${dev#*:}
	mknod "$device" c "$major" "$minor"
	created_device=true
fi

echo +memory > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null || true
mkdir -p "$cgroup"
(
	echo $$ > "$cgroup/cgroup.procs"
	./mmu_notifier_ppps
)
