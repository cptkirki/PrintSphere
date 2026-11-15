#pragma once
#include <cstddef>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_SPIRAM 4
inline size_t heap_caps_get_allocated_size(void*) { return 0; }
inline size_t heap_caps_get_free_size(int) { return 10000000; }
inline size_t heap_caps_get_largest_free_block(int) { return 10000000; }
