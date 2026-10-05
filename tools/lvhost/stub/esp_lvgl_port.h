/* esp_lvgl_port's names ui.c meets: on the host there is one task, the
 * harness's, so the lock is always had; the display and the touch are the
 * harness's own (slab.c). */
#ifndef ESP_LVGL_PORT_H
#define ESP_LVGL_PORT_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "panel.h"
#include "esp_lcd_touch.h"
typedef struct { int task_affinity; } lvgl_port_cfg_t;
#define ESP_LVGL_PORT_INIT_CONFIG() { .task_affinity = -1 }
typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
    uint32_t buffer_size;
    bool double_buffer;
    uint32_t hres, vres;
    struct { unsigned buff_dma : 1, sw_rotate : 1, swap_bytes : 1; } flags;
} lvgl_port_display_cfg_t;
typedef struct { lv_display_t *disp; esp_lcd_touch_handle_t handle; } lvgl_port_touch_cfg_t;
esp_err_t     lvgl_port_init(const lvgl_port_cfg_t *cfg);
lv_display_t *lvgl_port_add_disp(const lvgl_port_display_cfg_t *cfg);
lv_indev_t   *lvgl_port_add_touch(const lvgl_port_touch_cfg_t *cfg);
static inline bool lvgl_port_lock(uint32_t ms) { (void)ms; return true; }
static inline void lvgl_port_unlock(void) {}
/* FreeRTOS's critical sections, which come with it on the knob: the
 * telephone's keypad takes one. One task here, nothing to take. */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m)  ((void)(m))
#endif
