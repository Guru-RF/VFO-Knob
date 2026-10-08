#ifndef SHIM_ESP_HEAP_CAPS_H
#define SHIM_ESP_HEAP_CAPS_H
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define heap_caps_malloc(n, caps)    malloc(n)
#define heap_caps_calloc(k, n, caps) calloc(k, n)
/* The knob's free internal RAM, for a log line: as much as there is. */
#define heap_caps_get_free_size(caps) ((size_t)128 * 1024)
#endif
