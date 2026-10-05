/* esp_heap_caps.h for the PC: one heap -- uberhost's, and what the flex
 * client's 10 s report asks of it. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define MALLOC_CAP_8BIT      (1 << 2)
#define MALLOC_CAP_DMA       (1 << 3)
#define MALLOC_CAP_SPIRAM    (1 << 10)
#define MALLOC_CAP_INTERNAL  (1 << 11)
static inline void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t k, size_t n, uint32_t caps) { (void)caps; return calloc(k, n); }
size_t heap_caps_get_minimum_free_size(uint32_t caps);
void heap_caps_monitor_local_minimum_free_size_start(void);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
typedef void (*esp_alloc_failed_hook_t)(size_t size, uint32_t caps, const char *fn);
int heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t cb);
