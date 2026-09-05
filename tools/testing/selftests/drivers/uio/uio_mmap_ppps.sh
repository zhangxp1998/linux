#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run uio_mmap_ppps against its fixture UIO device.
here=$(dirname "$0")
runner=$here/ppps_run_module.sh
[ -x "$runner" ] || runner=$here/../../ppps/ppps_run_module.sh
exec "$runner" "$here/ppps_modules/uio_mmap_ppps_module.ko" \
	/dev/uio_mmap_ppps "/sys/class/uio/uio*/dev" -- "$here/uio_mmap_ppps"
