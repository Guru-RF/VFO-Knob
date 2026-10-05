/* ringbuf.h for the PC: a ring of whole items, under a mutex. */
#pragma once
#include <stddef.h>
#include "freertos/FreeRTOS.h"
typedef struct fh_ring *RingbufHandle_t;
typedef enum { RINGBUF_TYPE_NOSPLIT = 0, RINGBUF_TYPE_ALLOWSPLIT, RINGBUF_TYPE_BYTEBUF } RingbufferType_t;
RingbufHandle_t xRingbufferCreateWithCaps(size_t n, RingbufferType_t t, uint32_t caps);
BaseType_t xRingbufferSend(RingbufHandle_t r, const void *item, size_t n, TickType_t wait);
void *xRingbufferReceive(RingbufHandle_t r, size_t *n, TickType_t wait);
void vRingbufferReturnItem(RingbufHandle_t r, void *item);
