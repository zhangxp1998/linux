#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run udmabuf_ppps with the dmabuf_mmap fixture module loaded; the test
# itself skips when /dev/udmabuf is unavailable.
here=$(dirname "$0")
runner=$here/ppps_run_module.sh
[ -x "$runner" ] || runner=$here/../../ppps/ppps_run_module.sh
exec "$runner" "$here/ppps_modules/dmabuf_mmap_ppps_module.ko" \
	/dev/dmabuf_mmap_ppps -- "$here/udmabuf_ppps"
