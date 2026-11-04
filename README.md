# UMLDroid

Android 17 running as an ordinary app on an unrooted arm64 phone, with no
root, KVM or QEMU. The Android 17 Cuttlefish system image boots on
**User-Mode Linux**: Linux 7.3 runs as a normal process, on an arm64 UML
port with patches for Android, and Android's own arm64 code runs directly on
the phone's CPU.

Tested so far on a Realme GT Neo (Dimensity 1200, Android 13): Android 17
boots to the home screen, and apps run.

## Pieces

| directory | what |
|---|---|
| `app/` | The Android app ("Android 17 UML"): downloads Android 17, builds its disk, runs the kernel in a foreground service, and shows the screen with touch. Material You UI, boot screen. Gradle project, Java, no NDK. |
| `kit/` | The same for Termux, as scripts (`setup.sh`, `run.sh`), plus everything the app is built from: kernel patches and configs, guest helpers, host tools, the virtio-gpu backend. See `kit/README.md`. |
| `kit/patches/` | Kernel patches 0001-0020, on top of the UML arm64 port (5 patches, not included here). |
| `kit/gpu/` | `umlgpu`, a vhost-user virtio-gpu device for UML: 2D display, and a gfxstream mode that runs Android's GPU drivers on the host GPU (in progress). |
| `scripts/` | Build helpers (`build-gfxstream.sh`). |

## Status

- Boots Android 17 (build 16644040, `aosp_cf_arm64_only_phone`) to the home
  screen. Apps run; drawing is in software (SwiftShader) for now.
- Faster boots: odsign stand-in, no wait for a network card, and a
  "daily use" trim of unneeded system apps plus ahead-of-time compiling.
- Kernel speed-ups: idle polling (cross-CPU wakeups 6x cheaper on the phone),
  a vDSO clock, a pool of stub processes, and a fix for the stub-thread
  deadlock.
- GPU: virtio-gpu over vhost-user works for display (patch 0019), and so
  does GPU memory sharing (0020). gfxstream integration is written, and its
  build and test on the phone are next.

## Building

- Kernel: apply the UML arm64 port, then `kit/patches/*`; merge
  `kit/kernel/android-uml.config` and `android-base-uml.config` (append and
  run `olddefconfig`); build with clang against Termux's glibc
  (`kit/kernel/aarch64-termux-clang` cross-builds on x86_64). Put the result
  in `kit/bin/linux-arm64`.
- App: `cd app && gradle assembleDebug`. It copies `kit/bin/linux-arm64` and
  the guest helpers in. targetSdk is 28 on purpose (see
  `app/app/src/main/AndroidManifest.xml`).
- gfxstream: `scripts/build-gfxstream.sh` (on Android set `ANDROID_HOST=1`),
  or `kit/gpu/build-gpu.sh` in Termux.
