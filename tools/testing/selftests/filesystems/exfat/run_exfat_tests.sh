#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

KSFT_SKIP=4
TEST_DIR=$(dirname "$0")
WORK_DIR=
LOOP_DEV=

cleanup()
{
	if [ -n "$LOOP_DEV" ]; then
		losetup -d "$LOOP_DEV"
	fi
	if [ -n "$WORK_DIR" ]; then
		rm -rf "$WORK_DIR"
	fi
}
trap cleanup EXIT

if [ "$(id -u)" -ne 0 ]; then
	echo "1..0 # SKIP test requires root"
	exit $KSFT_SKIP
fi
for tool in mkfs.exfat losetup truncate; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "1..0 # SKIP missing $tool"
		exit $KSFT_SKIP
	fi
done

WORK_DIR=$(mktemp -d)
mkdir "$WORK_DIR/mnt"
truncate -s 64M "$WORK_DIR/exfat.img"
mkfs.exfat -L PPPS_TEST "$WORK_DIR/exfat.img" >/dev/null
LOOP_DEV=$(losetup --find --show "$WORK_DIR/exfat.img")

"$TEST_DIR/mmap_valid_size_ppps" "$LOOP_DEV" "$WORK_DIR/mnt"
