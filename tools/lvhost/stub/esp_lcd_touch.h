/* esp_lcd_touch's names ui.c meets: the touch is the harness's (slab.c). */
#ifndef ESP_LCD_TOUCH_H
#define ESP_LCD_TOUCH_H
#include <stdint.h>
#include "esp_err.h"
#define CONFIG_ESP_LCD_TOUCH_MAX_POINTS 1
typedef struct esp_lcd_touch_s *esp_lcd_touch_handle_t;
typedef struct { uint8_t track_id; uint16_t x, y, strength; } esp_lcd_touch_point_data_t;
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp);
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *pts,
                                 uint8_t *n, uint8_t max);
#endif
