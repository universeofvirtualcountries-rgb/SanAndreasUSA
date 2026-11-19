# ndk-build build for users who prefer it over CMake.
# CI and the documented workflow use CMake (see .github/workflows/build_android.yml),
# this file mirrors the same source list for AMD / manual builds.
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := .cpp .cc

LOCAL_MODULE := USA1to1

LOCAL_SRC_FILES := \
    src/main.cpp \
    src/adapter/gtasa_adapter.cpp \
    src/net/android_http.cpp \
    src/core/hash.cpp \
    src/core/json.cpp \
    src/core/regions.cpp \
    src/core/heightfield.cpp \
    src/core/model_catalog.cpp \
    src/core/chunk_generator.cpp \
    src/core/chunk_streamer.cpp \
    src/core/floating_origin.cpp \
    src/core/interior.cpp \
    src/core/world_delta.cpp \
    src/core/mission.cpp \
    src/core/test_polygon.cpp \
    src/core/touch_gesture.cpp \
    src/core/config.cpp \
    src/net/cloud_sync.cpp \
    mod/logger.cpp \
    mod/config.cpp \
    mod/thirdparty/imgui/imgui.cpp \
    mod/thirdparty/imgui/imgui_draw.cpp \
    mod/thirdparty/imgui/imgui_tables.cpp \
    mod/thirdparty/imgui/imgui_widgets.cpp \
    plugin-sdk/renderware/imgui/imgui_impl_renderware.cpp \
    plugin-sdk/renderware/imgui/imgui_shared.cpp

LOCAL_CFLAGS += -O2 -DNDEBUG -std=c++17 -fexceptions -frtti
LOCAL_CPPFLAGS += -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -fvisibility-inlines-hidden
ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
    LOCAL_CFLAGS += -mfloat-abi=softfp
endif

LOCAL_C_INCLUDES += $(LOCAL_PATH) $(LOCAL_PATH)/src $(LOCAL_PATH)/plugin-sdk $(LOCAL_PATH)/mod/thirdparty
LOCAL_LDLIBS += -llog -landroid
LOCAL_LDFLAGS += -Wl,--exclude-libs,ALL

include $(BUILD_SHARED_LIBRARY)
