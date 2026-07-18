#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

module=./dmabuf_mmap_ppps_module/dmabuf_mmap_ppps_module.ko
module_name=dmabuf_mmap_ppps_module
loaded=

cleanup()
{
	if [ -n "$loaded" ]; then
		rmmod "$module_name"
	fi
}

if [ "$(id -u)" -ne 0 ]; then
	echo "SKIP: root privileges are required"
	exit 4
fi

if [ ! -e /dev/udmabuf ]; then
	echo "SKIP: /dev/udmabuf is unavailable"
	exit 4
fi

if [ ! -e /dev/dmabuf_mmap_ppps ]; then
	if ! insmod "$module"; then
		echo "FAIL: could not load $module"
		exit 1
	fi
	loaded=1
fi

trap cleanup EXIT
./udmabuf_ppps
