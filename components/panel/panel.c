#include "panel.h"
#include "board_pins.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "panel";

extern const sh8601_lcd_init_cmd_t vfo_sh8601_init_cmds[];
extern const size_t                vfo_sh8601_init_cmds_len;

#define LCD_BPP        16
#define BL_TIMER       LEDC_TIMER_3
#define BL_CHANNEL     LEDC_CHANNEL_1
#define BL_MODE        LEDC_LOW_SPEED_MODE

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;

static void backlight_init(void)
{
    /* 50 kHz, 8-bit, low-speed mode -- same as the vendor's lcd_bl_pwm_bsp. */
    ledc_timer_config_t t = {
        .speed_mode      = BL_MODE,
        .timer_num       = BL_TIMER,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz         = 50 * 1000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {
        .gpio_num   = BOARD_PIN_LCD_BL,
        .speed_mode = BL_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void panel_set_brightness(uint8_t duty)
{
    ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
}

esp_err_t panel_init(void)
{
    backlight_init();

    const spi_bus_config_t bus = {
        .sclk_io_num     = BOARD_PIN_LCD_PCLK,
        .data0_io_num    = BOARD_PIN_LCD_DATA0,
        .data1_io_num    = BOARD_PIN_LCD_DATA1,
        .data2_io_num    = BOARD_PIN_LCD_DATA2,
        .data3_io_num    = BOARD_PIN_LCD_DATA3,
        /* One full frame is the worst case a flush can ask for. */
        .max_transfer_sz = BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t),
        /* Beside the LVGL task, not wherever this happens to run: see
         * PANEL_CORE. */
        .isr_cpu_id      = PANEL_CORE == 0 ? ESP_INTR_CPU_AFFINITY_0
                                           : ESP_INTR_CPU_AFFINITY_1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus,
                                           SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg =
        SH8601_PANEL_IO_QSPI_CONFIG(BOARD_PIN_LCD_CS, NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &io),
        TAG, "panel io");
    s_io = io;

    sh8601_vendor_config_t vendor = {
        .init_cmds      = vfo_sh8601_init_cmds,
        .init_cmds_size = vfo_sh8601_init_cmds_len,
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BPP,
        .vendor_config  = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(io, &dev, &s_panel),
                        TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel),  TAG, "init");

    /* Turn the backlight ON. LEDC is configured with duty 0, and until the
     * UI existed the only thing that ever raised it was the self-test -- so
     * disabling the self-test turned the screen black while everything behind
     * it carried on working perfectly. */
    panel_set_brightness(200);

    ESP_LOGI(TAG, "SH8601 up: %dx%d RGB565, QSPI on host %d, %u init cmds",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, BOARD_LCD_SPI_HOST,
             (unsigned)vfo_sh8601_init_cmds_len);
    return ESP_OK;
}

esp_lcd_panel_handle_t panel_handle(void) { return s_panel; }
esp_lcd_panel_io_handle_t panel_io_handle(void) { return s_io; }

/* --- M4 self-test --------------------------------------------------------
 * Drawn a row at a time: a full RGB565 frame is 259 kB, far more than internal
 * DMA memory can hold, and the real UI will never draw one either. */

#define ROW_BYTES (BOARD_LCD_H_RES * (int)sizeof(uint16_t))

static esp_err_t fill(uint16_t *row, uint16_t colour)
{
    for (int x = 0; x < BOARD_LCD_H_RES; x++) row[x] = colour;
    for (int y = 0; y < BOARD_LCD_V_RES; y++)
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(
            s_panel, 0, y, BOARD_LCD_H_RES, y + 1, row), TAG, "blit");
    return ESP_OK;
}

esp_err_t panel_selftest(void)
{
    uint16_t *row = heap_caps_malloc(ROW_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(row, ESP_ERR_NO_MEM, TAG, "row buffer");

    panel_set_brightness(255);

    /* RGB565, byte-swapped on the wire by the driver. If these come out in the
     * wrong order, rgb_ele_order is wrong. */
    const struct { uint16_t c; const char *name; } STEPS[] = {
        { 0xF800, "RED" }, { 0x07E0, "GREEN" }, { 0x001F, "BLUE" },
        { 0xFFFF, "WHITE" }, { 0x0000, "BLACK" },
    };
    for (size_t i = 0; i < sizeof STEPS / sizeof STEPS[0]; i++) {
        ESP_LOGI(TAG, "  [M4] full screen %s", STEPS[i].name);
        ESP_RETURN_ON_ERROR(fill(row, STEPS[i].c), TAG, "fill");
        vTaskDelay(pdMS_TO_TICKS(1400));
    }

    /* Geometry probe. CONFIRMED on hardware: the ring touches all four
     * extremes, so no esp_lcd_panel_set_gap() offset is required and the full
     * 360x360 is addressable with centre (180,180). */
    ESP_LOGI(TAG, "  [M4] 1 px ring at r=179 + centre crosshair");
    const int cx = BOARD_LCD_H_RES / 2, cy = BOARD_LCD_V_RES / 2, r = 179;
    for (int y = 0; y < BOARD_LCD_V_RES; y++) {
        for (int x = 0; x < BOARD_LCD_H_RES; x++) {
            int dx = x - cx, dy = y - cy;
            int d2 = dx * dx + dy * dy;
            bool ring  = d2 <= r * r && d2 > (r - 1) * (r - 1);
            bool cross = (x == cx || y == cy) && (dx * dx + dy * dy) < 400;
            row[x] = ring ? 0xFFFF : cross ? 0xF81F : 0x0000;
        }
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(
            s_panel, 0, y, BOARD_LCD_H_RES, y + 1, row), TAG, "blit");
    }
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* Backlight ramp: proves the LEDC path independently of the panel. */
    /* Ramp against WHITE so a brightness change is unmissable. */
    ESP_LOGI(TAG, "  [M4] backlight ramp (watch the screen dim and come back)");
    ESP_RETURN_ON_ERROR(fill(row, 0xFFFF), TAG, "fill");
    for (int rep = 0; rep < 2; rep++) {
        for (int d = 255; d >= 0; d -= 4) { panel_set_brightness((uint8_t)d); vTaskDelay(pdMS_TO_TICKS(10)); }
        for (int d = 0; d <= 255; d += 4) { panel_set_brightness((uint8_t)d); vTaskDelay(pdMS_TO_TICKS(10)); }
    }
    panel_set_brightness(255);

    heap_caps_free(row);
    ESP_LOGI(TAG, "  [M4] self-test done");
    return ESP_OK;
}
