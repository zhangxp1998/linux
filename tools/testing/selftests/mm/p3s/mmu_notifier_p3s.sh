#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
exec sh "$here/p3s_run_module.sh" \
	"$here/p3s_modules/mmu_notifier_p3s_module.ko" \
	/dev/mmu_notifier_p3s -- \
	sh "$here/p3s_run_debugfs.sh" "$here/mmu_notifier_p3s" "$@"
