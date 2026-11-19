#!/usr/bin/env bash
# Cross-compiles every translation unit for both Android ABIs using zig's
# bundled clang + musl sysroots and the header stubs in tools/arm_stubs.
#
# This is a *code* check (types, ABI, 32/64-bit branches), not the shipping
# build: the real build links against NDK r27d libc++/liblog in GitHub Actions.
# Usage: ZIG=/path/to/zig tools/arm_compile_check.sh
set -uo pipefail
cd "$(dirname "$0")/.."

ZIG_BIN="${ZIG:-zig}"
if ! command -v "$ZIG_BIN" >/dev/null 2>&1; then
    echo "zig not found; set ZIG=/path/to/zig" >&2
    exit 2
fi

SOURCES=(
  src/main.cpp
  src/adapter/gtasa_adapter.cpp
  src/net/android_http.cpp
  src/core/hash.cpp src/core/json.cpp src/core/regions.cpp src/core/heightfield.cpp
  src/core/model_catalog.cpp src/core/chunk_generator.cpp src/core/chunk_streamer.cpp
  src/core/floating_origin.cpp src/core/interior.cpp src/core/world_delta.cpp
  src/core/mission.cpp src/core/test_polygon.cpp src/core/touch_gesture.cpp
  src/core/config.cpp src/net/cloud_sync.cpp
  mod/logger.cpp mod/config.cpp
  mod/thirdparty/imgui/imgui.cpp mod/thirdparty/imgui/imgui_draw.cpp
  mod/thirdparty/imgui/imgui_tables.cpp mod/thirdparty/imgui/imgui_widgets.cpp
  plugin-sdk/renderware/imgui/imgui_impl_renderware.cpp
  plugin-sdk/renderware/imgui/imgui_shared.cpp
)

TARGETS=("aarch64-linux-musl" "arm-linux-musleabihf")
FLAGS=(-std=c++17 -c -o /tmp/usa1to1_check.o -D__ANDROID__ -DNDEBUG -O2
       -fexceptions -frtti -fvisibility=hidden -fvisibility-inlines-hidden
       -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
       -Wno-missing-field-initializers -Wno-unknown-warning-option
       -Itools/arm_stubs -I. -Isrc -Iplugin-sdk -Imod/thirdparty
       # Vendored AML sources rely on bionic pulling in <stddef.h>/<stdlib.h> via
       # <stdint.h>; the musl sysroot does not, so force them for this check only.
       -include cstddef -include cstdio -include cstdlib)

# No cache-dir overrides: the zig CLI changes those flags between releases.
CACHE_ARGS=()

status=0
for target in "${TARGETS[@]}"; do
    echo "=== $target"
    arch_flags=()
    if [[ "$target" == arm-* ]]; then
        arch_flags+=(-mfloat-abi=softfp)
    fi
    for src in "${SOURCES[@]}"; do
        if ! "$ZIG_BIN" "${CACHE_ARGS[@]}" c++ -target "$target" "${FLAGS[@]}" "${arch_flags[@]}" "$src" 2>/tmp/usa1to1_cc.log; then
            echo "FAIL $target $src"
            head -20 /tmp/usa1to1_cc.log
            status=1
        fi
    done
    echo "done: $target"
done

if [[ $status -eq 0 ]]; then
    echo "ALL TRANSLATION UNITS COMPILE FOR BOTH ABIs"
fi
exit $status
