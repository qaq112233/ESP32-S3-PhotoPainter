#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_INTERNAL = 2, MALLOC_CAP_8BIT = 4;
inline void* heap_caps_malloc(size_t n, int) { return std::malloc(n); }
inline void heap_caps_free(void* p) { std::free(p); }
inline unsigned heap_caps_get_free_size(int) { return 8000000; }
inline unsigned heap_caps_get_largest_free_block(int) { return 8000000; }
