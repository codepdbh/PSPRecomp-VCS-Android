#pragma once
// PSPRecomp shim for PPSSPP's logging macros, used only by compat.cpp.
#include <cstdio>
#if defined(__ANDROID__)
#include <android/log.h>
#define AT3_LOG(prio, ...) __android_log_print(prio, "VCSAtrac", __VA_ARGS__)
#define DEBUG_LOG(cat, ...) ((void)0)
#define INFO_LOG(cat, ...) AT3_LOG(ANDROID_LOG_INFO, __VA_ARGS__)
#define WARN_LOG(cat, ...) AT3_LOG(ANDROID_LOG_WARN, __VA_ARGS__)
#define ERROR_LOG(cat, ...) AT3_LOG(ANDROID_LOG_ERROR, __VA_ARGS__)
#else
#define DEBUG_LOG(cat, ...) ((void)0)
#define INFO_LOG(cat, ...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#define WARN_LOG(cat, ...) INFO_LOG(cat, __VA_ARGS__)
#define ERROR_LOG(cat, ...) INFO_LOG(cat, __VA_ARGS__)
#endif
