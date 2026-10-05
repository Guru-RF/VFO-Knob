/* The knob's second chip -- a Bluetooth headset's audio gateway, or a
 * speaker's music source: see ../CMakeLists.txt. */
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
#include "upd.h"

static const char *TAG = "companion";

/* The DAC's soft mute (its XSMT pin) is wired to this chip's IO32: low, and
 * the knob's jack is silent whatever the S3 plays. Held high from the start,
 * as Waveshare's own firmware for this chip held it -- and through an
 * update's restart (upd.c): esp_restart() resets the CPUs, not the GPIOs, so
 * the pin left high stays high, provided this start sets the level before it
 * makes the pin an output, and never lets go of it in between. */
#define PIN_DAC_XSMT 32

/* Bluetooth is up: hfp_init() has made the lock, and the state, that the
 * knob's frames reach. */
static volatile bool s_ready;
/* The knob's HELLO came, of either kind: ours, asking, can stop. */
static volatile bool s_knob_heard;

static void hello(bool ask)
{
    const char *v = esp_app_get_description()->version;
    uint8_t p[40];
    p[0] = BTL_PROTO;
    p[1] = (ask ? BTL_HELLO_ASK : 0) | (upd_can_take() ? BTL_HELLO_UPDATE : 0) | BTL_HELLO_SPEAKERS |
           BTL_HELLO_AV_VOLUME;
    const size_t n = strnlen(v, 32);          /* esp_app_desc_t's version[32] */
    memcpy(p + 2, v, n);
    link_send(BTL_HELLO, p, (uint16_t)(2 + n));
}

static void on_frame(uint8_t type, const uint8_t *p, uint16_t n)
{
    /* Bluetooth still coming up. A knob that was up all along goes on as
     * before -- its PING and CMD_STATE every 5 s, the headset's audio if it
     * had it open -- and taken now, its CMD_STATE could reach
     * hfp_report_state()'s lock before hfp_init() has made it (an assert, and
     * a restart), and its commands a stack not there yet. Dropped: our HELLO,
     * asking, goes out as soon as Bluetooth is up (app_main), and the knob
     * answers it with what it wants of us. */
    if (!s_ready) return;
    switch (type) {
    case BTL_HELLO: {
        const bool ask = n >= 2 && (p[1] & BTL_HELLO_ASK);
        /* Only a HELLO says the knob hears us. Any frame once did: restarted
         * under a knob that was up, this chip took its ping or its audio for
         * an answer and never asked -- the knob never learned of the restart,
         * nor said again what it wants, and the headset came back without its
         * audio. */
        s_knob_heard = true;
        /* The knob started: it says so, and where things are is news to it. */
        if (ask) {
            link_log("the knob says hello: protocol %u, %.*s", p[0], n - 2, (const char *)p + 2);
            hello(false);
            hfp_knob_hello();
        }
        /* What it can do, in every hello: a knob that does not say it keeps
         * its own microphone while a speaker plays is given no speaker --
         * every device is a headset to it, as before. */
        if (n >= 2) hfp_knob_flags(p[1]);
        /* This chip's firmware, as news for it too: the boot story, the INFO
         * -- and a knob restarted mid-update has no update going any more. */
        upd_knob_hello(ask);
        hfp_report_state();
        break;
    }
    case BTL_PING:
        link_send(BTL_PONG, p, n);
        break;
    case BTL_UPD_BEGIN:
    case BTL_UPD_DATA:
    case BTL_UPD_END:
    case BTL_UPD_ABORT:
    case BTL_UPD_KEEP:
    case BTL_UPD_ASK:
        upd_on_frame(type, p, n);
        break;
    default:
        hfp_on_frame(type, p, n);
        break;
    }
}

void app_main(void)
{
    gpio_set_level(PIN_DAC_XSMT, 1);
    gpio_config(&(gpio_config_t){ .pin_bit_mask = 1ULL << PIN_DAC_XSMT, .mode = GPIO_MODE_OUTPUT });

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_LOGI(TAG, "VFO-Knob companion %s", esp_app_get_description()->version);
    /* On trial or not, what went back and why: before anything else can
     * hang or crash, and before the knob is told anything. */
    upd_boot();
    link_init(on_frame);
    hfp_init();
    s_ready = true;                           /* the knob's frames, from here on */
    /* Hello until the knob answers -- it may have started first, or later --
     * and the device's errands meanwhile; and a firmware on trial's: the
     * watchdog fed, kept when the knob says so, back if it never does. */
    int64_t t_hello = 0;
    for (int i = 0;; ) {
        const int64_t now = esp_timer_get_time();
        if (!s_knob_heard && now - t_hello > (i < 10 ? 500000 : 3000000)) {
            t_hello = now;
            i++;
            hello(true);
        }
        hfp_tick();
        upd_tick();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
