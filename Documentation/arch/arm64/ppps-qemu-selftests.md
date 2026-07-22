# Building and testing arm64 PPPS with QEMU

This document describes a reproducible workflow for building an arm64 kernel
with 16 KiB native pages and per-process page-size support (PPPS), booting the
kernel with QEMU, and running the PPPS kselftests. It also describes a 4 KiB
non-PPPS comparison build and a small initramfs workflow for fast regression
testing.

The commands below assume:

- the kernel branch is `ppps_v1_wip`;
- the build machine is an arm64 Ubuntu VM;
- the QEMU host is an Apple Silicon Mac using HVF acceleration;
- the base configuration is `cuttlefish.defconfig`;
- `/home/zhangkelvin/linux-ppps_v1_wip` is the kernel source directory.

The same workflow works on an x86-64 Linux build machine by using an arm64
cross compiler. The QEMU command can also use TCG on Linux when HVF or KVM is
not available.

## 1. Resulting configurations

The PPPS kernel must have all three of these options enabled:

```text
CONFIG_ARM64_16K_PAGES=y
CONFIG_ARM64_VA_BITS_47=y
CONFIG_ARM64_PER_PROCESS_PAGE_SIZE=y
```

`CONFIG_ARM64_PER_PROCESS_PAGE_SIZE` depends on 16 KiB pages, 47-bit virtual
addresses, and an MMU. Enabling only the PPPS symbol is not sufficient.

Keep a second kernel as a control:

```text
CONFIG_ARM64_4K_PAGES=y
# CONFIG_ARM64_PER_PROCESS_PAGE_SIZE is not set
```

Run the same regression against both kernels. The PPPS kernel validates the
4 KiB process-on-16 KiB-kernel behavior, while the 4 KiB kernel detects
regressions accidentally introduced into the normal page-size path.

## 2. Install build dependencies

On the arm64 Ubuntu build VM:

```bash
sudo apt-get update
sudo apt-get install -y \
    bc binutils bison build-essential busybox-static cpio debootstrap \
    dwarves e2fsprogs flex gcc git initramfs-tools kmod libcap-dev \
    libelf-dev libmount-dev libssl-dev pkg-config python3 qemu-utils \
    rsync zlib1g-dev
```

For BPF selftests, also install LLVM and Clang:

```bash
sudo apt-get install -y clang llvm lld
```

On the Apple Silicon Mac used to launch QEMU:

```bash
brew install qemu
```

Check the QEMU binary:

```bash
/opt/homebrew/bin/qemu-system-aarch64 --version
```

## 3. Check out the kernel branch

```bash
kernel_src=/home/zhangkelvin/linux-ppps_v1_wip

git clone --branch ppps_v1_wip --single-branch \
    git@github.com:zhangxp1998/linux.git "$kernel_src"
cd "$kernel_src"
git status --short --branch
git log -1 --oneline
```

Do not build or rewrite history in a worktree that contains unrelated local
changes. Use a clean clone or a separate worktree for reproducible results.

## 4. Create the 16 KiB PPPS configuration

The following commands start with the checked-in Cuttlefish configuration and
let Kconfig resolve all derived page-size symbols:

```bash
cd /home/zhangkelvin/linux-ppps_v1_wip

kernel_src=$PWD
ppps_out=$kernel_src/out-cuttlefish-ppps16k

mkdir -p "$ppps_out"
cp "$kernel_src/cuttlefish.defconfig" "$ppps_out/.config"

scripts/config --file "$ppps_out/.config" \
    --disable ARM64_4K_PAGES \
    --enable ARM64_16K_PAGES \
    --disable ARM64_64K_PAGES \
    --disable ARM64_VA_BITS_39 \
    --enable ARM64_VA_BITS_47 \
    --enable ARM64_PER_PROCESS_PAGE_SIZE

make ARCH=arm64 O="$ppps_out" olddefconfig
```

Do not set generated symbols such as `CONFIG_PAGE_SIZE_16KB` by hand.
`olddefconfig` derives them from the arm64 page-size choice.

Verify that Kconfig did not silently change the requested geometry:

```bash
grep -E '^CONFIG_ARM64_(4K_PAGES|16K_PAGES|64K_PAGES|VA_BITS_39|VA_BITS_47|VA_BITS|PER_PROCESS_PAGE_SIZE)=' \
    "$ppps_out/.config"
grep -E '^CONFIG_PAGE_SIZE_(4KB|16KB|64KB)=' "$ppps_out/.config"
```

The expected result includes:

```text
CONFIG_ARM64_16K_PAGES=y
CONFIG_ARM64_PER_PROCESS_PAGE_SIZE=y
CONFIG_ARM64_VA_BITS_47=y
CONFIG_ARM64_VA_BITS=47
CONFIG_PAGE_SIZE_16KB=y
```

For the selftests in this branch, also verify the relevant facilities:

```bash
grep -E '^CONFIG_(BPF_SYSCALL|DEBUG_INFO_BTF|IO_URING|KVM|MODULES|PERF_EVENTS|PROC_FS|SECURITY_SELINUX|TRACING|USERFAULTFD)=' \
    "$ppps_out/.config"
```

The Cuttlefish configuration intentionally disables some facilities. Tests
for these facilities should report `SKIP`, not `FAIL`:

```text
# CONFIG_NUMA is not set
# CONFIG_SYSVIPC is not set
# CONFIG_9P_FS is not set
```

## 5. Build the PPPS kernel

### Native arm64 GCC build

On an arm64 Ubuntu VM, the simplest build is:

```bash
cd /home/zhangkelvin/linux-ppps_v1_wip

kernel_src=$PWD
ppps_out=$kernel_src/out-cuttlefish-ppps16k

/usr/bin/time -v make \
    ARCH=arm64 \
    O="$ppps_out" \
    -j"$(nproc)" \
    Image modules
```

### LLVM build

To use the Android-style LLVM toolchain instead:

```bash
make \
    ARCH=arm64 \
    O="$ppps_out" \
    LLVM=1 \
    LLVM_IAS=1 \
    -j"$(nproc)" \
    Image modules
```

Use the same toolchain consistently for incremental builds. If the toolchain
changes, use a new output directory.

### x86-64 Linux cross build

On an x86-64 Linux build machine:

```bash
sudo apt-get install -y gcc-aarch64-linux-gnu

make \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- \
    O="$ppps_out" \
    -j"$(nproc)" \
    Image modules
```

The important artifacts are:

```text
out-cuttlefish-ppps16k/arch/arm64/boot/Image
out-cuttlefish-ppps16k/.config
out-cuttlefish-ppps16k/Module.symvers
```

Verify them before constructing a root filesystem:

```bash
test -s "$ppps_out/arch/arm64/boot/Image"
test -s "$ppps_out/Module.symvers"
make -s ARCH=arm64 O="$ppps_out" kernelrelease
file "$ppps_out/arch/arm64/boot/Image"
```

`Module.symvers` is required by the PPPS tests that build external test
modules. Those modules must be built against exactly the kernel that QEMU
boots.

## 6. Build the 4 KiB non-PPPS comparison kernel

The original Cuttlefish configuration is a 4 KiB configuration. Keep it in a
separate output directory:

```bash
cd /home/zhangkelvin/linux-ppps_v1_wip

kernel_src=$PWD
control_out=$kernel_src/out-arm64-4k-noppps

mkdir -p "$control_out"
cp "$kernel_src/cuttlefish.defconfig" "$control_out/.config"

scripts/config --file "$control_out/.config" \
    --enable ARM64_4K_PAGES \
    --disable ARM64_16K_PAGES \
    --disable ARM64_64K_PAGES \
    --disable ARM64_PER_PROCESS_PAGE_SIZE

make ARCH=arm64 O="$control_out" olddefconfig
make ARCH=arm64 O="$control_out" -j"$(nproc)" Image modules
```

Verify the control configuration:

```bash
grep -E '^CONFIG_ARM64_(4K_PAGES|16K_PAGES|PER_PROCESS_PAGE_SIZE)=' \
    "$control_out/.config"
grep -E '^CONFIG_PAGE_SIZE_(4KB|16KB)=' "$control_out/.config"
```

For a full control run, build the selftest modules against `control_out` and
generate a separate control initramfs. Never load a test module built with
16 KiB `PAGE_SIZE` into the 4 KiB kernel, even if the release strings happen
to match.

## 7. Build and install the PPPS selftests

Build selftests with an arm64 userspace compiler. Building them natively in
the arm64 Ubuntu VM avoids mixing x86 host binaries with the arm64 guest.

### Build the MM suite

```bash
cd /home/zhangkelvin/linux-ppps_v1_wip

kernel_src=$PWD
ppps_out=$kernel_src/out-cuttlefish-ppps16k

make -C tools/testing/selftests/mm \
    KDIR="$ppps_out" \
    -j"$(nproc)"
```

The test-module Makefiles use `KDIR` to find `.config`, generated headers, and
`Module.symvers`. A missing or mismatched build directory is the most common
cause of skipped module-backed tests.

### Build all PPPS-related collections

```bash
cd /home/zhangkelvin/linux-ppps_v1_wip

kernel_src=$PWD
ppps_out=$kernel_src/out-cuttlefish-ppps16k
selftest_install=$kernel_src/out-kselftest-ppps

ppps_targets="mm arm64 bpf kvm landlock lsm net perf_events proc ftrace \
seccomp clone3 openat2 mount mount_setattr namespaces filesystems \
filesystems/binderfs filesystems/exfat filesystems/statmount \
drivers/dma-buf drivers/gpu drivers/uio alsa coredump cachestat \
ptp sched scsi timens ublk vDSO vfio"

make -C tools/testing/selftests \
    KDIR="$ppps_out" \
    TARGETS="$ppps_targets" \
    INSTALL_PATH="$selftest_install" \
    -j"$(nproc)" \
    install
```

List the installed PPPS tests:

```bash
cd "$selftest_install"
./run_kselftest.sh --list | grep ppps
```

Some BPF PPPS tests are cases inside `test_progs`, so their case names do not
appear as separate entries in `run_kselftest.sh --list`.

## 8. Recommended: create a full Ubuntu arm64 root filesystem

A full root filesystem is the best way to run the complete kselftest install
tree. A minimal BusyBox initramfs is useful for a single statically linked
regression, but it lacks Bash, shared libraries, system utilities, and the
services needed by many kselftests.

Run the following on the arm64 Ubuntu build VM. Use the same Ubuntu suite as
the build VM so that the selftest binaries and guest libraries have compatible
glibc versions.

```bash
cd /home/zhangkelvin/linux-ppps_v1_wip

kernel_src=$PWD
ppps_out=$kernel_src/out-cuttlefish-ppps16k
selftest_install=$kernel_src/out-kselftest-ppps
guest_work=/home/zhangkelvin/qemu-ppps-guest
root_image=$guest_work/ubuntu-arm64.ext4
root_qcow2=$guest_work/ubuntu-arm64.qcow2
root_mount=$guest_work/rootfs

. /etc/os-release
ubuntu_suite=$VERSION_CODENAME
kernel_release=$(make -s ARCH=arm64 O="$ppps_out" kernelrelease)

mkdir -p "$guest_work" "$root_mount"
truncate -s 8G "$root_image"
mkfs.ext4 -F "$root_image"
sudo mount -o loop "$root_image" "$root_mount"

sudo debootstrap \
    --arch=arm64 \
    "$ubuntu_suite" \
    "$root_mount" \
    http://ports.ubuntu.com/ubuntu-ports
```

Install runtime tools and configure the serial console:

```bash
sudo chroot "$root_mount" apt-get update
sudo chroot "$root_mount" apt-get install -y \
    bash binutils coreutils initramfs-tools iproute2 kmod openssh-server \
    procps sudo util-linux

printf 'root:root\n' | sudo chroot "$root_mount" chpasswd
sudo mkdir -p \
    "$root_mount/etc/systemd/system/getty.target.wants"
sudo ln -sf /lib/systemd/system/serial-getty@.service \
    "$root_mount/etc/systemd/system/getty.target.wants/serial-getty@ttyAMA0.service"

printf '%s\n' \
    'proc /proc proc defaults 0 0' \
    'sysfs /sys sysfs defaults 0 0' \
    'debugfs /sys/kernel/debug debugfs defaults 0 0' \
    'tracefs /sys/kernel/tracing tracefs defaults 0 0' \
    | sudo tee "$root_mount/etc/fstab"
```

Install the matching kernel modules and tests:

```bash
sudo make \
    ARCH=arm64 \
    O="$ppps_out" \
    INSTALL_MOD_PATH="$root_mount" \
    modules_install

sudo mkdir -p "$root_mount/opt/kselftest"
sudo cp -a "$selftest_install/." "$root_mount/opt/kselftest/"
```

Generate an initramfs. It must include the storage driver used for the root
disk. The Cuttlefish configuration commonly builds virtio block as a module,
so booting the ext4 image without this initramfs can fail with an unknown root
device.

```bash
sudo chroot "$root_mount" depmod "$kernel_release"
sudo chroot "$root_mount" update-initramfs -c -k "$kernel_release"

sudo cp "$root_mount/boot/initrd.img-$kernel_release" \
    "$guest_work/initrd-ppps16k.img"

sudo umount "$root_mount"
qemu-img convert -f raw -O qcow2 "$root_image" "$root_qcow2"
```

If the mount is busy, leave the chroot and stop processes using it before
unmounting. Do not force-remove the mounted directory.

## 9. Copy the kernel and guest image to the Mac

On the Mac:

```bash
mkdir -p qemu_run

scp utm:/home/zhangkelvin/linux-ppps_v1_wip/out-cuttlefish-ppps16k/arch/arm64/boot/Image \
    qemu_run/Image-ppps16k
scp utm:/home/zhangkelvin/qemu-ppps-guest/initrd-ppps16k.img \
    qemu_run/initrd-ppps16k.img
scp utm:/home/zhangkelvin/qemu-ppps-guest/ubuntu-arm64.qcow2 \
    qemu_run/ubuntu-arm64.qcow2
```

Copy the 4 KiB control kernel as well:

```bash
scp utm:/home/zhangkelvin/linux-ppps_v1_wip/out-arm64-4k-noppps/arch/arm64/boot/Image \
    qemu_run/Image-arm64-4k-noppps
```

## 10. Boot the PPPS kernel with QEMU on Apple Silicon

From the directory containing `qemu_run`:

```bash
qemu_bin=/opt/homebrew/bin/qemu-system-aarch64
run_dir=$PWD/qemu_run

"$qemu_bin" \
    -machine virt,accel=hvf,gic-version=3 \
    -cpu host \
    -smp 4 \
    -m 4096 \
    -nographic \
    -no-reboot \
    -kernel "$run_dir/Image-ppps16k" \
    -initrd "$run_dir/initrd-ppps16k.img" \
    -drive file="$run_dir/ubuntu-arm64.qcow2",format=qcow2,if=none,id=rootfs \
    -device virtio-blk-pci,drive=rootfs \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=net0 \
    -append "root=/dev/vda rw console=ttyAMA0 earlycon panic=-1"
```

Log in on the serial console as `root` with password `root`. The password is
only for this local test image; do not use this image on an untrusted network.

To use software emulation on a Linux host, replace the machine and CPU
arguments with:

```text
-machine virt,gic-version=3 -cpu max
```

If KVM is available on an arm64 Linux host, use:

```text
-machine virt,accel=kvm,gic-version=3 -cpu host
```

## 11. Verify the running PPPS kernel

Inside the QEMU guest:

```bash
uname -a
if test -r /proc/config.gz; then
    zgrep -E 'CONFIG_ARM64_(16K_PAGES|VA_BITS_47|PER_PROCESS_PAGE_SIZE)=' \
        /proc/config.gz
else
    echo "/proc/config.gz is unavailable; compare the saved build .config"
fi
```

Expected configuration:

```text
CONFIG_ARM64_16K_PAGES=y
CONFIG_ARM64_VA_BITS_47=y
CONFIG_ARM64_PER_PROCESS_PAGE_SIZE=y
```

An executable selected for PPPS should report 4096 through
`sysconf(_SC_PAGESIZE)` or `getconf PAGESIZE`, even though the kernel was built
with 16 KiB native pages. Check the executable's load-segment alignment when a
test unexpectedly remains a native 16 KiB process:

```bash
readelf -l /opt/kselftest/mm/mincore_ppps | grep -A1 LOAD
```

Most PPPS tests either use a 4 KiB-aligned ELF or set
`ADDR_4KB_COMPAT_PAGE_SIZE` and re-exec themselves. Do not globally force the
entire guest into one page-size mode; several remote-mm tests deliberately use
a 4 KiB owner and a native 16 KiB helper in the same test.

Mount optional test filesystems if the root filesystem did not mount them:

```bash
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
mountpoint -q /sys/kernel/tracing || mount -t tracefs nodev /sys/kernel/tracing
mkdir -p /sys/fs/bpf
mountpoint -q /sys/fs/bpf || mount -t bpf bpf /sys/fs/bpf
```

## 12. Run the PPPS selftests

### Run one test

```bash
cd /opt/kselftest

./run_kselftest.sh --list | grep ppps
./run_kselftest.sh \
    --per-test-log /var/log/ppps-selftests \
    --test mm:mincore_ppps
```

The runner uses kselftest exit codes:

- `0`: pass;
- `1`: fail;
- `4`: skip because a configuration, device, permission, or CPU feature is
  unavailable.

### Run the core MM PPPS groups

The MM runner contains the majority of the PPPS tests:

```bash
cd /opt/kselftest/mm

./run_vmtests.sh \
    -t "mmap gup_test userfaultfd mlock mremap process_madv pfnmap"
```

This also runs existing non-PPPS MM tests in those categories. That is useful:
the PPPS changes must not regress the normal MM behavior.

### Run every installed test whose name contains `ppps`

Use Bash arrays so each test identifier remains one argument:

```bash
cd /opt/kselftest

mapfile -t ppps_test_ids < <(./run_kselftest.sh --list | awk '/ppps/ { print $1 }')
ppps_test_args=()
for ppps_test_id in "${ppps_test_ids[@]}"; do
    ppps_test_args+=(--test "$ppps_test_id")
done

./run_kselftest.sh \
    --per-test-log /var/log/ppps-selftests \
    "${ppps_test_args[@]}"
```

Inspect the result:

```bash
grep -R -E '^(not ok|Bail out!)' /var/log/ppps-selftests || true
```

### Run BPF PPPS cases embedded in `test_progs`

```bash
cd /opt/kselftest/bpf

./test_progs -t arena_ppps
./test_progs -t bpf_copy_remote_str_ppps
```

Run BPF and perf tests as root. Restrictive `perf_event_paranoid`, unprivileged
BPF, or lockdown settings can otherwise turn a kernel regression into a
permission failure.

## 13. Fast path: run one test from a minimal initramfs

Use this workflow for a small regression that can be statically linked and
does not require a full distribution. The example runs `mincore_ppps`.

On the arm64 Ubuntu VM:

```bash
cd /home/zhangkelvin/linux-ppps_v1_wip

kernel_src=$PWD
quick_work=/home/zhangkelvin/qemu-ppps-quick
quick_root=$quick_work/rootfs

mkdir -p \
    "$quick_root/bin" \
    "$quick_root/dev" \
    "$quick_root/proc" \
    "$quick_root/sys" \
    "$quick_root/tests" \
    "$quick_root/tmp"

cp /bin/busybox "$quick_root/bin/busybox"
for applet in sh mount poweroff; do
    ln -sf busybox "$quick_root/bin/$applet"
done

gcc -O2 -Wall -Wextra -static \
    -Wl,-z,max-page-size=4096 \
    -I"$kernel_src/tools/testing/selftests" \
    -I"$kernel_src/usr/include" \
    "$kernel_src/tools/testing/selftests/mm/mincore_ppps.c" \
    -o "$quick_root/tests/mincore_ppps"
```

Create `$quick_root/init` with the following contents:

```sh
#!/bin/sh

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

echo "=== PPPS_MINCORE_START ==="
/tests/mincore_ppps
test_status=$?
echo "=== PPPS_MINCORE_STATUS=$test_status ==="

poweroff -f
```

Make it executable and create the archive:

```bash
chmod 0755 "$quick_root/init" "$quick_root/tests/mincore_ppps"

(
    cd "$quick_root"
    find . -print0 | cpio --null -o --format=newc
) >"$quick_work/initramfs-mincore-ppps.cpio"
```

Copy the archive to the Mac and run it:

```bash
scp utm:/home/zhangkelvin/qemu-ppps-quick/initramfs-mincore-ppps.cpio \
    qemu_run/initramfs-mincore-ppps.cpio

/opt/homebrew/bin/qemu-system-aarch64 \
    -machine virt,accel=hvf,gic-version=3 \
    -cpu host \
    -smp 2 \
    -m 2048 \
    -nographic \
    -no-reboot \
    -kernel qemu_run/Image-ppps16k \
    -initrd qemu_run/initramfs-mincore-ppps.cpio \
    -append "console=ttyAMA0 panic=-1"
```

Run the same archive with `Image-arm64-4k-noppps` to obtain the control
result. A reusable regression runner should save the serial output, enforce a
timeout, and require an explicit pass marker rather than relying only on the
QEMU process exit status.

## 14. Interpreting skips and failures

The following skips are expected with the phone-oriented Cuttlefish
configuration:

- NUMA and `mbind` tests when `CONFIG_NUMA=n`;
- SysV shared-memory tests when `CONFIG_SYSVIPC=n`;
- 9P tests when `CONFIG_9P_FS=n`;
- MTE or GCS tests when the emulated CPU does not expose the feature;
- VFIO, UIO, DRM, ALSA, or media tests when the corresponding test device or
  test module is unavailable;
- KVM tests when nested virtualization is unavailable.

Do not convert an expected `SKIP` into a forced feature enablement merely to
make the test count larger. The target configuration should remain
representative of an Android phone.

A `FAIL` is actionable when:

- the required Kconfig symbol is enabled;
- the test module loaded successfully;
- the test is running on the intended kernel;
- the process entered the intended 4 KiB or native 16 KiB page-size mode;
- the failure is reproducible against the PPPS kernel and not caused by the
  root filesystem or QEMU device model.

## 15. Troubleshooting

### Test module reports invalid format or unknown symbols

Rebuild the test and module against the exact running kernel output directory.
Compare:

```bash
uname -r
modinfo ./some_ppps_test_module.ko | grep vermagic
```

Do not reuse a module built against the 4 KiB control output with the 16 KiB
PPPS kernel.

### QEMU cannot mount `/dev/vda`

Ensure the initramfs contains `virtio_blk`, `virtio_pci`, and their
dependencies. Re-run `modules_install`, `depmod`, and `update-initramfs` after
the final kernel build.

### No serial output

Use both `-nographic` and:

```text
console=ttyAMA0 earlycon
```

### A PPPS test reports a 16 KiB process page size

Check the final ELF rather than only the compiler flags:

```bash
readelf -l ./test_binary | grep -A1 LOAD
```

For a minimal regression binary, link with:

```text
-Wl,-z,max-page-size=4096
```

Tests that need both geometries should use the PPPS personality flag and
re-exec instead of applying the linker flag to every helper.

### BPF tests cannot find BTF

Verify:

```bash
test -r /sys/kernel/btf/vmlinux
zgrep CONFIG_DEBUG_INFO_BTF /proc/config.gz
```

### Tests fail with `EPERM`

Run the suite as root and distinguish a permission failure from a PPPS
failure. Check SELinux, kernel lockdown, BPF restrictions,
`perf_event_paranoid`, and `vm.unprivileged_userfaultfd` as applicable. Record
any temporary policy or sysctl change in the test log.

## 16. Completion checklist

Before reporting a PPPS test run as complete, record:

1. kernel commit ID and `uname -r`;
2. the full `.config` or its checksum;
3. QEMU version, machine type, CPU, memory, and acceleration mode;
4. root filesystem or initramfs checksum;
5. PPPS test logs and explicit pass/fail/skip totals;
6. the same regression result on the 4 KiB non-PPPS control kernel;
7. all expected skips and the Kconfig or hardware reason for each skip.

This makes failures reproducible and prevents an old kernel image, stale test
module, or unexpected process page size from being mistaken for a PPPS bug.
