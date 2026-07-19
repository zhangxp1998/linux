#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run ttm_vm_fault_ppps against its fixture module, which needs ttm loaded.
here=$(dirname "$0")
runner=$here/ppps_run_module.sh
[ -x "$runner" ] || runner=$here/../../ppps/ppps_run_module.sh
exec "$runner" -d ttm "$here/ppps_modules/ttm_vm_fault_ppps_module.ko" \
	/dev/ttm_vm_fault_ppps -- "$here/ttm_vm_fault_ppps"
