/* A PNG -- 8 bits a channel, grey, RGB, palette, with or without alpha, not
 * interlaced, which is what UberSDR's SSTV gallery serves -- decoded into
 * RGB565 and fitted into box_w x box_h, nearest neighbour, with the ROM's own
 * inflate. The PNG's buffer is overwritten (its image data is gathered in
 * place). */
#ifndef UBER_PNG_H
#define UBER_PNG_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t upng_decode(uint8_t *png, size_t n, uint16_t *out, int box_w, int box_h,
                      int *ow, int *oh, char *why, size_t wn);

#endif /* UBER_PNG_H */
