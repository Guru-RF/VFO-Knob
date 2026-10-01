/* The knob's second chip: see ../CMakeLists.txt. */
#include <string.h>

#include "bt_link_proto.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hfp.h"
#include "link.h"
#include "nvs_flash.h"

static const char *TAG = "companion";

/* The DAC's soft mute (its XSMT pin) is wired to this chip's IO32: low, and
 * the knob's jack is silent whatever the S3 plays. Held high from the start,
 * as Waveshare's own firmware for this chip held it. */
#define PIN_DAC_XSMT 32

static volatile bool s_knob_heard;

static void hello(bool ask)
{
    const char *v = esp_app_get_description()->version;
    uint8_t p[40];
    p[0] = BTL_PROTO;
    p[1] = ask ? BTL_HELLO_ASK : 0;
    const size_t n = strnlen(v, 32);          /* esp_app_desc_t's version[32] */
    memcpy(p + 2, v, n);
    link_send(BTL_HELLO, p, (uint16_t)(2 + n));
}

static void on_frame(uint8_t type, const uint8_t *p, uint16_t n)
{
    s_knob_heard = true;
    switch (type) {
    case BTL_HELLO:
        /* The knob started: it says so, and where things are is news to it. */
        if (n >= 2 && (p[1] & BTL_HELLO_ASK)) {
            link_log("the knob says hello: protocol %u, %.*s", p[0], n - 2, (const char *)p + 2);
            hello(false);
        }
        hfp_report_state();
        break;
    case BTL_PING:
        link_send(BTL_PONG, p, n);
        break;
    default:
        hfp_on_frame(type, p, n);
        break;
    }
}

void app_main(void)
{
    gpio_reset_pin(PIN_DAC_XSMT);
    gpio_set_direction(PIN_DAC_XSMT, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_DAC_XSMT, 1);

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_LOGI(TAG, "VFO-Knob companion %s", esp_app_get_description()->version);
    link_init(on_frame);
    hfp_init();
    /* Hello until the knob answers -- it may have started first, or later --
     * and the headset's errands meanwhile. */
    int64_t t_hello = 0;
    for (int i = 0;; ) {
        const int64_t now = esp_timer_get_time();
        if (!s_knob_heard && now - t_hello > (i < 10 ? 500000 : 3000000)) {
            t_hello = now;
            i++;
            hello(true);
        }
        hfp_tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
