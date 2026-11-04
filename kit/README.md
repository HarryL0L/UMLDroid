# Android 17 on User-Mode Linux

Runs the Android 17 Cuttlefish system image as an ordinary process: no root,
no KVM, no QEMU. Built for the arm64 UML port running in Termux; the same kit
works on x86_64 Linux.

## Quick start (Termux, arm64)

    pkg install python unzip e2fsprogs curl
    tar xzf android17-uml-kit.tar.gz && cd android17-uml
    ./setup.sh          # downloads Android 17 arm64 (1.1 GB), builds android/
    ./run.sh            # boots it

Then open http://localhost:6080/ in the phone's browser. Tap or drag on the
screen to touch; the buttons send Back, Home and Recents. Ctrl-C in Termux
stops Android.

- First boot: several minutes (Android compiles its Java code once and shows
  "Phone is starting"). Later boots are much faster. On the x86_64 test
  machine: about 5.5 minutes for the first boot, 1 minute after that.
- Swipe up on the lock screen to unlock (no PIN).
- Needs about 5 GB of free storage while setting up (3 GB afterwards) and
  enough free RAM for `--mem` (default 2048M).
- The guest console log is `android/console.log`. To also collect
  Android's crash and watchdog messages there, start with
  `UML_CONSOLE_FEED=host/diag-feed.sh ./run.sh`.

`run.sh` options: `--mem 2048M --cpus 2 --size 720x1280 --density 320
--port 6080 --idlepoll 200 --stubthreads --cpuset LIST --no-viewer`, and `-- <kernel args>` for
anything else.

`--cpuset LIST`: run UML and all its stub processes on these host CPUs
only (`taskset -c LIST`, from `pkg install util-linux`). Measured on a
Dimensity 1200 it made launches slower (`4-7`, the big cores, with 4 UML
CPUs), so it is off by default.

`--idlepoll US`: an idle Android CPU spins this many microseconds before
sleeping on the host. Waking a task on another CPU is then much cheaper
(on phones a host thread wakeup costs over 100 us), at the cost of some
battery while Android is idle. `--idlepoll 0` turns it off.

## Uninstalling

Everything lives in this directory: stop `run.sh` (Ctrl-C) and delete the
`android17-uml` directory. UML also leaves a small `~/.uml` directory
(control sockets); delete it too. Remove the Termux packages with
`pkg uninstall python unzip e2fsprogs curl` if nothing else needs them.
Nothing is installed outside Termux and no root is used.

The viewer only listens on the phone itself (127.0.0.1). Android inside has
no network access in this setup.

`android/disk.img` holds Android's data. Delete it and rerun `setup.sh` to
start from scratch; `setup.sh` never overwrites an existing disk.

## What is where

- `bin/linux-arm64`: the UML kernel for arm64 hosts, built from Linux 7.3
  with your five port patches plus the eighteen in `patches/` (config in
  `kernel/config-arm64`). `bin/linux-x86_64`: the same patches, for x86_64.
- `setup.sh`: unpacks the Cuttlefish image into `android/disk.img` (one GPT
  disk with Android's partitions, built by `host/mkdisk.py`, sparse) and
  `android/initrd.img` (Cuttlefish's two ramdisks plus our first-stage hook).
- `run.sh`: starts UML and the viewer.
- `host/umlview.py`: the browser viewer (Python standard library only).
  `host/umlfb.py`: save a frame as PNG; send taps, swipes and keys from a
  script.
- `guest/`: what runs inside Android. `first_stage.sh` creates the
  /dev/block/by-name links; `init.rc` (installed as /metadata/uml/init.rc
  and used via `androidboot.init_rc`) runs `fixup` before the HALs start and
  `bpfstub` after the BPF loaders. Sources are next to the binaries. `fixup`
  also hides the UWB feature and the media.c2 software store declaration
  (nothing behind them on UML; their clients polled servicemanager every
  second). `bin-arm64/fixup` is built with `~/UML/toolchain/aarch64-glibc-gcc
  -static -O2`.

## Measuring speed

    UML_CONSOLE_FEED=host/bench-feed.sh ./run.sh
    grep BENCH android/console.log      # after about 3 minutes

prints how long a system call, a clock read, a pipe and a Binder round trip,
a page fault and a fork take inside Android (`guest/umlbench.c`). Run
`guest/bin-arm64/umlbench` directly in Termux for the native numbers.

    UML_CONSOLE_FEED=host/launch-feed.sh ./run.sh
    grep -a "^LAUNCH" android/console.log   # after about 6 minutes

times Settings and Clock launches in milliseconds, three rounds each: cold
(app process started fresh) and warm (app still running). The first round
is slower (Android is still compiling and caching).

## Display and touch, for your GLES app

- Frames: the kernel writes every displayed frame into `android/fb.bin`
  (`vkms.export=`). It starts with a 4 KiB header of little-endian fields:
  `u32 magic ("UMFB" = 0x42464d55), version (1), width, height, stride,
  format (DRM_FORMAT_ABGR8888, bytes R G B A), u64 seq, u64 frames`, then
  the pixels. `seq` is odd while a frame is being written: read it, copy the
  pixels, read it again, and use the copy only if both reads match and are
  even. Map the file and upload the pixels as a GL_RGBA texture.
- Touch: write 12-byte records `{u32 type; s32 a; s32 b}` (little-endian)
  to the FIFO `android/touch.fifo` (`umltouch=`): type 1 = finger
  down/move to (a, b) in display pixels, type 0 = finger up, type 2 = key
  `a` (Linux key code: 158 Back, 172 Home, 580 Recents, 116 Power) with
  `b` = 1 press, 0 release.

## Building the kernel yourself (on the phone, with your port's ./mk)

Apply the eighteen patches on top of your port's five (`git am patches/*.patch`
in `src/linux-7.3`; they apply without fuzz), then merge the Android options:

    cat bin/kernel.config <kit>/kernel/android-uml.config \
        <kit>/kernel/android-base-uml.config > build/.config
    ./mk olddefconfig && ./mk

(`merge_config.sh` loses arm64-only options here; appending and
`olddefconfig` keeps them.) Build with clang and Termux's glibc, as `./mk`
does. GCC's arm64 defaults break the UML stub ("Unexpectedly lost MM
child"), and a kernel linked against a stock distribution glibc is killed by
Android ("Bad system call"): its startup calls `rseq` and `set_robust_list`,
which Android's app seccomp filter does not allow. Termux's glibc avoids
both. `kernel/aarch64-termux-clang` is the wrapper used to cross-build
`bin/linux-arm64` on an x86_64 machine against an unpacked Termux glibc
package.

## Stub threads (`--stubthreads`, arm64)

Without stub threads (`nostubthreads`, the default in `run.sh`), all
threads of a process share one host thread, so they take turns: an app's UI
thread, render thread and SwiftShader's drawing threads never run at the
same time. Per-thread stubs (your port's patch) remove that limit.

They used to hang the whole kernel while Android's apexd started: both UML
CPU threads waited in `wait_stub_done_seccomp()` for stub threads that
waited in `sync_wait()` for an earlier ticket. Patch 0014 fixes the likely
cause: a task could be preempted after its page table sync took a ticket
but before its stub had the batch, and nothing ran it again once every CPU
waited for it. `./run.sh --stubthreads` turns stub threads on. Tested on a
phone (Dimensity 1200): it no longer hangs, and with patch 0018 (no stub
thread for a process that has only one thread) fork costs the same as
without. Warm launches get faster, cold ones slower (every new app thread
starts a host thread), so it stays off by default.

## Kernel options added by patches 0015-0018

- vDSO (0015): `clock_gettime()`/`gettimeofday()` without trapping into UML
  (3 us -> 0.3 us); `novdsoclock` makes the vDSO use the system call.
- `stubpool=N` (0017, default 4): started stub processes kept ready for
  fork and exec (fork+exit+wait about 7-9 ms -> 3 ms); 0 turns it off.
- `faultahead=N` (0016, default 0): populate the next N pages of a
  sequential anonymous write fault. Slower on the phone, so off.
- `eagerstubthreads` (0018): give every task a stub thread, as before.

## More console feeds

- `host/compile-feed.sh`: AOT-compile all apps once
  (`COMPILE_MODE=speed-profile` or `speed`, `reset` undoes it).
- `host/probe-feed.sh`: where features/services are declared, idle CPU users.

## Status

- x86_64: tested end to end with this kit: setup from the zip, first boot,
  normal boot, lock screen, home screen, app drawer and apps through the
  viewer.
- arm64: tested under QEMU (TCG, Cortex-A76) with this kernel: boot through
  APEX, dm-verity, the BPF loader (including the arm64 BPF thunk code),
  netd, zygote, SurfaceFlinger and the display. Emulation was too slow to
  reach the home screen. Running it natively on the phone is the next test.
- Known harmless noise: bt_hci, ril, nfc, seriallogging restart in a loop
  (no Cuttlefish host behind them), and media.extractor aborts on its seccomp
  policy. SELinux is permissive.
