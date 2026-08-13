#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Runner for the PPPS TCP receive-zerocopy alignment reproducer.
# Must run as a compat (4K) process on a 16K-native PPPS kernel; the C test
# SKIPs itself otherwise.  Needs no privilege and no real network -- it uses a
# loopback TCP connection with an empty receive queue.

ksft_skip=4

test_bin=./tcp_zerocopy_align_ppps
if [ ! -x "$test_bin" ]; then
	echo "$0: $test_bin not built"
	exit "$ksft_skip"
fi

exec "$test_bin"
