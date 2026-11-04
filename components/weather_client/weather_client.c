/* The Weather firmware's radio.h: there is no radio, but Open-Meteo's
 * forecast for the page's place. Its task asks every Refresh minutes -- at
 * once when the page moves the place -- reads the answer without a tree
 * (components/vfo_core weather.c) and hands it to the face through
 * app_main. A new connection each time: a quarter of an hour apart, a TLS
 * session kept open would hold internal RAM for nothing. After a failure it
 * asks again sooner, 15 s and doubling, never later than Refresh. Weather
 * data by Open-Meteo.com, CC BY 4.0. */
#include "radio.h"
#include "weather_client.h"
#include "net_prov.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "weather";

#define BODY_MAX    (32 * 1024)        /* a reply is some 2 kB */
#define RETRY_MS    15000
#define FRESH_US    (3LL * 3600 * 1000 * 1000)   /* a forecast older: no forecast */
#define GIVE_UP_US  (60LL * 1000 * 1000)

typedef struct {
    char  *b;
    size_t len;
    bool   over;
} body_t;

static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_snap_mx;
static TaskHandle_t      s_task;

/* The answer read into one, handed over in the other. */
EXT_RAM_BSS_ATTR static wx_t s_parse, s_snap;
static uint32_t s_gen;

static body_t s_body;
static char   s_ua[112];

/* The forecast's state, for radio_get_status and the face's foot. */
static bool    s_ever_ok;
static int64_t s_ok_us, s_fail_us;
static char    s_why[16];

static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA || b->over || e->data_len <= 0) return ESP_OK;
    if (b->len + (size_t)e->data_len + 1 > BODY_MAX) {
        b->over = true;
        return ESP_OK;
    }
    memcpy(b->b + b->len, e->data, (size_t)e->data_len);
    b->len += (size_t)e->data_len;
    b->b[b->len] = 0;
    return ESP_OK;
}

/* One GET into s_body: 200, another status, or -1 with why. */
static int get(const char *url, char *why, size_t wn)
{
    const esp_http_client_config_t hc = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .event_handler     = on_http,
        .user_data         = &s_body,
        .user_agent        = s_ua,
        .buffer_size       = 2048,
        /* The request line alone is some 460 bytes: in the default 512 no
         * header fitted after it, and the client then sent nothing at all
         * and waited out its timeout (the test knob's first run, 2026-11-04). */
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t h = esp_http_client_init(&hc);
    if (!h) {
        snprintf(why, wn, "no memory");
        return -1;
    }
    s_body.len = 0;
    s_body.over = false;
    s_body.b[0] = 0;
    const esp_err_t err = esp_http_client_perform(h);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(h) : -1;
    esp_http_client_cleanup(h);
    if (err != ESP_OK) {
        snprintf(why, wn, "%s", err == ESP_ERR_HTTP_CONNECT ? "no answer"
                              : err == ESP_ERR_HTTP_EAGAIN  ? "timed out" : "cut off");
        ESP_LOGW(TAG, "open-meteo: %s", esp_err_to_name(err));
        return -1;
    }
    if (status != 200) {
        snprintf(why, wn, "HTTP %d", status);
        return status;
    }
    if (s_body.over) {
        snprintf(why, wn, "too much");
        return -1;
    }
    return 200;
}

static void failed(const char *why)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    const bool news = strcmp(s_why, why) != 0;
    if (!s_fail_us) s_fail_us = now;
    strlcpy(s_why, why, sizeof s_why);
    portEXIT_CRITICAL(&s_lock);
    /* Said once, not at each ask. */
    if (news) ESP_LOGW(TAG, "no forecast: %s", why);
}

static void publish(void)
{
    xSemaphoreTake(s_snap_mx, portMAX_DELAY);
    s_snap = s_parse;
    s_gen++;
    xSemaphoreGive(s_snap_mx);
    portENTER_CRITICAL(&s_lock);
    s_ever_ok = true;
    s_ok_us = esp_timer_get_time();
    s_fail_us = 0;
    s_why[0] = 0;
    portEXIT_CRITICAL(&s_lock);
}

static void wx_task(void *arg)
{
    (void)arg;
    int fails = 0;
    for (;;) {
        net_weather_t c;
        net_prov_weather(&c);
        const int64_t refresh_ms = (int64_t)c.refresh_min * 60 * 1000;
        int64_t wait_ms = refresh_ms;
        char url[640], why[16] = "";
        bool ok = false;

        if (!net_prov_is_connected()) {
            snprintf(why, sizeof why, "no WiFi");
        } else if (!wx_url(c.lat_e6 / 1e6, c.lon_e6 / 1e6, url, sizeof url)) {
            snprintf(why, sizeof why, "no address");
        } else {
            const int64_t t0 = esp_timer_get_time();
            if (get(url, why, sizeof why) == 200) {
                if (wx_parse(s_body.b, s_body.len, &s_parse)) {
                    ok = true;
                    publish();
                    ESP_LOGI(TAG, "%s: %.1f C, %s, %d hours, %d days; %u bytes in %d ms; free internal %u",
                             c.place[0] ? c.place : "-", (double)s_parse.now.temp_c, wx_text(s_parse.now.code),
                             s_parse.n_hours, s_parse.n_days, (unsigned)s_body.len,
                             (int)((esp_timer_get_time() - t0) / 1000),
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                } else {
                    ESP_LOGW(TAG, "%u bytes, not a forecast: %.80s", (unsigned)s_body.len, s_body.b);
                    snprintf(why, sizeof why, "not a forecast");
                }
            }
        }
        if (ok) {
            fails = 0;
        } else {
            failed(why);
            const int64_t retry = (int64_t)RETRY_MS << (fails < 6 ? fails : 6);
            wait_ms = retry < refresh_ms ? retry : refresh_ms;
            fails++;
        }
        /* Until the next ask: a poke cuts it short. */
        xTaskNotifyWait(0, 1, NULL, pdMS_TO_TICKS(wait_ms));
    }
}

bool weather_client_take(wx_t *out, uint32_t *gen)
{
    if (!s_snap_mx) return false;
    xSemaphoreTake(s_snap_mx, portMAX_DELAY);
    const bool news = s_gen != *gen;
    if (news) {
        *out = s_snap;
        *gen = s_gen;
    }
    xSemaphoreGive(s_snap_mx);
    return news;
}

void weather_client_poke(void)
{
    if (s_task) xTaskNotify(s_task, 1, eSetBits);
}

void weather_client_state(char *out, size_t n)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(out, s_why, n);
    portEXIT_CRITICAL(&s_lock);
}

int32_t weather_client_age_s(void)
{
    portENTER_CRITICAL(&s_lock);
    const int64_t at = s_ever_ok ? s_ok_us : -1;
    portEXIT_CRITICAL(&s_lock);
    return at < 0 ? -1 : (int32_t)((esp_timer_get_time() - at) / 1000000);
}

extern const httpd_uri_t weather_web_uris[];
extern const size_t      weather_web_uris_n;

size_t radio_web_endpoints(const httpd_uri_t **out);
size_t radio_web_endpoints(const httpd_uri_t **out)
{
    *out = weather_web_uris;
    return weather_web_uris_n;
}

static atomic_bool s_saved;

void weather_client_settings_saved(void)
{
    atomic_store(&s_saved, true);
    weather_client_poke();
}

bool weather_client_take_settings(void) { return atomic_exchange(&s_saved, false); }

/* ---------------------------------------------------------------- radio.h */

const char *radio_link_name(void) { return "OPEN-METEO"; }

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    (void)host; (void)port; (void)user; (void)pass;
    if (s_task) return ESP_OK;
    if (!s_snap_mx && !(s_snap_mx = xSemaphoreCreateMutex())) return ESP_ERR_NO_MEM;
    if (!s_body.b && !(s_body.b = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM))) return ESP_ERR_NO_MEM;
    const char *v = esp_app_get_description()->version;
    snprintf(s_ua, sizeof s_ua, "VFO-Knob-Weather/%s (+github.com/Guru-RF/VFO-Knob)", *v == 'v' ? v + 1 : v);
    /* Its stack in PSRAM: it never writes flash. On core 0, beside WiFi,
     * as ADS-B Radar's: a TLS handshake is seconds of arithmetic, kept off
     * the drawing's core. */
    if (xTaskCreatePinnedToCoreWithCaps(wx_task, "weather", 8192, NULL, 3, &s_task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void radio_get_status(radio_status_t *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
    o->rx_only = true;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (s_ever_ok && now - s_ok_us < FRESH_US) {
        o->link = RADIO_LINK_READY;
    } else if (!s_ever_ok && !(s_fail_us && now - s_fail_us > GIVE_UP_US)) {
        o->link = RADIO_LINK_CONNECTING;
    } else {
        o->link = RADIO_LINK_DOWN;
        strlcpy(o->link_why, "NO FORECAST", sizeof o->link_why);
    }
    portEXIT_CRITICAL(&s_lock);
}

int64_t radio_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz)
{
    (void)detents; (void)accel_mult; (void)step_hz;
    return 0;
}

bool radio_is_ready(void) { return false; }
bool radio_on_air(void)   { return false; }

void radio_set_step(int32_t step_hz)               { (void)step_hz; }
void radio_audio_suspend(bool suspend)             { (void)suspend; }
void radio_ptt_key(void)                           {}
void radio_ptt_unkey(void)                         {}
void radio_ptt_toggle(void)                        {}
void radio_ptt_force_abort(uint8_t reason)         { (void)reason; }
void radio_set_mode(const char *mode)              { (void)mode; }
void radio_set_filter(int32_t lo, int32_t hi)      { (void)lo; (void)hi; }
void radio_select_filter(uint8_t n)                { (void)n; }
void radio_set_rit(int32_t hz)                     { (void)hz; }
void radio_set_agc(const char *agc)                { (void)agc; }
void radio_set_gain(int8_t gain)                   { (void)gain; }
void radio_goto_freq(int64_t hz)                   { (void)hz; }
void radio_memory_mode(bool on)                    { (void)on; }
void radio_memory_group(uint8_t group)             { (void)group; }
void radio_select_rx(uint8_t rx)                   { (void)rx; }
void radio_set_antenna(uint8_t ant, bool rx_ant)   { (void)ant; (void)rx_ant; }
void radio_tune(void)                              {}
void radio_atu_tune(void)                          {}
void radio_atu_memories(bool on)                   { (void)on; }
void radio_set_dsp(uint8_t which, bool on)         { (void)which; (void)on; }
void radio_set_rf_gain(uint8_t pct)                { (void)pct; }
void radio_set_rf_power(uint8_t pct)               { (void)pct; }
void radio_set_tuner(bool on)                      { (void)on; }
void radio_set_squelch(uint8_t pct)                { (void)pct; }
void radio_tg_lock(bool locked)                    { (void)locked; }
void radio_mute(bool muted)                        { (void)muted; }

bool radio_get_choice(uint8_t i, char *title, size_t tn, char *name, size_t nn)
{
    (void)i; (void)title; (void)tn; (void)name; (void)nn;
    return false;
}
void radio_choose(uint8_t i) { (void)i; }
