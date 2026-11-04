#!/data/data/com.termux/files/usr/bin/bash
# Build gfxstream's host renderer and umlgpu with it, in Termux on the phone.
#
#   gpu/build-gpu.sh        -> bin/umlgpu-arm64 (with gfxstream)
#
# gfxstream (Apache-2.0, https://github.com/google/gfxstream) is pinned to the
# commit umlgpu was written against. Its meson build only special-cases
# Linux; the few Linux-only settings that Android needs too are patched in
# below. Takes a while (a large C++ code base). Build tree: $GPU_BUILD
# (default ~/gfxstream-build).
set -euo pipefail
D=$(cd "$(dirname "$0")" && pwd)
K=$(dirname "$D")
W=${GPU_BUILD:-$HOME/gfxstream-build}
REV=94240f814c0097a63027ae680ea4bec7b0cb25f7

pkg install -y clang meson ninja pkg-config python git vulkan-headers vulkan-loader-android
python -c "import mako" 2>/dev/null || python -m pip install mako

mkdir -p "$W"
cd "$W"
if [ ! -d gfxstream ]; then
  git init -q gfxstream
  git -C gfxstream remote add origin https://github.com/google/gfxstream.git
fi
cd gfxstream
git fetch -q --depth 1 origin "$REV"
git checkout -q -f "$REV"

# Android hosts: Linux's settings (memfd blobs, external sync), not X11.
python - <<'PY'
import re
def ed(p, old, new):
    s = open(p).read()
    if new in s:
        return
    assert old in s, (p, old)
    open(p, "w").write(s.replace(old, new, 1))
ed("meson.build", "elif host_machine.system() == 'linux'\n",
   "elif host_machine.system() in ['linux', 'android']\n")
ed("host/meson.build",
   "if host_machine.system() == 'linux'\n  deps_gfxstream_backend += [\n    thread_dep,\n  ]\n  link_args_gfxstream_backend = ['-Wl,-lpthread,-lrt,-ldl']\n",
   "if host_machine.system() == 'android'\n  deps_gfxstream_backend += [\n    thread_dep,\n  ]\n  link_args_gfxstream_backend = ['-ldl']\nelif host_machine.system() == 'linux'\n  deps_gfxstream_backend += [\n    thread_dep,\n  ]\n  link_args_gfxstream_backend = ['-Wl,-lpthread,-lrt,-ldl']\n")
PY

# Vulkan decoder (Android's guest GLES is ANGLE on Vulkan) and the composer
[ -d build ] || meson setup build -Ddefault_library=shared -Dgfxstream-build=host \
  -Ddecoders=vulkan,composer -Dbuildtype=release -Dlog-level=warn
ninja -C build

LIB=$(dirname "$(find build -name 'libgfxstream_backend.so*' | head -1)")
clang -O2 -Wall -Wno-address-of-packed-member -D_GNU_SOURCE -DHAVE_GFXSTREAM \
  -I"$D/src" -I"$W/gfxstream/host/include" \
  -o "$K/bin/umlgpu-arm64" "$D/src/umlgpu.c" "$D/src/libvhost-user.c" \
  -L"$LIB" -lgfxstream_backend -Wl,-rpath,"$(cd "$LIB" && pwd)"
echo "== built $K/bin/umlgpu-arm64 (with gfxstream)"
