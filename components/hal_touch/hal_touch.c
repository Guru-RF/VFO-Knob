#include "hal_touch.h"
#include "board.h"
#include "board_pins.h"

#include "esp_check.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

static esp_lcd_touch_handle_t s_tp;
static touch_sample_t         s_sample;
static portMUX_TYPE           s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool          s_stop;

static void touch_task(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    bool was_pressed = false;

    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(TOUCH_POLL_MS));
        if (s_stop) break;

        uint16_t x = 0, y = 0;
        uint8_t  cnt = 0;
        bool ok = false;

        if (esp_lcd_touch_read_data(s_tp) == ESP_OK)
            ok = esp_lcd_touch_get_coordinates(s_tp, &x, &y, NULL, &cnt, 1);

        taskENTER_CRITICAL(&s_lock);
        if (!ok || cnt == 0) {
            if (was_pressed) s_sample.presses++;   /* count on release */
            s_sample.pressed = false;
            was_pressed = false;
        } else {
            s_sample.pressed = true;
            s_sample.x = x;
            s_sample.y = y;
            was_pressed = true;
        }
        taskEXIT_CRITICAL(&s_lock);
    }
    vTaskDelete(NULL);
}

void hal_touch_stop(void) { s_stop = true; }

esp_err_t hal_touch_init(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    /* The CST816 is happier slow, and it shares the bus with the haptic driver
     * which runs at 400 kHz; the new i2c_master driver allows per-device rates. */
    io_cfg.scl_speed_hz = 100000;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(board_i2c(), &io_cfg, &io),
                        TAG, "panel io");

    esp_lcd_touch_config_t cfg = {
        .x_max         = BOARD_LCD_H_RES,
        .y_max         = BOARD_LCD_V_RES,
        .rst_gpio_num  = BOARD_PIN_TOUCH_RST,
        .int_gpio_num  = BOARD_PIN_TOUCH_INT,
        .levels        = { .reset = 0, .interrupt = 0 },
        /* No mirroring here: esp_lvgl_port applies the display rotation to the
         * touch stream as well, so doing it twice would cancel out. */
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(io, &cfg, &s_tp),
                        TAG, "cst816s");

    /* Keep the controller out of auto-sleep: register 0xFE, non-zero disables
     * it. After a couple of seconds untouched it drops to a slow low-power
     * scan and wakes only on a firmer contact -- and the first touch after
     * tuning with the knob is usually PTT. The chip has just been reset and
     * answered its ID read, so it is awake to take this. USB-powered, so the
     * power the sleep would save does not matter. */
    const uint8_t no_sleep = 1;
    if (esp_lcd_panel_io_tx_param(io, 0xFE, &no_sleep, 1) != ESP_OK)
        ESP_LOGW(TAG, "could not disable auto-sleep");

    /* And out of its two self-resets, 0 disabling each: 0xFB resets the chip
     * after a spell of touch with no gesture it knows, 0xFC after a long
     * press -- 5 s and 10 s by the family's datasheet, either of which would
     * end a finger held still, as the holds that bring up the address card
     * and the firmware picker are. This board's CST816D (id 182) reads 0 for both
     * already; they are set anyway, and what was there logged, in case
     * another batch of the chip starts with the datasheet's values. */
    const uint8_t off = 0;
    uint8_t was[2] = { 0xFF, 0xFF }, now[2] = { 0xFF, 0xFF };
    for (int i = 0; i < 2; i++) {
        const int reg = 0xFB + i;
        esp_lcd_panel_io_rx_param(io, reg, &was[i], 1);
        esp_lcd_panel_io_tx_param(io, reg, &off, 1);
        esp_lcd_panel_io_rx_param(io, reg, &now[i], 1);
    }
    ESP_LOGI(TAG, "self-reset after touch with no gesture: %u s -> %u, "
                  "after a long press: %u s -> %u (0 = never)",
             was[0], now[0], was[1], now[1]);

    xTaskCreatePinnedToCore(touch_task, "touch", 4096, NULL, 10, NULL, 1);
    ESP_LOGI(TAG, "CST816 up at 0x%02X, polled every %d ms",
             BOARD_I2C_ADDR_TOUCH, TOUCH_POLL_MS);
    return ESP_OK;
}

void hal_touch_get(touch_sample_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_lock);
    *out = s_sample;
    taskEXIT_CRITICAL(&s_lock);
}

esp_lcd_touch_handle_t hal_touch_handle(void) { return s_tp; }
