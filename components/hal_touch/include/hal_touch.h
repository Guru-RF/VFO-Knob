/* CST816 capacitive touch.
 *
 * Touch is the only non-rotary input on this board -- there is no usable
 * button -- so it carries step selection and PTT. That makes its latency a
 * safety figure, not a convenience one: LVGL's input device, which drives
 * every control on the face, reads the controller every TOUCH_POLL_MS rather
 * than at its default cadence.
 *
 * The task here samples it too, but only until boot has checked for a finger
 * held down (the USB-networking escape); hal_touch_stop() then ends it. Two
 * readers of one controller steal each other's samples, because reading a
 * point clears it.
 */
#ifndef HAL_TOUCH_H
#define HAL_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_touch.h"

/* 10 ms, not 20. At 20 ms a brisk tap could fall between samples and simply
 * not register, which reads as "you have to press firmly". PTT release latency
 * is also a safety figure, so this is not a place to economise. */
#define TOUCH_POLL_MS 10

typedef struct {
    bool     pressed;
    uint16_t x, y;
    uint32_t presses;      /* completed press-and-release cycles */
    uint32_t i2c_errors;
} touch_sample_t;

esp_err_t hal_touch_init(void);

/* Latest sample. Safe to call from any task. */
void hal_touch_get(touch_sample_t *out);

/* End the boot-time sampler, leaving LVGL the controller's only reader.
 * hal_touch_get() keeps returning the last sample. */
void hal_touch_stop(void);

esp_lcd_touch_handle_t hal_touch_handle(void);

#endif /* HAL_TOUCH_H */
