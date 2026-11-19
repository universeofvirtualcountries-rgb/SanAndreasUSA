cmake_minimum_required(VERSION 3.22)
project(USA1to1 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Engine-neutral core: compiled and unit tested on the host as well.
set(CORE_SOURCES
    src/core/hash.cpp
    src/core/json.cpp
    src/core/regions.cpp
    src/core/heightfield.cpp
    src/core/model_catalog.cpp
    src/core/chunk_generator.cpp
    src/core/chunk_streamer.cpp
    src/core/floating_origin.cpp
    src/core/interior.cpp
    src/core/world_delta.cpp
    src/core/mission.cpp
    src/core/test_polygon.cpp
    src/core/touch_gesture.cpp
    src/core/config.cpp
    src/net/cloud_sync.cpp
)

if(ANDROID)
    # ------------------------------------------------------------------ mod
    set(MOD_SOURCES
        src/main.cpp
        src/adapter/gtasa_adapter.cpp
        src/net/android_http.cpp
        mod/logger.cpp
        mod/config.cpp
        mod/thirdparty/imgui/imgui.cpp
        mod/thirdparty/imgui/imgui_draw.cpp
        mod/thirdparty/imgui/imgui_tables.cpp
        mod/thirdparty/imgui/imgui_widgets.cpp
        plugin-sdk/renderware/imgui/imgui_impl_renderware.cpp
        plugin-sdk/renderware/imgui/imgui_shared.cpp
        ${CORE_SOURCES}
    )

    add_library(USA1to1 SHARED ${MOD_SOURCES})

    target_include_directories(USA1to1 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/plugin-sdk
    )

    target_compile_options(USA1to1 PRIVATE
        -O2
        -fvisibility=hidden
        -fvisibility-inlines-hidden
        -fexceptions
        -frtti
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Wno-unused-variable
        -Wno-missing-field-initializers
    )

    if(ANDROID_ABI STREQUAL "armeabi-v7a")
        target_compile_options(USA1to1 PRIVATE -mfloat-abi=softfp)
    endif()

    target_link_libraries(USA1to1 PRIVATE log android)
    # Keep libc++ and our own symbols private: other AML mods may be built with
    # a different NDK and must not interpose us.
    target_link_options(USA1to1 PRIVATE -Wl,--exclude-libs,ALL)

    set_target_properties(USA1to1 PROPERTIES OUTPUT_NAME "USA1to1")
else()
    # ------------------------------------------------------------- host build
    add_library(usa1to1_core STATIC ${CORE_SOURCES})
    target_include_directories(usa1to1_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_compile_options(usa1to1_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

    add_executable(usa1to1_tests tests/test_main.cpp)
    target_link_libraries(usa1to1_tests PRIVATE usa1to1_core)
    target_include_directories(usa1to1_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_options(usa1to1_tests PRIVATE -Wall -Wextra)
    target_compile_definitions(usa1to1_tests PRIVATE USA1TO1_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

    enable_testing()
    add_test(NAME USA1to1.unit COMMAND usa1to1_tests)
endif()
