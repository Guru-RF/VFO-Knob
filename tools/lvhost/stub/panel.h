#ifndef VFO_PANEL_H
#define VFO_PANEL_H
#include <stdint.h>
#define PANEL_CORE 1
typedef void *esp_lcd_panel_handle_t;
typedef void *esp_lcd_panel_io_handle_t;
esp_lcd_panel_handle_t panel_handle(void);
esp_lcd_panel_io_handle_t panel_io_handle(void);
void panel_set_brightness(uint8_t duty);
#endif
