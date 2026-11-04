/*
 * Stand-in for the platform's <cutils/native_handle.h> when building
 * gfxstream with the public NDK (and -DANDROID, as platform builds have).
 */
#pragma once
#include <stdint.h>

typedef struct native_handle {
    int version; /* sizeof(native_handle_t) */
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;
