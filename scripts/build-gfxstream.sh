#!/bin/bash
# Build gfxstream's host renderer (libgfxstream_backend) for umlgpu.
#
#   scripts/build-gfxstream.sh [BUILD_DIR] [-- extra meson options]
#
# gfxstream (Apache-2.0, https://github.com/google/gfxstream) is pinned to the
# commit umlgpu's gfxstream mode (kit/gpu/src/umlgpu-gfx.inc) is written
# against. Set ANDROID_HOST=1 when the result runs on Android (Termux or the
# app): Android gets Linux's settings except X11. MESON_CROSS=<file> builds
# with a meson cross file (Android NDK).
set -euo pipefail
B=${1:-$PWD/gfxstream-build}
[ $# -gt 0 ] && shift
[ "${1:-}" = "--" ] && shift
REV=94240f814c0097a63027ae680ea4bec7b0cb25f7

mkdir -p "$B"
cd "$B"
if [ ! -d gfxstream/.git ]; then
  git init -q gfxstream
  git -C gfxstream remote add origin https://github.com/google/gfxstream.git
fi
cd gfxstream
git fetch -q --depth 1 origin "$REV"
git checkout -q -f "$REV"

if [ "${ANDROID_HOST:-0}" = 1 ]; then
python3 - <<'PY'
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
fi

CROSS=()
[ -n "${MESON_CROSS:-}" ] && CROSS=(--cross-file "$MESON_CROSS")
[ -d build ] || meson setup build "${CROSS[@]}" -Ddefault_library=shared \
  -Dgfxstream-build=host -Ddecoders=vulkan,composer -Dbuildtype=release \
  -Dlog-level=warn "$@"
ninja -C build
find "$PWD/build" -name 'libgfxstream_backend.so*' | head -3
