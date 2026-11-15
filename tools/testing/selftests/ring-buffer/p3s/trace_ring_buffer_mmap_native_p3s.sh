#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
here=$(dirname "$0")
exec "$here/trace_ring_buffer_mmap_p3s" --native
