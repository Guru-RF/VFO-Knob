/* CST816 capacitive touch.
 *
 * One 50 Hz sample feeds BOTH the LVGL input device and the PTT gate. Touch is
 * the only non-rotary input on this board -- there is no usable button -- so it
 * carries step selection and, eventually, PTT. That makes its latency a safety
 * figure, not a convenience one, which is why it is polled by a task we own
 * rather than left to LVGL's default 30 ms convenience cadence.
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

esp_lcd_touch_handle_t hal_touch_handle(void);

#endif /* HAL_TOUCH_H */
