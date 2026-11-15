#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
here=$(dirname "$0")
exec "$here/bpf_remote_access_p3s" --native
