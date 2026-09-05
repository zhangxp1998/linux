#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run kvm_pfnmap_ppps against its PFNMAP fixture module.
here=$(dirname "$0")
runner=$here/ppps_run_module.sh
[ -x "$runner" ] || runner=$here/../ppps/ppps_run_module.sh
exec "$runner" "$here/ppps_modules/kvm_pfnmap_ppps_module.ko" \
	/dev/kvm-pfnmap-ppps -- "$here/kvm_pfnmap_ppps"
