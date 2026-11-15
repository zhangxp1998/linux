#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Load a P3S fixture module, make sure its device node exists, run a test
# command against it and unload the module again.
#
#   p3s_run_module.sh <ko> <devnode> -- <command> [<args>...]
#
# Exit codes follow kselftest: 4 (skip) when not root or the module is not
# available, 1 when loading fails, otherwise the command's exit status.

ksft_skip=4
here=$(dirname "$0")
loaded=
created_device=

usage()
{
	echo "usage: $0 <ko> <devnode> -- command..." >&2
	exit 1
}

[ $# -ge 3 ] || usage
ko=$1
device=$2
shift 2
[ "$1" = "--" ] || usage
shift
[ $# -ge 1 ] || usage

module_name=$(basename "$ko" .ko)
sysfs_dev="/sys/class/misc/$(basename "$device")/dev"

cleanup()
{
	status=$?
	trap - EXIT
	if [ -n "$created_device" ]; then
		rm -f "$device"
	fi
	if [ -n "$loaded" ]; then
		if ! rmmod "$module_name"; then
			echo "$0: failed to unload $module_name" >&2
			status=1
		fi
	fi
	exit "$status"
}

if [ "$(id -u)" -ne 0 ]; then
	echo "$0: must be run as root"
	exit "$ksft_skip"
fi
if ! command -v insmod >/dev/null 2>&1; then
	echo "$0: insmod is unavailable"
	exit "$ksft_skip"
fi

for candidate in "$ko" "$here/$ko"; do
	if [ -f "$candidate" ]; then
		ko=$candidate
		break
	fi
done
if [ ! -f "$ko" ]; then
	echo "$0: test module $module_name is unavailable"
	exit "$ksft_skip"
fi

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if insmod "$ko"; then
	loaded=1
elif [ ! -e "$device" ]; then
	echo "$0: failed to load $ko"
	exit 1
fi

if [ ! -e "$device" ]; then
	name=$(basename "$device")
	dev=$(cat "$sysfs_dev" 2>/dev/null)
	if [ -z "$dev" ]; then
		echo "$0: $name was not registered"
		exit 1
	fi
	# The device manager (ueventd/udev) may create the node concurrently.
	if mknod "$device" c "${dev%:*}" "${dev#*:}"; then
		created_device=1
	elif [ ! -e "$device" ]; then
		exit 1
	fi
	chmod 0600 "$device"
fi

"$@"
