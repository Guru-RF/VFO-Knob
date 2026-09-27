#include "hal_encoder.h"
#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "enc";

typedef struct {
    uint8_t  low_ticks;   /* consecutive polls seen low */
    uint8_t  last;
    uint32_t raw_edges;
    uint32_t accepted;
    uint32_t rejected;
} chan_t;

static chan_t   s_ch[2];       /* 0 = A / right, 1 = B / left */
static int32_t  s_count;
static int32_t  s_last_read;
static int8_t   s_last_dir;    /* +1 / -1 / 0 */
static uint16_t s_since_accept;
static uint32_t s_reversals;

static const gpio_num_t PIN[2] = { BOARD_PIN_ENC_A, BOARD_PIN_ENC_B };

esp_err_t hal_encoder_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_PIN_ENC_A) | (1ULL << BOARD_PIN_ENC_B),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* contacts close to ground */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,    /* polled -- see the header */
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");

    for (int i = 0; i < 2; i++) {
        s_ch[i].last      = (uint8_t)gpio_get_level(PIN[i]);
        s_ch[i].low_ticks = 0;
    }
    ESP_LOGI(TAG, "switch knob: A(right)=%d B(left)=%d, poll %d ms, "
                  "min contact %d ms",
             BOARD_PIN_ENC_A, BOARD_PIN_ENC_B,
             ENC_POLL_MS, ENC_MIN_LOW_TICKS * ENC_POLL_MS);
    return ESP_OK;
}

int32_t hal_encoder_poll(void)
{
    int32_t delta = 0;
    if (s_since_accept < 0xFFFF) s_since_accept++;

    for (int i = 0; i < 2; i++) {
        chan_t *c = &s_ch[i];
        uint8_t lvl = (uint8_t)gpio_get_level(PIN[i]);
        if (lvl != c->last) c->raw_edges++;

        if (lvl == 0) {
            /* Contact closed. Only DURATION matters here -- this is what an
             * edge-triggered lockout cannot do. */
            if (c->low_ticks < 255) c->low_ticks++;
        } else {
            if (c->low_ticks >= ENC_MIN_LOW_TICKS) {
                /* Fire on release: a detent is one complete close-and-open,
                 * not an edge. */
                int8_t dir = (i == 0) ? 1 : -1;
                if (s_last_dir != 0 && dir != s_last_dir &&
                    s_since_accept < ENC_DIR_LOCKOUT_TICKS) {
                    /* Opposite contact too soon after the last detent: the
                     * knob did not physically reverse that fast. */
                    s_reversals++;
                } else {
                    delta += dir;
                    c->accepted++;
                    s_last_dir     = dir;
                    s_since_accept = 0;
                }
            } else if (c->low_ticks > 0) {
                c->rejected++;
            }
            c->low_ticks = 0;
        }
        c->last = lvl;
    }

    s_count += delta;
    return delta;
}

int32_t hal_encoder_count(void) { return s_count; }

int32_t hal_encoder_read_delta(void)
{
    int32_t d = s_count - s_last_read;
    s_last_read = s_count;
    return d;
}

void hal_encoder_stats(enc_stats_t *st)
{
    if (!st) return;
    st->raw_a      = s_ch[0].raw_edges;
    st->raw_b      = s_ch[1].raw_edges;
    st->accepted_a = s_ch[0].accepted;
    st->accepted_b = s_ch[1].accepted;
    st->rejected   = s_ch[0].rejected + s_ch[1].rejected;
    st->reversals  = s_reversals;
}
