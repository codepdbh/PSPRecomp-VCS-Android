#pragma once
// PSPRecomp shim for PPSSPP's aligned allocator, used only by mem.cpp.
#include <cstddef>
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>
inline void *AllocateAlignedMemory(size_t size, size_t alignment) { return _aligned_malloc(size ? size : 1, alignment); }
inline void FreeAlignedMemory(void *ptr) { _aligned_free(ptr); }
#else
inline void *AllocateAlignedMemory(size_t size, size_t alignment) {
    void *ptr = nullptr;
    return posix_memalign(&ptr, alignment, size ? size : 1) == 0 ? ptr : nullptr;
}
inline void FreeAlignedMemory(void *ptr) { std::free(ptr); }
#endif
