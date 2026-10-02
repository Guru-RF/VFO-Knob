/* The UART to the knob's ESP32-S3: frames as bt_link_proto.h has them. */
#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stdint.h>

/* Called on the link's task with each whole frame. */
typedef void (*link_rx_cb_t)(uint8_t type, const uint8_t *p, uint16_t n);

void link_init(link_rx_cb_t cb);
bool link_send(uint8_t type, const void *p, uint16_t n);
/* A line for the knob's log, and this chip's console. */
void link_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t link_bad_frames(void);
/* Waits (up to 100 ms) until all that was sent has left: before a restart. */
void link_flush(void);

#endif /* LINK_H */
