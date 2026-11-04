#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
exec sh "$here/p3s_run_module.sh" \
	"$here/p3s_modules/pfn_insert_p3s_module.ko" \
	/dev/pfn_insert_p3s -- "$here/pfn_insert_p3s" "$@"
