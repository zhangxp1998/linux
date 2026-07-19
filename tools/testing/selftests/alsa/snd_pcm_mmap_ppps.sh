#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Run snd_pcm_mmap_ppps against its fixture module.
here=$(dirname "$0")
runner=$here/ppps_run_module.sh
[ -x "$runner" ] || runner=$here/../ppps/ppps_run_module.sh
exec "$runner" "$here/ppps_modules/snd_pcm_mmap_ppps_module.ko" \
	/dev/snd_pcm_mmap_ppps -- "$here/snd_pcm_mmap_ppps"
