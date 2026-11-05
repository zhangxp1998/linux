# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 The Android Open Source Project

"""Kleaf build definitions for P3S (per-process page size) kselftests."""

load("@bazel_skylib//lib:paths.bzl", "paths")
load("@rules_cc//cc:defs.bzl", "cc_library")
load(
    "@rules_pkg//pkg:mappings.bzl",
    "pkg_attributes",
    "pkg_filegroup",
    "pkg_files",
    "strip_prefix",
)
load("//build/kernel/kleaf:kernel.bzl", "ddk_headers", "ddk_module")
load(":bazel/abi.bzl", "cc_binary_with_abi")

visibility(["//common/..."])

_P3S_ASM_TARGETS = {
    "brk_aslr_p3s_sample": {
        "features": ["-pie"],
        "linkopts": [
            "-nostdlib",
            "-static",
            "-no-pie",
            "-Wl,-z,max-page-size=16384",
        ],
        "test_dir": "exec/p3s",
    },
    "elf_4k_align_probe": {
        "features": ["-pie"],
        "linkopts": [
            "-nostdlib",
            "-static",
            "-Wl,-no-pie",
            "-Wl,--build-id=none",
            "-Wl,-z,max-page-size=4096",
        ],
        "test_dir": "exec/p3s",
    },
    "exec_aslr_p3s_helper": {
        "features": [],
        "linkopts": [
            "-nostdlib",
            "-pie",
            "-Wl,-e,_start",
            "-Wl,--dynamic-linker=/proc/self/fd/3",
            "-Wl,--build-id=none",
            "-Wl,-z,max-page-size=16384",
        ],
        "test_dir": "exec/p3s",
    },
    "exec_aslr_p3s_interp": {
        "features": ["-pie"],
        "linkopts": [
            "-nostdlib",
            "-static-pie",
            "-Wl,-e,_start",
            "-Wl,--build-id=none",
            "-Wl,-z,max-page-size=16384",
        ],
        "test_dir": "exec/p3s",
    },
}

_P3S_TEST_DIRS = [
    "mm/p3s",
    "filesystems/binderfs/p3s",
    "alsa/p3s",
    "android/p3s",
    "arm64/mte/p3s",
    "bpf/p3s",
    "coredump/p3s",
    "dma/p3s",
    "dmabuf-heaps/p3s",
    "drivers/gpu/p3s",
    "exec/p3s",
    "ftrace/p3s",
    "futex/p3s",
    "io_uring/p3s",
    "kcov/p3s",
    "lsm/p3s",
    "media_tests/p3s",
    "memfd/p3s",
    "net/p3s",
    "pci/p3s",
    "perf_events/p3s",
    "proc/p3s",
    "ring-buffer/p3s",
    "splice/p3s",
]

# The 15 GKI KMI-compatible fixture modules. gup_retry_p3s_module,
# fault_in_p3s_module, mmu_notifier_p3s_module, and iommu_dma_mmap_p3s_module
# depend on symbols trimmed by trim_nonlisted_kmi = True on GKI and are
# intentionally omitted so p3s_run_module.sh skips them cleanly with exit 4.
_P3S_FIXTURE_MODULES = {
    "dma": [
        "dma_mmap_attrs_p3s_module",
        "dma_mmap_pages_p3s_module",
        "dma_mmap_bounds_p3s_module",
    ],
    "media_tests": [
        "vb2_mmap_p3s_module",
        "vb2_userptr_reject_p3s_module",
    ],
    "mm": [
        "mremap_pgoff_p3s_module",
        "iov_iter_p3s_module",
        "vma_address_end_p3s_module",
        "mmap_fail_p3s_module",
        "iov_range_retry_p3s_module",
        "vm_iomap_memory_p3s_module",
        "remap_pfn_cow_p3s_module",
        "pfn_insert_p3s_module",
        "vm_map_pages_p3s_module",
        "vmalloc_align_p3s_module",
    ],
}

def _p3s_ko_files_impl(ctx):
    kos = [
        file
        for target in ctx.attr.modules
        for file in target[DefaultInfo].files.to_list()
        if file.extension == "ko"
    ]
    return [DefaultInfo(files = depset(kos))]

_p3s_ko_files = rule(
    implementation = _p3s_ko_files_impl,
    attrs = {
        "modules": attr.label_list(),
    },
)

def define_p3s_kselftests(name, kselftest_dir, kselftest_copts):
    """Defines userspace P3S kselftest targets for arm64 tests.zip.

    Args:
      name: Name prefix for generated kselftest targets.
      kselftest_dir: Package-relative kselftest root directory.
      kselftest_copts: Common compiler flags inherited from GKI kselftests.
    """
    p3s_copts = kselftest_copts + [
        "-U_FORTIFY_SOURCE",
        "-ffile-prefix-map=common/tools/testing/selftests/mm/p3s/=",
        "-ffile-prefix-map=common/tools/testing/selftests/mm/=",
    ]

    lib_name = name + "_lib"
    cc_library(
        name = lib_name,
        hdrs = native.glob(
            ["tools/testing/selftests/" + d + "/*.h" for d in _P3S_TEST_DIRS],
            allow_empty = True,
        ) + [
            "tools/testing/selftests/kselftest_p3s.h",
            "tools/testing/selftests/p3s_harness.h",
        ],
        copts = p3s_copts,
        includes = [
            "include/uapi/",
            "tools/testing/selftests",
            "tools/testing/selftests/mm",
            "tools/testing/selftests/mm/p3s",
        ],
        visibility = ["//visibility:private"],
        deps = [
            ":kselftest_headers_lib",
            ":kselftest_mm_vm_util",
        ],
    )

    pkg_srcs = []

    for test_dir in _P3S_TEST_DIRS:
        for src in native.glob(["tools/testing/selftests/" + test_dir + "/*.c"]):
            stem = paths.split_extension(paths.basename(src))[0]
            target_name = name + "_" + stem
            cc_binary_with_abi(
                name = target_name,
                srcs = [src],
                out = test_dir + "/" + stem,
                abis = ["arm64"],
                copts = p3s_copts,
                linkopts = ["-Wl,-z,max-page-size=16384"],
                path_prefix = kselftest_dir,
                target_compatible_with = ["@platforms//os:android"],
                visibility = ["//visibility:private"],
                deps = [
                    ":kselftest_headers_lib",
                    ":" + lib_name,
                    ":kselftest_mm_vm_util",
                ],
            )
            pkg_srcs.append(":" + target_name + "_arm64")

    pagemap_name = name + "_pagemap_ioctl_bin"
    cc_binary_with_abi(
        name = pagemap_name,
        srcs = ["tools/testing/selftests/mm/pagemap_ioctl.c"],
        out = "mm/p3s/pagemap_ioctl",
        abis = ["arm64"],
        copts = p3s_copts,
        linkopts = ["-Wl,-z,max-page-size=16384"],
        path_prefix = kselftest_dir,
        target_compatible_with = ["@platforms//os:android"],
        visibility = ["//visibility:private"],
        deps = [
            ":kselftest_headers_lib",
            ":" + lib_name,
            ":kselftest_mm_vm_util",
        ],
    )
    pkg_srcs.append(":" + pagemap_name + "_arm64")

    for stem, cfg in _P3S_ASM_TARGETS.items():
        target_name = name + "_" + stem
        cc_binary_with_abi(
            name = target_name,
            srcs = ["tools/testing/selftests/" + cfg["test_dir"] + "/" + stem + ".S"],
            out = cfg["test_dir"] + "/" + stem,
            abis = ["arm64"],
            features = cfg["features"],
            linkopts = cfg["linkopts"],
            path_prefix = kselftest_dir,
            target_compatible_with = ["@platforms//os:android"],
            visibility = ["//visibility:private"],
        )
        pkg_srcs.append(":" + target_name + "_arm64")

    for test_dir in _P3S_TEST_DIRS:
        scripts = native.glob(["tools/testing/selftests/" + test_dir + "/*.sh"], allow_empty = True)
        if scripts:
            scripts_name = name + "_" + test_dir.replace("/", "_").replace("-", "_") + "_scripts_arm64"
            pkg_files(
                name = scripts_name,
                srcs = scripts,
                attributes = pkg_attributes(mode = "0755"),
                renames = {
                    script: paths.join(kselftest_dir, "arm64", test_dir, paths.basename(script))
                    for script in scripts
                },
                visibility = ["//visibility:private"],
            )
            pkg_srcs.append(":" + scripts_name)

    pkg_filegroup(
        name = name + "_arm64",
        srcs = pkg_srcs,
        visibility = ["//visibility:private"],
    )

def define_p3s_kselftest_modules(name):
    """Defines shared fixture headers and collects packaged KMI modules.

    Args:
      name: Name of the generated pkg_filegroup and header prefix.
    """
    ddk_headers(
        name = name + "_hdrs",
        hdrs = native.glob([
            "tools/testing/selftests/mm/p3s/*.h",
            "tools/testing/selftests/dma/p3s/*.h",
        ]) + [
            "mm/internal.h",
            "mm/p3s/filemap.h",
            "mm/p3s/pgtable.h",
            "mm/p3s/rmap.h",
            "mm/vma.h",
        ],
        includes = ["mm"],
        visibility = ["//common/tools/testing/selftests:__subpackages__"],
    )

    pkg_filegroup(
        name = name,
        srcs = [
            "//common/tools/testing/selftests/" + subsys + "/p3s/p3s_modules:" + name
            for subsys in _P3S_FIXTURE_MODULES
        ],
        visibility = ["//visibility:private"],
    )

def define_p3s_subsystem_modules(name, subsys, kernel_build, kselftest_dir, deps):
    """Defines KMI-compatible P3S fixture DDK modules for one subsystem.

    Args:
      name: Name of the generated pkg_files target.
      subsys: Subsystem directory key in _P3S_FIXTURE_MODULES.
      kernel_build: Target kernel_build label for the DDK modules.
      kselftest_dir: Package-relative kselftest root directory.
      deps: List of ddk_headers dependencies providing fixture headers.
    """
    mod_targets = []
    for mod in _P3S_FIXTURE_MODULES[subsys]:
        ddk_module(
            name = mod,
            srcs = [mod + ".c"],
            out = mod + ".ko",
            kernel_build = kernel_build,
            visibility = ["//visibility:private"],
            deps = deps,
        )
        mod_targets.append(":" + mod)

    kos_name = name + "_kos"
    _p3s_ko_files(
        name = kos_name,
        modules = mod_targets,
        visibility = ["//visibility:private"],
    )

    pkg_files(
        name = name,
        srcs = [":" + kos_name],
        attributes = pkg_attributes(mode = "0644"),
        prefix = paths.join(kselftest_dir, "arm64", subsys, "p3s/p3s_modules"),
        strip_prefix = strip_prefix.files_only(),
        visibility = ["//common:__pkg__"],
    )
