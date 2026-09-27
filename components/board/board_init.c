#include "board.h"
#include "board_pins.h"

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"

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

    /* Free a hung bus before claiming it.
     *
     * If the chip resets in the middle of a read -- which it does constantly
     * during development, and which the touch poll makes likely at 50 Hz --
     * the slave can be left mid-byte holding SDA low. The controller then sees
     * a permanently busy bus and every probe times out, which looks exactly
     * like dead hardware. Nine clock pulses walk any slave out of its transfer,
     * then a STOP re-synchronises it. Cheap, and it only does anything when
     * the bus is actually stuck. */
    gpio_config_t rec = {
        .pin_bit_mask = (1ULL << BOARD_PIN_I2C_SCL) | (1ULL << BOARD_PIN_I2C_SDA),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rec));
    if (gpio_get_level(BOARD_PIN_I2C_SDA) == 0) {
        ESP_LOGW(TAG, "SDA stuck low -- clocking the bus free");
        for (int i = 0; i < 9; i++) {
            gpio_set_level(BOARD_PIN_I2C_SCL, 0); esp_rom_delay_us(5);
            gpio_set_level(BOARD_PIN_I2C_SCL, 1); esp_rom_delay_us(5);
        }
        /* STOP: SDA low->high while SCL is high. */
        gpio_set_level(BOARD_PIN_I2C_SDA, 0); esp_rom_delay_us(5);
        gpio_set_level(BOARD_PIN_I2C_SCL, 1); esp_rom_delay_us(5);
        gpio_set_level(BOARD_PIN_I2C_SDA, 1); esp_rom_delay_us(5);
        ESP_LOGW(TAG, "recovery done, SDA=%d", gpio_get_level(BOARD_PIN_I2C_SDA));
    }

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
