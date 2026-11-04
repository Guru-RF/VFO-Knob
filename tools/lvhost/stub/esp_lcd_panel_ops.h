/* esp_lcd's panel calls the screensaver makes: on the host the harness's
 * frame takes the pixels (slab.c), as the glass would. */
#ifndef ESP_LCD_PANEL_OPS_H
#define ESP_LCD_PANEL_OPS_H
#include <stdbool.h>
#include "esp_err.h"
#include "panel.h"
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start, int x_end, int y_end,
                                    const void *color_data);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on_off);
#endif
