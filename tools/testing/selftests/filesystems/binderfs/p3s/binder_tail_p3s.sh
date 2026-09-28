#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
cd "$(dirname "$0")" || exit 1
exec ./binder_alloc_p3s tail
