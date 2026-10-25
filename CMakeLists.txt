cmake_minimum_required(VERSION 3.20)
project(stardunk_bridge LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BRIDGE_TRACE "Log every guest import call" OFF)

if(NOT GUEST_SO)
    message(FATAL_ERROR "Pass -DGUEST_SO=<armeabi-v7a libGame.so extracted from your APK> (build.sh does this)")
endif()
configure_file(src/guest_blob.S.in ${CMAKE_CURRENT_BINARY_DIR}/guest_blob.S @ONLY)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/guest_blob.S PROPERTIES OBJECT_DEPENDS ${GUEST_SO})

add_library(Game SHARED
    src/memory.cpp
    src/dlmalloc.c
    src/cpu.cpp
    src/loader.cpp
    src/printf.cpp
    src/hle_libc.cpp
    src/hle_gl.cpp
    src/jni_guest.cpp
    src/exports.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/guest_blob.S
)

target_include_directories(Game PRIVATE
    src
    ${DYNARMIC_ROOT}/src
    ${DYNARMIC_ROOT}/externals/oaknut/include
    ${DYNARMIC_ROOT}/externals/fmt/include
    ${DYNARMIC_ROOT}/externals/mcl/include
)
target_include_directories(Game SYSTEM PRIVATE ${BOOST_ROOT})
target_compile_options(Game PRIVATE -O2 -fvisibility=hidden -Wall -Wno-unused-function)
if(BRIDGE_TRACE)
    target_compile_definitions(Game PRIVATE BRIDGE_TRACE=1)
endif()

set(DYN_BUILD ${DYNARMIC_ROOT}/build-arm64)
target_link_libraries(Game PRIVATE
    ${DYN_BUILD}/src/dynarmic/libdynarmic.a
    ${DYN_BUILD}/externals/fmt/libfmt.a
    ${DYN_BUILD}/externals/mcl/src/libmcl.a
    log GLESv2 android m
)
target_link_options(Game PRIVATE -static-libstdc++ -Wl,--gc-sections -Wl,-z,max-page-size=16384)
