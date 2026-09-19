// SPDX-License-Identifier: GPL-3.0-or-later
// Logging shim. On Android this is __android_log_print, byte for byte what the
// driver and DSP engine called before they left the app. Anywhere else it goes
// to stderr, so the library builds and its tests run on the host.
//
// Not for the realtime path: both backends can block.

#pragma once

#if defined(__ANDROID__)
#include <android/log.h>
#define TAC_LOG(prio, tag, ...) \
    __android_log_print(ANDROID_LOG_##prio, tag, __VA_ARGS__)
#else
#include <cstdio>
#define TAC_LOG(prio, tag, ...)                          \
    do {                                                 \
        std::fprintf(stderr, "%s/%s: ", #prio, tag);     \
        std::fprintf(stderr, __VA_ARGS__);               \
        std::fputc('\n', stderr);                        \
    } while (0)
#endif
