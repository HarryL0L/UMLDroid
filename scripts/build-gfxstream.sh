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
# GL: Android is __linux__ but has no GLX; use the Android native window
ed("host/gl/glestranslator/common/gl_dispatch.cpp", "#ifdef __linux__\n#include <GL/glx.h>",
   "#if defined(__linux__) && !defined(__ANDROID__)\n#include <GL/glx.h>")
ed("host/native_window/meson.build",
   "elif host_machine.system() == 'linux' and use_gles\n",
   "elif host_machine.system() == 'android'\n  files_lib_host_native_window += files('native_sub_window_android.cpp')\nelif host_machine.system() == 'linux' and use_gles\n")
ed("meson.build", "elif host_machine.system() == 'linux'\n",
   "elif host_machine.system() in ['linux', 'android']\n")
ed("host/meson.build",
   "if host_machine.system() == 'linux'\n  deps_gfxstream_backend += [\n    thread_dep,\n  ]\n  link_args_gfxstream_backend = ['-Wl,-lpthread,-lrt,-ldl']\n",
   "if host_machine.system() == 'android'\n  deps_gfxstream_backend += [\n    thread_dep,\n  ]\n  link_args_gfxstream_backend = ['-ldl']\nelif host_machine.system() == 'linux'\n  deps_gfxstream_backend += [\n    thread_dep,\n  ]\n  link_args_gfxstream_backend = ['-Wl,-lpthread,-lrt,-ldl']\n")
# AndroidAHB mode (the only mode on Android hosts): ColorBuffers are
# AHardwareBuffers, and images that share them are created for AHB handles,
# but importExternalMemory() imported their memory as an opaque FD, which
# drivers reject (VK_ERROR_INVALID_EXTERNAL_HANDLE). Import the AHB itself.
ed("host/vulkan/vk_common_operations.cpp",
   "    VkImportMemoryHostPointerInfoEXT importInfoHostPtr;\n",
   "    VkImportMemoryHostPointerInfoEXT importInfoHostPtr;\n"
   "#ifdef __ANDROID__\n"
   "    VkImportAndroidHardwareBufferInfoANDROID importInfoAhb;\n"
   "#endif\n")
ed("host/vulkan/vk_common_operations.cpp",
   "    switch (mDeviceInfo.externalMemoryMode) {\n"
   "        case ExternalMemory::Mode::AndroidAHB:\n"
   "        case ExternalMemory::Mode::OpaqueFd: {\n"
   "            auto dupHandle = dupExternalMemory(handleInfo);",
   "    switch (mDeviceInfo.externalMemoryMode) {\n"
   "        case ExternalMemory::Mode::AndroidAHB:\n"
   "#ifdef __ANDROID__\n"
   "            if (handleInfo && handleInfo->streamHandleType == STREAM_HANDLE_TYPE_PLATFORM_AHB) {\n"
   "                importInfoAhb = {\n"
   "                    VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,\n"
   "                    dedicatedAllocInfoPtr,\n"
   "                    reinterpret_cast<AHardwareBuffer*>(static_cast<intptr_t>(handleInfo->handle)),\n"
   "                };\n"
   "                importInfoPtr = &importInfoAhb;\n"
   "                break;\n"
   "            }\n"
   "            [[fallthrough]];\n"
   "#endif\n"
   "        case ExternalMemory::Mode::OpaqueFd: {\n"
   "            auto dupHandle = dupExternalMemory(handleInfo);")
PY
fi

# Vulkan-only builds (no GLES decoder) do not compile at the pinned commit:
# host/gles_compat.h was deleted upstream (Aug 2026) and a few ColorBuffer
# checks use the GL backing unguarded. Restore the header (as it was, plus
# one GL enum still used) and make those checks constant without GLES.
python3 - <<'PY'
import os
if True:
    open("host/gles_compat.h", "w").write("""\
// Restored for Vulkan-only builds (removed upstream in f85b46742).
#ifndef GLES_COMPAT_H
#define GLES_COMPAT_H

#include <cstdint>
// GL enums the format tables use (constants and types only, no EGL)
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

typedef unsigned int GLenum;
typedef int32_t EGLint;
typedef void* EGLNativeWindowType;

#ifndef GL_BGR10_A2_ANGLEX
#define GL_BGR10_A2_ANGLEX 0x6AF9
#endif

namespace gfxstream {
namespace gl {
class EmulationGl {
   public:
    EmulationGl() {}
    ~EmulationGl() {}

   private:
    uint32_t payload;
};
}  // namespace gl
}  // namespace gfxstream

#endif
""")
p = "host/color_buffer.cpp"
s = open(p).read()
if "UMLDROID_GL_BACKING" not in s:
    s = s.replace("!(mColorBufferGl && mColorBufferVk)", "!(UMLDROID_GL_BACKING && mColorBufferVk)")
    i = s.index("namespace gfxstream")
    s = s[:i] + """#if GFXSTREAM_ENABLE_HOST_GLES
#define UMLDROID_GL_BACKING mColorBufferGl
#else
#define UMLDROID_GL_BACKING false
#endif

""" + s[i:]
    open(p, "w").write(s)
PY

CROSS=()
[ -n "${MESON_CROSS:-}" ] && CROSS=(--cross-file "$MESON_CROSS")
[ -d build ] || meson setup build "${CROSS[@]}" -Ddefault_library=shared \
  -Dgfxstream-build=host -Ddecoders=${DECODERS:-auto} -Dbuildtype=release \
  -Dlog-level=warn "$@"
ninja -C build
find "$PWD/build" -name 'libgfxstream_backend.so*' | head -3
