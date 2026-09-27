#include "board.h"
#include "board_pins.h"

#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "board";
static i2c_master_bus_handle_t s_i2c;

esp_err_t board_init(void)
{
    /* Drive the PCM5100A mux select high. v1 does not use the DAC, but
     * leaving this floating is untidy and v2 needs it anyway. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_PIN_AUDIO_MUX_SEL,
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_PIN_AUDIO_MUX_SEL, 1));

    i2c_master_bus_config_t bus = {
        .i2c_port                     = BOARD_I2C_PORT,
        .sda_io_num                   = BOARD_PIN_I2C_SDA,
        .scl_io_num                   = BOARD_PIN_I2C_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        /* The board has external pull-ups; enabling the internal ones too
         * would weaken the rise time at 400 kHz. */
        .flags.enable_internal_pullup = false,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_i2c));
    ESP_LOGI(TAG, "i2c up: sda=%d scl=%d @%d Hz",
             BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL, BOARD_I2C_HZ);
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c(void) { return s_i2c; }
