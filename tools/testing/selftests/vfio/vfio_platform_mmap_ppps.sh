#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run vfio_platform_mmap_ppps against its fixture module.
here=$(dirname "$0")
runner=$here/ppps_run_module.sh
[ -x "$runner" ] || runner=$here/../ppps/ppps_run_module.sh
exec "$runner" "$here/ppps_modules/vfio_platform_mmap_ppps_module.ko" \
	/dev/vfio_platform_mmap_ppps -- "$here/vfio_platform_mmap_ppps"
