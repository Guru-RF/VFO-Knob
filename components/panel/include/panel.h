/* SH8601 QSPI panel, 360x360 RGB565, plus the LEDC backlight.
 *
 * Driver and init sequence taken from Waveshare's own 08_LVGL_Test demo, which
 * is the only authority for this board: the controller is an SH8601 but the
 * panel also has a real PWM backlight on GPIO47, a combination that several
 * third-party sources get wrong in one direction or the other.
 */
#ifndef VFO_PANEL_H
#define VFO_PANEL_H

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/* The core that drives the display. The SPI interrupt is allocated on it, and
 * whatever queues pixels -- the LVGL task -- must run on it too. IDF's SPI bus
 * lock hands the bus between that interrupt and the task under a critical
 * section, which masks the interrupt on the task's own core and does nothing
 * about the other one. With the interrupt on core 0 and LVGL on core 1 the
 * interrupt read back a bus owner the task had just cleared, and panicked in
 * spi_bus_lock_bg_exit after four and a half hours of plain receive. */
#define PANEL_CORE 1

esp_err_t panel_init(void);

/* Handle for LVGL to flush into. */
esp_lcd_panel_handle_t panel_handle(void);

/* The panel IO handle. esp_lvgl_port needs it to hook the transfer-done
 * callback, which is how it knows a flush has completed. */
esp_lcd_panel_io_handle_t panel_io_handle(void);

/* 0..255. LEDC 8-bit at 50 kHz on GPIO47, matching the vendor demo. */
void panel_set_brightness(uint8_t duty);

/* M4 acceptance test, drawn directly with no LVGL in the way:
 *   - solid red / green / blue  -> wrong order means rgb_ele_order is wrong
 *   - a 1 px white circle at r=179 -> must touch all four screen extremes,
 *     which is how a panel gap offset shows itself in a single flash
 *   - a backlight ramp          -> proves the PWM path
 */
esp_err_t panel_selftest(void);

#endif /* VFO_PANEL_H */
