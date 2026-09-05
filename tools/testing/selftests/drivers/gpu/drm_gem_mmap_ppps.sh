#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run drm_gem_mmap_ppps against its fixture DRM device (a render node).
here=$(dirname "$0")
runner=$here/ppps_run_module.sh
[ -x "$runner" ] || runner=$here/../../ppps/ppps_run_module.sh
# A stale node may point at another driver's render node (e.g. virtio-gpu);
# the fixture registers its device under its own platform parent.
rm -f /dev/drm_gem_mmap_ppps
exec "$runner" "$here/ppps_modules/drm_gem_mmap_ppps_module.ko" \
	/dev/drm_gem_mmap_ppps \
	"/sys/devices/drm_gem_mmap_ppps_parent/drm/renderD*/dev" -- \
	"$here/drm_gem_mmap_ppps"
