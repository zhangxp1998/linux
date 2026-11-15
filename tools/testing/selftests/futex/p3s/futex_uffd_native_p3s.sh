#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
here=$(dirname "$0")
exec "$here/futex_uffd_p3s" --native
