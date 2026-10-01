/* The UART to the knob's ESP32-S3. See bt_link_proto.h for the frames. */
#include "link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bt_link_proto.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "link";

/* The S3's TX (net ESP32S3_TX) comes in on IO18; ours goes out on IO23 (net
 * ESP32S3_RX), the S3's RX. */
#define LINK_UART  UART_NUM_1
#define PIN_TX     23
#define PIN_RX     18

static link_rx_cb_t      s_cb;
static SemaphoreHandle_t s_tx, s_up;
static btl_rx_t          s_rx;

bool link_send(uint8_t type, const void *p, uint16_t n)
{
    static uint8_t f[BTL_MAX_PAYLOAD + 7];
    if (n > BTL_MAX_PAYLOAD || !s_tx) return false;
    xSemaphoreTake(s_tx, portMAX_DELAY);
    const size_t len = btl_frame(f, type, p, n);
    const int w = uart_write_bytes(LINK_UART, f, len);
    xSemaphoreGive(s_tx);
    return w == (int)len;
}

void link_log(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof line) n = sizeof line - 1;
    ESP_LOGI(TAG, "%s", line);
    link_send(BTL_EVT_LOG, line, (uint16_t)n);
}

uint32_t link_bad_frames(void) { return s_rx.bad; }

/* At 2 Mbit/s the receive FIFO's 128 bytes last 0.64 ms. The driver's
 * default asks for them at 120 -- 40 us to spare -- and its interrupt ran on
 * core 0 with the Bluetooth controller's: bytes were lost, and with each
 * byte a whole frame of the knob's audio, which the headset heard as a
 * stutter. So the interrupt is installed from this task, on core 1, in IRAM
 * (CONFIG_UART_ISR_IN_IRAM: a flash write cannot hold it off), and asks at
 * 32 bytes. */
static void install(void)
{
    const uart_config_t c = {
        .baud_rate = BTL_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 8192, 8192, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &c));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_rx_full_threshold(LINK_UART, 32));
    ESP_ERROR_CHECK(uart_set_rx_timeout(LINK_UART, 4));
}

static void rx_task(void *arg)
{
    (void)arg;
    static uint8_t chunk[256];
    install();
    xSemaphoreGive(s_up);
    for (;;) {
        const int n = uart_read_bytes(LINK_UART, chunk, sizeof chunk, pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++)
            if (btl_rx_put(&s_rx, chunk[i]) && s_cb) s_cb(s_rx.type, s_rx.buf, s_rx.len);
    }
}

void link_init(link_rx_cb_t cb)
{
    s_cb = cb;
    s_up = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(rx_task, "link_rx", 4096, NULL, 12, NULL, 1);
    xSemaphoreTake(s_up, portMAX_DELAY);    /* the driver is in */
    s_tx = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "UART%d at %d baud: TX IO%d, RX IO%d", LINK_UART, BTL_BAUD, PIN_TX, PIN_RX);
}
