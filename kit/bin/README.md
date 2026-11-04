# Binaries (not in git)

| file | what | built by |
|---|---|---|
| `linux-arm64` | UML kernel for arm64 (Termux glibc, static) | your UML arm64 port + `../patches/*` with `../kernel/config-arm64` |
| `linux-x86_64` | UML kernel for x86_64 hosts | `../patches/*` (x86 parts) with `../kernel/config-x86_64` |
| `umlgpu-arm64` | virtio-gpu backend (`../gpu`) | `../gpu/build-gpu.sh` in Termux (with gfxstream), or cross-built without it |
| `umlgpu-x86_64` | the same for x86_64 | `cc -static -D_GNU_SOURCE -I../gpu/src ../gpu/src/umlgpu.c ../gpu/src/libvhost-user.c` |

Prebuilt copies come with the kit tarballs.
