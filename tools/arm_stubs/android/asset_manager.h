// COMPILE-CHECK STUB - see tools/arm_stubs/jni.h for why this exists.
#pragma once
#include <stdint.h>
struct AAsset;
struct AAssetDir;
struct AAssetManager { void *reserved; };
struct AAsset { void *reserved; };
enum {
    AASSET_MODE_UNKNOWN = 0,
    AASSET_MODE_RANDOM = 1,
    AASSET_MODE_STREAMING = 2,
    AASSET_MODE_BUFFER = 3,
};
