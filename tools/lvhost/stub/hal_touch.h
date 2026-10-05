#ifndef HAL_TOUCH_H
#define HAL_TOUCH_H
#include "esp_lcd_touch.h"
#define TOUCH_POLL_MS 10
esp_lcd_touch_handle_t hal_touch_handle(void);
#endif
