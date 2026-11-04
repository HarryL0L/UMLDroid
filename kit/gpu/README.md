# GPU for Android on UML

Android talks to a virtio-gpu device; `umlgpu` (bin/umlgpu-*) is that device,
a vhost-user backend running next to the kernel. `./run.sh --gpu MODE`
starts it and connects the kernel (patches 0019, 0020).

| mode | what it does | status |
|---|---|---|
| `--gpu 2d` | display only: frames reach `android/fb.bin` through virtio-gpu; Android still draws with SwiftShader | works (x86 test) |
| `--gpu gfxstream` | Android's GPU drivers (vulkan.ranchu, ANGLE for GLES) run on the phone's GPU through gfxstream | to be tested on the phone |

- Kernel: 0019 lets Android's composer and gralloc use virtio-gpu like
  vkms; 0020 adds virtio shared memory regions to virtio_uml, which
  host-visible GPU memory needs (the backend passes the memory as FDs).
- `bin/umlgpu-arm64` as shipped is built without gfxstream (2D, and the
  shared memory test mode `UMLGPU_BLOBS=1`). `gpu/build-gpu.sh` builds
  gfxstream in Termux and replaces it with a gfxstream-enabled one.
- Logs: `android/umlgpu.log` (backend), `android/console.log` (guest).
  `UMLGPU_DEBUG=1` in the environment logs every command.

## Shared memory test

    UMLGPU_BLOBS=1 ./run.sh --gpu 2d     # boot, then on the console:
    su 0 /dev/uml/shmtest                # expect four "ok" lines

## gfxstream mode

1. `gpu/build-gpu.sh` (Termux packages: clang, meson, ninja, vulkan
   headers and Android's Vulkan loader; then gfxstream itself).
2. `./run.sh --gpu gfxstream`. umlgpu.log should list capsets 3 (Vulkan)
   and 9 (composer) and "gfxstream ready"; the guest then uses
   vulkan.ranchu and the ranchu composer.
