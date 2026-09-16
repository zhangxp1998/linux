#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
here=$(dirname "$0")
runner=$here/ppps_run_module.sh
[ -x "$runner" ] || runner=$here/../../ppps/ppps_run_module.sh
exec "$runner" "$here/ppps_modules/drm_gem_dma_import_ppps_module.ko" \
	/dev/drm_dma_import_ppps -- "$here/drm_gem_dma_import_ppps" "$@"
