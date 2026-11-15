#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
exec sh "$here/p3s_run_module.sh" \
	"$here/p3s_modules/gup_retry_p3s_module.ko" \
	/dev/gup_retry_p3s -- "$here/gup_retry_p3s" "$@"
