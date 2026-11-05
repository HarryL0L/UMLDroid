#!/usr/bin/env bash
# Boot the Android 17 prepared by setup.sh on User-Mode Linux.
#
#   ./run.sh [--mem 2048M] [--cpus N] [--size 720x1280] [--density 320]
#            [--port 6080] [--idlepoll US] [--stubthreads] [--cpuset LIST]
#            [--no-lite] [--gpu 2d|gfxstream] [--no-viewer]
#            [-- extra kernel args]
#
# The screen is served at http://localhost:6080/ (click or drag to touch;
# Back/Home/Recents buttons). The guest console log is android/console.log.
# Stop with Ctrl-C.
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
A=$D/android
PY=$(command -v python3 || command -v python)
NPROC=$(nproc 2>/dev/null || echo 2)
CPUS=$(( NPROC >= 6 ? 4 : 2 ))
MEM=2048M SIZE=720x1280 DENSITY=320 PORT=6080 VIEWER=1 EXTRA=()
# Idle CPUs spin this many microseconds before sleeping: much cheaper
# wakeups between CPUs, at the cost of some host CPU time (0 = off).
IDLEPOLL=200 STUBTHREADS=0 CPUSET= LITE=1 GPU=
while [ $# -gt 0 ]; do
  case "$1" in
    --mem) MEM=$2; shift 2 ;;
    --cpus) CPUS=$2; shift 2 ;;
    --size) SIZE=$2; shift 2 ;;
    --density) DENSITY=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --idlepoll) IDLEPOLL=$2; shift 2 ;;
    --stubthreads) STUBTHREADS=1; shift ;;
    --cpuset) CPUSET=$2; shift 2 ;;
    --no-lite) LITE=0; shift ;;
    --gpu) GPU=$2; shift 2 ;;
    --no-viewer) VIEWER=0; shift ;;
    --) shift; EXTRA=("$@"); break ;;
    *) echo "run: unknown option $1" >&2; exit 1 ;;
  esac
done
W=${SIZE%x*} H=${SIZE#*x}
case "$(uname -m)" in
  aarch64|arm64) KERNEL=${KERNEL:-$D/bin/linux-arm64}
                 # Per-thread stubs let the threads of a process run in
                 # parallel. Off by default until tested; see README.
                 if [ "$STUBTHREADS" = 1 ]; then ARCHARGS=(); else ARCHARGS=(nostubthreads); fi ;;
  *)             KERNEL=${KERNEL:-$D/bin/linux-x86_64}
                 ARCHARGS=(seccomp=on) ;;
esac
[ -f "$A/disk.img" ] && [ -f "$A/initrd.img" ] || { echo "run: run ./setup.sh first" >&2; exit 1; }

BOOT=(
  androidboot.hardware=cutf_cvm androidboot.slot_suffix=_a androidboot.fstab_suffix=cf.f2fs.hctr2
  androidboot.boot_devices=virtual/block/ubda androidboot.verifiedbootstate=orange
  androidboot.serialno=UML0001 androidboot.selinux=permissive androidboot.first_stage_console=1
  androidboot.force_normal_boot=1 androidboot.init_rc=/dev/uml/init.rc
  androidboot.vendor.apex.com.android.hardware.gatekeeper=com.android.hardware.gatekeeper.nonsecure
  androidboot.vendor.apex.com.android.hardware.keymint=com.android.hardware.keymint.rust_nonsecure
  androidboot.vendor.apex.com.android.hardware.graphics.composer=com.android.hardware.graphics.composer.drm_hwcomposer
  androidboot.vendor.apex.com.google.emulated.camera.provider.hal=com.google.emulated.camera.provider.hal
  androidboot.cpuvulkan.version=4206592 androidboot.hardware.gralloc=minigbm
  androidboot.hardware.hwcomposer=drm androidboot.hardware.hwcomposer.display_finder_mode=drm
  androidboot.hardware.hwcomposer.display_framebuffer_format=rgba androidboot.hardware.egl=angle
  androidboot.hardware.vulkan=pastel androidboot.opengles.version=196609
  androidboot.lcd_density=$DENSITY androidboot.hw_timeout_multiplier=50
  androidboot.serialconsole=1 androidboot.console=ttyS0 androidboot.enable_bootanimation=0
  androidboot.setupwizard_mode=DISABLED androidboot.ddr_size=2048
  androidboot.vsock_lights_port=6900 androidboot.vsock_lights_cid=3
  androidboot.hypervisor.vm.supported=0 androidboot.hypervisor.protected_vm.supported=0
  androidboot.guest_soc.model=UML androidboot.uml_lite=$LITE
)
# --gpu: a virtio-gpu device served by umlgpu (see gpu/README.md) instead of
# vkms. 2d: display only (Android still draws with SwiftShader). gfxstream:
# Android's GPU drivers (vulkan.ranchu, ANGLE on it) run on the host GPU.
GPUARGS=()
case "$GPU" in
  "") ;;
  2d|gfxstream)
    GPUARGS=(vkms.create_default_dev=0 virtio_uml.device=gpu.sock:16)
    if [ "$GPU" = gfxstream ]; then
      for i in "${!BOOT[@]}"; do
        case "${BOOT[$i]}" in
          androidboot.hardware.hwcomposer=*) BOOT[$i]=androidboot.hardware.hwcomposer=ranchu ;;
          androidboot.hardware.vulkan=*) BOOT[$i]=androidboot.hardware.vulkan=ranchu ;;
          androidboot.cpuvulkan.version=*) BOOT[$i]=androidboot.cpuvulkan.version=0 ;;
          androidboot.vendor.apex.com.android.hardware.graphics.composer=*)
            BOOT[$i]=androidboot.vendor.apex.com.android.hardware.graphics.composer=com.android.hardware.graphics.composer.ranchu ;;
        esac
      done
      # as Cuttlefish's gfxstream_guest_angle: the host composer (no hwcomposer.mode)
      BOOT+=(androidboot.hardware.gltransport=virtio-gpu-asg androidboot.uml_logcat=1)
    fi ;;
  *) echo "run: --gpu takes 2d or gfxstream" >&2; exit 1 ;;
esac
KARGS=(
  mem=$MEM ncpus=$CPUS idlepoll=$IDLEPOLL "${ARCHARGS[@]}" ubd0=$A/disk.img initrd=$A/initrd.img
  console=ttyS0 con0=null con=null ssl0=fd:0,fd:1 ssl=null
  printk.devkmsg=on audit=0 8250.nr_uarts=1 firmware_class.path=/vendor/etc/ loop.max_part=7 init=/init
  vkms.width=$W vkms.height=$H vkms.export=$A/fb.bin umltouch=$A/touch.fifo,${W}x${H}
  "${BOOT[@]}" "${GPUARGS[@]}" "${EXTRA[@]}"
)

# --cpuset: run UML (all its threads and stub processes) on these host
# CPUs only, e.g. the big cores (needs taskset: pkg install util-linux).
PIN=()
if [ -n "$CPUSET" ]; then
  command -v taskset >/dev/null || { echo "run: --cpuset needs taskset (pkg install util-linux)" >&2; exit 1; }
  PIN=(taskset -c "$CPUSET")
fi

[ -p "$A/touch.fifo" ] || mkfifo "$A/touch.fifo"
: > "$A/console.log"
PIDS=() KPID=
cleanup() {
  kill "${PIDS[@]}" 2>/dev/null || true
  # UML does not stop on SIGTERM; give it a moment, then kill it.
  if [ -n "$KPID" ]; then
    for _ in 1 2 3 4 5; do kill -0 "$KPID" 2>/dev/null || break; sleep 1; done
    kill -KILL "$KPID" 2>/dev/null || true
  fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM
if [ $VIEWER = 1 ]; then
  "$PY" "$D/host/umlview.py" "$A/fb.bin" "$A/touch.fifo" "$PORT" &
  PIDS+=($!)
fi
if [ -n "$GPU" ]; then
  case "$(uname -m)" in aarch64|arm64) UMLGPU=${UMLGPU:-$D/bin/umlgpu-arm64} ;; *) UMLGPU=${UMLGPU:-$D/bin/umlgpu-x86_64} ;; esac
  [ -x "$UMLGPU" ] || { echo "run: $UMLGPU missing (gpu/build-gpu.sh builds it)" >&2; exit 1; }
  # The kernel connects to it at boot; a relative socket path stays short
  rm -f "$A/gpu.sock"
  ( cd "$A" && if [ "$GPU" = gfxstream ]; then export UMLGPU_GFXSTREAM=1; fi
    exec "$UMLGPU" gpu.sock "$A/fb.bin" "$W" "$H" ) > "$A/umlgpu.log" 2>&1 &
  PIDS+=($!)
  for _ in $(seq 50); do [ -S "$A/gpu.sock" ] && break; sleep 0.1; done
  [ -S "$A/gpu.sock" ] || { echo "run: umlgpu did not start, see android/umlgpu.log" >&2; exit 1; }
  cd "$A"
fi
echo "Android is booting; screen at http://localhost:$PORT/ , log in android/console.log"
# First-stage init opens a console shell (that is how first_stage.sh runs);
# leave it as soon as its prompt appears so the boot continues.
# UML_CONSOLE_FEED=<script> may print more console commands (debugging).
( until grep -q "full job control" "$A/console.log" 2>/dev/null; do sleep 1; done
  sleep 1; printf 'exit\n'
  if [ -n "${UML_CONSOLE_FEED:-}" ]; then bash "$UML_CONSOLE_FEED"; fi
  exec sleep 2147483647 ) |
  "${PIN[@]}" "$KERNEL" "${KARGS[@]}" > "$A/console.log" 2>&1 &
# $! is the kernel (the last command of the pipeline): stopping run.sh, with
# Ctrl-C or otherwise, stops the kernel and the viewer too.
KPID=$!
wait $KPID
