#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
here=$(dirname "$0")
exec "$here/drm_gem_slice_p3s" --native
