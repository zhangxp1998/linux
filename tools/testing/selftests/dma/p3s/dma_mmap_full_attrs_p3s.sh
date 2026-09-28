#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
here=$(dirname "$0")
exec sh "$here/../../mm/p3s/p3s_run_module.sh" "$here/p3s_modules/dma_mmap_attrs_p3s_module.ko" /dev/dma_mmap_attrs_p3s -- "$here/dma_mmap_full_p3s" attrs
