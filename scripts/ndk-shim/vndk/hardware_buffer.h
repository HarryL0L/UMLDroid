/*
 * Stand-in for the platform's <vndk/hardware_buffer.h> when building
 * gfxstream with the public NDK: gfxstream only uses the AHardwareBuffer
 * functions the NDK has too (allocate, describe, acquire, release).
 */
#pragma once
#include <android/hardware_buffer.h>
