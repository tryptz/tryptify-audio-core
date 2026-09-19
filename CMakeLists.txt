# SPDX-License-Identifier: GPL-3.0-or-later
#
# tryptify-audio-core: the native audio pieces shared by Tryptify and the
# StepMania port. Two static libraries, no JNI:
#
#   tac_usb  libusb UAC1/UAC2 isochronous output driver (vendored libusb by
#            default; set TAC_LIBUSB_TARGET to reuse a consumer's own)
#   tac_dsp  mix-bus DSP engine and its snap-in processors
#
# Consumers add_subdirectory() this and link the targets into their own shared
# library. JNI lives with the consumer, because a JNI symbol name encodes the
# Kotlin/Java class that calls it and that is different in every app.

cmake_minimum_required(VERSION 3.22.1)
project(tryptify_audio_core LANGUAGES C CXX)

# Tests default on only when this is the root project, so a consumer's build
# never compiles them by accident.
option(TAC_BUILD_TESTS "Build host tests" ${PROJECT_IS_TOP_LEVEL})
option(TAC_BUILD_USB   "Build the libusb output driver" ON)

# A consumer that already vendors libusb must not get a second copy. Two
# static libusb archives in one binary means duplicate symbols at link, or —
# if the linker resolves it — two libusb contexts at runtime, each with its
# own event thread and device list. ITGmania is exactly this case: it vendors
# libusb-cmake for its arcade I/O boards. Point TAC_LIBUSB_TARGET at the
# consumer's target and the vendored build is skipped.
set(TAC_LIBUSB_TARGET "" CACHE STRING
    "CMake target providing libusb-1.0. Empty builds the vendored copy.")

set(TAC_ARM64_SIMD $<$<STREQUAL:${ANDROID_ABI},arm64-v8a>:-march=armv8-a+simd>)

# ── libusb ───────────────────────────────────────────────────────────────
# Upstream builds with autotools + Android.mk. Listing the Linux backend
# sources here lets one CMake toolchain cover every ABI. android/config.h is
# upstream's pre-generated autotools config for bionic; it also holds on
# glibc hosts, which is what lets the host test build link the driver.
if(TAC_BUILD_USB AND TAC_LIBUSB_TARGET)
    if(NOT TARGET ${TAC_LIBUSB_TARGET})
        message(FATAL_ERROR
            "TAC_LIBUSB_TARGET is set to '${TAC_LIBUSB_TARGET}', which is not "
            "a CMake target. Define it before add_subdirectory() of this "
            "project, or clear it to build the vendored libusb.")
    endif()
    message(STATUS "tac_usb: using consumer libusb target ${TAC_LIBUSB_TARGET}")
    add_library(tac_libusb INTERFACE)
    target_link_libraries(tac_libusb INTERFACE ${TAC_LIBUSB_TARGET})
elseif(TAC_BUILD_USB)
    set(TAC_LIBUSB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb)
    if(NOT EXISTS ${TAC_LIBUSB_DIR}/libusb/core.c)
        message(FATAL_ERROR
            "libusb submodule missing: git submodule update --init --recursive")
    endif()
    add_library(usb1.0 STATIC
        ${TAC_LIBUSB_DIR}/libusb/core.c
        ${TAC_LIBUSB_DIR}/libusb/descriptor.c
        ${TAC_LIBUSB_DIR}/libusb/hotplug.c
        ${TAC_LIBUSB_DIR}/libusb/io.c
        ${TAC_LIBUSB_DIR}/libusb/sync.c
        ${TAC_LIBUSB_DIR}/libusb/strerror.c
        ${TAC_LIBUSB_DIR}/libusb/os/linux_usbfs.c
        ${TAC_LIBUSB_DIR}/libusb/os/events_posix.c
        ${TAC_LIBUSB_DIR}/libusb/os/threads_posix.c
        ${TAC_LIBUSB_DIR}/libusb/os/linux_netlink.c
    )
    target_include_directories(usb1.0
        PUBLIC  ${TAC_LIBUSB_DIR}/libusb
        PRIVATE ${TAC_LIBUSB_DIR}/libusb/os
                ${TAC_LIBUSB_DIR}/android)
    target_compile_options(usb1.0 PRIVATE
        -fvisibility=hidden -Wno-unused-parameter -Wno-unused-but-set-variable)
    set_target_properties(usb1.0 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    find_package(Threads REQUIRED)
    target_link_libraries(usb1.0 PUBLIC Threads::Threads)
    add_library(tac_libusb INTERFACE)
    target_link_libraries(tac_libusb INTERFACE usb1.0)
endif()

if(TAC_BUILD_USB)
    add_library(tac_usb STATIC usb/libusb_uac_driver.cpp)
    target_include_directories(tac_usb PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/usb
        ${CMAKE_CURRENT_SOURCE_DIR}/common)
    target_compile_features(tac_usb PUBLIC cxx_std_17)
    target_compile_options(tac_usb PRIVATE -O3 -fno-exceptions -fno-rtti)
    # Static archives get linked into a consumer's .so, so the code inside
    # them must be position independent or the final link fails on arm64.
    set_target_properties(tac_usb PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(tac_usb PUBLIC tac_libusb)
    if(ANDROID)
        target_link_libraries(tac_usb PUBLIC log)
    endif()
endif()

# ── DSP engine ───────────────────────────────────────────────────────────
add_library(tac_dsp STATIC dsp/dsp_engine.cpp)
target_include_directories(tac_dsp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/dsp
    ${CMAKE_CURRENT_SOURCE_DIR}/dsp/util
    ${CMAKE_CURRENT_SOURCE_DIR}/dsp/snapins
    ${CMAKE_CURRENT_SOURCE_DIR}/dsp/oxford
    ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_features(tac_dsp PUBLIC cxx_std_17)
# Same flags the engine shipped with inside Tryptify. They stay PRIVATE: a
# consumer that includes the snap-in headers compiles them with its own flags,
# and it must choose to match these (Tryptify does) rather than inherit them.
target_compile_options(tac_dsp PRIVATE
    -O3 -ffast-math -fno-exceptions -fno-rtti ${TAC_ARM64_SIMD})
set_target_properties(tac_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(ANDROID)
    target_link_libraries(tac_dsp PUBLIC log)
endif()

# ── Host tests ───────────────────────────────────────────────────────────
if(TAC_BUILD_TESTS AND NOT ANDROID)
    enable_testing()

    add_executable(wsola_pitch_test dsp/tests/wsola_pitch_test.cpp)
    target_compile_options(wsola_pitch_test PRIVATE -O2)
    add_test(NAME wsola_pitch COMMAND wsola_pitch_test)

    add_executable(dsp_engine_test tests/dsp_engine_test.cpp)
    target_link_libraries(dsp_engine_test PRIVATE tac_dsp)
    add_test(NAME dsp_engine COMMAND dsp_engine_test)

    if(TAC_BUILD_USB)
        add_executable(usb_link_test tests/usb_link_test.cpp)
        target_link_libraries(usb_link_test PRIVATE tac_usb)
        add_test(NAME usb_link COMMAND usb_link_test)
    endif()
endif()
