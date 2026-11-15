#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

ksft_skip=4
selinuxfs=/sys/fs/selinux
mounted_selinuxfs=false

cleanup()
{
	status=$?
	trap - EXIT
	if $mounted_selinuxfs && ! umount "$selinuxfs"; then
		echo "$0: could not unmount test-owned selinuxfs" >&2
		status=1
	fi
	exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [ ! -e "$selinuxfs/status" ]; then
	if [ ! -d "$selinuxfs" ]; then
		echo "$0: selinuxfs is unavailable"
		exit "$ksft_skip"
	fi
	if [ "$(id -u)" -ne 0 ] || ! command -v mount >/dev/null 2>&1; then
		echo "$0: SELinux status is unavailable"
		exit "$ksft_skip"
	fi
	if ! mount -t selinuxfs none "$selinuxfs"; then
		echo "$0: could not mount selinuxfs"
		exit "$ksft_skip"
	fi
	mounted_selinuxfs=true
fi

cd "$(dirname "$0")" || exit 1
./selinux_status_mmap_p3s "$@"
