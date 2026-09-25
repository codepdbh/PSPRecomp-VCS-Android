#pragma once
// PSPRecomp shim: the only part of PPSSPP's ppsspp_config.h that
// atrac3plusdsp.cpp needs, which picks its SIMD path per architecture.
#define PPSSPP_ARCH(FEATURE) (PPSSPP_ARCH_##FEATURE)
#if defined(__aarch64__) || defined(_M_ARM64)
#define PPSSPP_ARCH_ARM64 1
#define PPSSPP_ARCH_ARM_NEON 1
#elif defined(__arm__) && defined(__ARM_NEON)
#define PPSSPP_ARCH_ARM_NEON 1
#elif defined(__x86_64__) || defined(_M_X64)
#define PPSSPP_ARCH_AMD64 1
#define PPSSPP_ARCH_SSE2 1
#elif defined(__i386__) || defined(_M_IX86)
#define PPSSPP_ARCH_X86 1
#define PPSSPP_ARCH_SSE2 1
#endif
#ifndef PPSSPP_ARCH_ARM64
#define PPSSPP_ARCH_ARM64 0
#endif
#ifndef PPSSPP_ARCH_ARM_NEON
#define PPSSPP_ARCH_ARM_NEON 0
#endif
#ifndef PPSSPP_ARCH_AMD64
#define PPSSPP_ARCH_AMD64 0
#endif
#ifndef PPSSPP_ARCH_X86
#define PPSSPP_ARCH_X86 0
#endif
#ifndef PPSSPP_ARCH_SSE2
#define PPSSPP_ARCH_SSE2 0
#endif
