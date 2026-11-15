#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
here=$(dirname "$0")
exec sh "$here/p3s_run_debugfs.sh" "$here/mglru_folded_p3s"
