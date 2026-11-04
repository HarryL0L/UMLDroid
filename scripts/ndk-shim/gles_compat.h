/*
 * gfxstream's Vulkan-only build includes "gles_compat.h" for the GL/EGL type
 * names its interfaces mention, but the pinned commit does not have that
 * file. The NDK's EGL and GLES headers declare them (types only: nothing is
 * linked from them).
 */
#pragma once
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
