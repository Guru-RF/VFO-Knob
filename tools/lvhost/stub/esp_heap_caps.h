#ifndef ESP_HEAP_CAPS_H
#define ESP_HEAP_CAPS_H
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA      2
#define MALLOC_CAP_SPIRAM   4
#define MALLOC_CAP_8BIT     8
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 100000; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 50000; }
static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
#endif
