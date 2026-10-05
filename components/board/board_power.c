/* The 5 V rail, read through BATT_ADC (board_pins.h): ADC1 one-shot, 12 dB
 * attenuation -- about 3.1 V at the pin, the rail's 5 V halved being 2.5 --
 * with the chip's curve-fitting calibration, 16 samples averaged. ADC1, not
 * ADC2: the WiFi takes ADC2 while it runs. And from it, a reading a second,
 * the knob's own power: USB, or the battery and its charge (knob_batt.c).
 * Without the calibration no reading at all: the ADC's reference is then
 * anywhere from 1.0 to 1.2 V (ESP-IDF's calibration guide), hundreds of mV
 * at the rail -- more than the line between USB and the battery, or the
 * charge, can take. */
#include "board.h"
#include "board_pins.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <math.h>

static const char *TAG = "board";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static adc_channel_t             s_ch;

/* The gauge, board_power_poll()'s alone; what it shows, for any task, under
 * a spinlock -- four fields, never read half written. */
static knob_batt_t   s_batt;
static int64_t       s_polled_us;
static portMUX_TYPE  s_pw_mux = portMUX_INITIALIZER_UNLOCKED;
static board_power_t s_pw = { .src = KNOB_PWR_UNKNOWN, .pct = -1, .mv = -1, .smooth_mv = -1 };

esp_err_t board_power_init(void)
{
    knob_batt_init(&s_batt);
    adc_unit_t unit;
    esp_err_t  err = adc_oneshot_io_to_channel(BOARD_PIN_BATT_ADC, &unit, &s_ch);
    if (err != ESP_OK) return err;
    const adc_oneshot_unit_init_cfg_t u = { .unit_id = unit };
    adc_oneshot_unit_handle_t         adc;
    if ((err = adc_oneshot_new_unit(&u, &adc)) != ESP_OK) return err;
    const adc_oneshot_chan_cfg_t          c = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    const adc_cali_curve_fitting_config_t k = {
        .unit_id = unit, .chan = s_ch, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if ((err = adc_oneshot_config_channel(adc, s_ch, &c)) == ESP_OK &&
        (err = adc_cali_create_scheme_curve_fitting(&k, &s_cali)) != ESP_OK)
        ESP_LOGW(TAG, "no ADC calibration in this chip: the knob's own power is not read");
    if (err != ESP_OK) {
        adc_oneshot_del_unit(adc);
        return err;
    }
    s_adc = adc;
    return ESP_OK;
}

int board_power_mv(void)
{
    if (!s_adc) return -1;
    int sum = 0, n = 0;
    for (int i = 0; i < 16; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_ch, &raw) != ESP_OK || adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK)
            continue;
        sum += mv;
        n++;
    }
    return n ? 2 * sum / n : -1;                /* the divider halves the rail */
}

bool board_power_poll(void)
{
    const int64_t now = esp_timer_get_time();
    if (!s_adc || (s_polled_us && now - s_polled_us < 1000000)) return false;
    s_polled_us = now;
    const bool          changed = knob_batt_feed(&s_batt, board_power_mv(), (uint32_t)(now / 1000));
    const board_power_t p       = {
        .src       = s_batt.src,
        .pct       = s_batt.pct,
        .mv        = s_batt.mv,
        .smooth_mv = s_batt.src == KNOB_PWR_BATTERY ? (int16_t)lroundf(s_batt.smooth_mv) : -1,
    };
    portENTER_CRITICAL(&s_pw_mux);
    s_pw = p;
    portEXIT_CRITICAL(&s_pw_mux);
    return changed;
}

void board_power_get(board_power_t *out)
{
    portENTER_CRITICAL(&s_pw_mux);
    *out = s_pw;
    portEXIT_CRITICAL(&s_pw_mux);
}
