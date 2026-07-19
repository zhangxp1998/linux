#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

ksft_skip=4
driver=./drm_gem_mmap_ppps_module/drm_gem_mmap_ppps_test.ko
device=/dev/drm_gem_mmap_ppps
created_device=false

if [ ! -f "$driver" ]; then
	driver=./drm_gem_mmap_ppps_test.ko
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
	rmmod drm_gem_mmap_ppps_test >/dev/null 2>&1 || true
}
trap cleanup EXIT

if ! insmod "$driver"; then
	echo "$0: failed to load $driver"
	exit 1
fi

render_node=
for candidate in /sys/class/drm/renderD*; do
	if [ -f "$candidate/dev" ]; then
		render_node=$candidate
		break
	fi
done
if [ -z "$render_node" ]; then
	echo "$0: DRM render node was not registered"
	exit 1
fi
if [ ! -e "$device" ]; then
	dev=$(cat "$render_node/dev")
	major=${dev%:*}
	minor=${dev#*:}
	mknod "$device" c "$major" "$minor"
	created_device=true
fi

./drm_gem_mmap_ppps
