#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Mount debugfs only if this invocation needs to, then restore the prior state.

mounted=0
cleanup()
{
	status=$?
	trap - EXIT
	if [ "$mounted" -eq 1 ] && ! umount /sys/kernel/debug; then
		echo "$0: could not unmount test-owned debugfs" >&2
		status=1
	fi
	exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if ! grep -q ' /sys/kernel/debug debugfs ' /proc/mounts 2>/dev/null; then
	if mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null; then
		mounted=1
	else
		echo "$0: debugfs is unavailable"
		exit 4
	fi
fi

"$@"
