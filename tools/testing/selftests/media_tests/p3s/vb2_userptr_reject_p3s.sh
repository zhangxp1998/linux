#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
here=$(dirname "$0")
modprobe -q videobuf2_vmalloc 2>/dev/null || true
modprobe -q videobuf2_v4l2 2>/dev/null || true
exec sh "$here/../../mm/p3s/p3s_run_module.sh" "$here/p3s_modules/vb2_userptr_reject_p3s_module.ko" /dev/vb2_userptr_reject_p3s -- "$here/vb2_userptr_reject_p3s" "$@"
