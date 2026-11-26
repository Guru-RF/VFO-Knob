/* The TailChaser Tag firmware's radio.h: there is no radio. It answers for a
 * receiver always ready -- nothing to connect to, so nothing to say NO LINK
 * about -- and listens: the knob's own microphone, on for good from the
 * start (audio_in_listen), into the callsign tag decoder (vfo_core tag.c),
 * whose readings make the list of who was heard (tag_hist.c), one entry per
 * over, kept on the SD card. Nothing heard leaves the knob: only the
 * callsigns read are kept, and the levels shown.
 *
 * The decoder is ~0.8 MB, in PSRAM, on a task of its own below the face's
 * (its stack in PSRAM too: it writes no flash, and kvstore writes from its
 * own task). The microphone's ring holds 2 s, so the decoder may fall behind
 * for a moment without a sample lost; a block lost all the same is counted
 * (tag_view_t.lost), as it moves every frame after it. */
#include "radio.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_in.h"
#include "kvstore.h"
#include "tctag_client.h"
#include "tctag_say.h"
#include "wallclock.h"

static const char *TAG = "tctag";

#define HIST_NS      "taghist"
#define HIST_KEY     "list"
#define HIST_CAP     (TAG_HIST_N * 64)        /* a line is at most ~45 characters */
#define SAVE_LIVE_MS 60000                    /* an over that goes on: kept a minute at a time */
#define READ_N       1024                     /* 64 ms at 16 kHz */
#define LOG_EVERY_MS 30000

const char *radio_link_name(void) { return "TAILCHASER TAG"; }

int64_t radio_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz)
{
    (void)detents; (void)accel_mult; (void)step_hz;
    return 0;
}

void radio_get_status(radio_status_t *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
    o->link    = RADIO_LINK_READY;
    o->rx_only = true;
}

bool radio_is_ready(void) { return true; }
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

extern const httpd_uri_t tctag_web_uris[];
extern const size_t      tctag_web_uris_n;

size_t radio_web_endpoints(const httpd_uri_t **out);
size_t radio_web_endpoints(const httpd_uri_t **out)
{
    *out = tctag_web_uris;
    return tctag_web_uris_n;
}

/* ---- the listening ------------------------------------------------------- */

static tag_dec_t        *s_dec;                     /* PSRAM */
EXT_RAM_BSS_ATTR static tag_hist_t s_hist;
static tag_stats_t       s_st;
static SemaphoreHandle_t s_mx;                      /* s_hist, s_st and the counts below */
static uint32_t          s_gen;
static bool              s_listening;
static uint32_t          s_lost;
static uint8_t           s_cpu;
static uint32_t          s_slow_ms;                 /* the longest feed of a chunk, the last 30 s */
static atomic_bool       s_clear;                   /* the page's: empty the list */
static bool              s_started;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* The wall time, 0 before NTP has answered. */
static int64_t unix_now(void)
{
    if (wallclock_age_s() == UINT32_MAX) return 0;
    return (int64_t)time(NULL);
}

static void hist_load(void)
{
    kv_handle_t h;
    if (kv_open(HIST_NS, &h) != ESP_OK) return;
    char *b = heap_caps_malloc(HIST_CAP + 1, MALLOC_CAP_SPIRAM);
    if (!b) return;
    size_t len = HIST_CAP;
    if (kv_get_blob(h, HIST_KEY, b, &len) == ESP_OK) {
        b[len] = 0;
        const int n = tag_hist_load(&s_hist, b);
        ESP_LOGI(TAG, "%d heard before, from the card", n);
    }
    free(b);
}

/* What is in s_hist now, to the card (kvstore writes it, batched). Under s_mx. */
static void hist_save(void)
{
    kv_handle_t h;
    if (kv_open(HIST_NS, &h) != ESP_OK) return;
    EXT_RAM_BSS_ATTR static char b[HIST_CAP];
    const size_t len = tag_hist_save(&s_hist, b, sizeof b);
    if (kv_set_blob(h, HIST_KEY, b, len) == ESP_OK) kv_commit(h);
}

static const char *hist_word(tag_hist_r r)
{
    return r == TAG_HIST_NEW ? "NEW" : r == TAG_HIST_SAME ? "same over" : "waits for a second reading";
}

static void tag_task(void *arg)
{
    (void)arg;
    EXT_RAM_BSS_ATTR static int16_t buf[READ_N];
    int64_t saved_ms = 0, logged_ms = now_ms(), cpu_from = esp_timer_get_time();
    int64_t busy_us = 0, slow_us = 0, try_us = 0;            /* try_us: the longest chunk with a try */
    uint32_t tries = 0;
    bool dirty = false, timed = false, last_waits = true;
    audio_in_stats_t mic0 = { 0 };
    audio_in_stats(&mic0);
    for (;;) {
        const size_t n = audio_in_listen_read(buf, READ_N, 200);
        const int64_t t0 = esp_timer_get_time();
        if (n) tag_dec_feed(s_dec, buf, n);
        const int64_t took = esp_timer_get_time() - t0;
        busy_us += took;
        if (took > slow_us) slow_us = took;
        /* Never the whole core for long: behind, the ring keeps 2 s, and a
         * reading's tries come in bursts -- without a pause between chunks
         * they held core 1 past the task watchdog's 5 s (2026-11-23). */
        vTaskDelay(1);

        const int64_t ms = now_ms(), unix_s = unix_now();
        tag_reading_t rd;
        xSemaphoreTake(s_mx, portMAX_DELAY);
        if (atomic_exchange(&s_clear, false)) {
            tag_hist_init(&s_hist);
            s_gen++;
            dirty = true;
            saved_ms = 0;
            last_waits = true;
        }
        bool fresh = false;
        while (tag_dec_take(s_dec, &rd)) {
            const tag_hist_r r = tag_hist_reading(&s_hist, rd.call, rd.local, rd.sure, ms, unix_s);
            ESP_LOGI(TAG, "%s%s: %s (sync %.1f dB, %.3f; %u fixed, %u frame%s, clock %+d, start %+d)",
                     rd.call, rd.local ? " local" : "", hist_word(r), (double)rd.sync_db, (double)rd.sync_avg,
                     (unsigned)rd.corrected, (unsigned)rd.frames, rd.frames == 1 ? "" : "s", rd.drift, rd.nudge);
            last_waits = r == TAG_HIST_WAIT;
            if (r != TAG_HIST_WAIT) {
                s_gen++;
                dirty = true;
            }
            if (r == TAG_HIST_NEW) {
                fresh = true;
                tctag_say_new(rd.call);                      /* said out loud, when the page asks for it */
            }
        }
        tag_dec_stats(s_dec, &s_st);
        if (s_st.tries != tries && took > try_us) try_us = took;
        tries = s_st.tries;
        /* The over goes on while the start of its last reading stands out --
         * not that of a reading still waiting to be believed. */
        if (s_st.holding && !last_waits) {
            tag_hist_holding(&s_hist, ms, unix_s);
            s_gen++;
        }
        if (unix_s && !timed) {                              /* NTP came: this start's entries get the time */
            timed = true;
            tag_hist_clock(&s_hist, ms, unix_s);
            s_gen++;
            dirty = true;
        }
        const tag_entry_t *e = tag_hist_at(&s_hist, 0);
        const bool over_ended = e && e->now_start && ms - e->last_ms > TAG_HIST_GAP_MS;
        if (dirty && (fresh || over_ended || ms - saved_ms >= SAVE_LIVE_MS)) {
            hist_save();
            saved_ms = ms;
            dirty = false;
        }
        audio_in_stats_t mic;
        audio_in_stats(&mic);
        s_lost = mic.overruns - mic0.overruns;
        const int64_t span = esp_timer_get_time() - cpu_from;
        if (span >= 5000000) {
            s_cpu = (uint8_t)(busy_us * 100 / span);
            busy_us = 0;
            cpu_from = esp_timer_get_time();
        }
        const tag_stats_t st = s_st;
        const uint32_t lost = s_lost;
        const uint8_t cpu = s_cpu;
        xSemaphoreGive(s_mx);

        if (ms - logged_ms >= LOG_EVERY_MS) {
            logged_ms = ms;
            s_slow_ms = (uint32_t)(slow_us / 1000);
            const uint32_t try_ms = (uint32_t)(try_us / 1000);
            slow_us = 0;
            try_us = 0;
            ESP_LOGI(TAG, "band %.0f dBFS (floor %.0f, all %.0f); sync %.1f dB, now %.3f, avg %.3f%s; "
                          "tried %" PRIu32 ", read %" PRIu32 ", CRC bad %" PRIu32 "; cpu %u%%, slowest %" PRIu32
                          " ms (with a try %" PRIu32 " ms), lost %" PRIu32,
                     (double)st.band_dbfs, (double)st.floor_dbfs, (double)st.all_dbfs, (double)st.sync_db,
                     (double)st.sync_now, (double)st.sync_avg, st.holding ? ", holding" : "", st.tries,
                     st.readings, st.crc_bad, (unsigned)cpu, s_slow_ms, try_ms, lost);
        }
    }
}

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    (void)host; (void)port; (void)user; (void)pass;
    if (s_started) return ESP_OK;
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    if (!s_dec) s_dec = heap_caps_malloc(tag_dec_size(), MALLOC_CAP_SPIRAM);
    if (!s_mx || !s_dec) {
        ESP_LOGE(TAG, "no room for the decoder (%u bytes)", (unsigned)tag_dec_size());
        return ESP_ERR_NO_MEM;
    }
    tag_dec_init(s_dec);
    tag_hist_init(&s_hist);
    hist_load();
    tctag_say_start();
    if (audio_in_listen() != ESP_OK) {
        /* Safe mode brings no microphone up: the list from the card alone. */
        ESP_LOGW(TAG, "no microphone: nothing is listened to");
        s_started = true;
        return ESP_OK;
    }
    if (xTaskCreatePinnedToCoreWithCaps(tag_task, "tctag", 12288, NULL, 3, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no room for the decoder's task");
        return ESP_ERR_NO_MEM;
    }
    s_listening = true;
    s_started = true;
    ESP_LOGI(TAG, "listening at %d Hz for TailChaser's callsign tag (format %d, %.2f Hz, %d chips/s); decoder %u bytes",
             TAG_FS, TAG_FORMAT, TAG_FC_HZ, TAG_CHIP_RATE, (unsigned)tag_dec_size());
    return ESP_OK;
}

void tctag_view(tag_view_t *o)
{
    memset(o, 0, sizeof *o);
    o->now_ms = now_ms();
    o->now_unix = unix_now();
    if (!s_mx) {
        tag_hist_init(&o->h);
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    o->st = s_st;
    o->h = s_hist;
    o->gen = s_gen;
    o->listening = s_listening;
    o->lost = s_lost;
    o->cpu = s_cpu;
    xSemaphoreGive(s_mx);
}

void tctag_clear(void)
{
    atomic_store(&s_clear, true);
    if (!s_listening && s_mx) {                              /* no task to do it */
        xSemaphoreTake(s_mx, portMAX_DELAY);
        tag_hist_init(&s_hist);
        s_gen++;
        hist_save();
        xSemaphoreGive(s_mx);
    }
}
