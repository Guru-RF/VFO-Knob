/* The Clock firmware's radio.h: there is no radio. It answers for a receiver
 * always ready -- there is nothing to connect to, so nothing to say NO LINK
 * or TX DISABLED about -- taking no settings, and the knob's face is the
 * clock (ui.c CLOCK_FACE). Whether the time is known yet is the
 * face's to say: "--:--" until SNTP has answered (components/wallclock). */
#include "radio.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_http_server.h"

#include "audio_out.h"
#include "bt_link.h"
#include "clock_client.h"
#include "net_prov.h"

const char *radio_link_name(void) { return "CLOCK"; }

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    (void)host; (void)port; (void)user; (void)pass;
    return ESP_OK;
}

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

/* ---- the alarms' page, and their beeps ---------------------------------- */

extern const httpd_uri_t clock_web_uris[];
extern const size_t      clock_web_uris_n;

size_t radio_web_endpoints(const httpd_uri_t **out);
size_t radio_web_endpoints(const httpd_uri_t **out)
{
    *out = clock_web_uris;
    return clock_web_uris_n;
}

static atomic_bool s_saved;

/* clock_web.c's, once the page's alarms are kept. */
void clock_client_alarms_saved(void);
void clock_client_alarms_saved(void) { atomic_store(&s_saved, true); }

bool clock_client_take_alarms(void) { return atomic_exchange(&s_saved, false); }

/* An alarm clock's: four beeps at 1 kHz, 80 ms each and 80 between, then
 * quiet to the end of 1.2 s, round again. They grow louder, from a third of
 * the page's volume to all of it over the first 30 s, so a sleeper is woken,
 * not thrown out of bed. Each beep comes in and goes out over 4 ms: no
 * clicks. */
#define BEEP_HZ      1000
#define BEEP_N       (AUDIO_RATE_HZ * 80 / 1000)
#define BEEP_CYCLE   (AUDIO_RATE_HZ * 1200 / 1000)
#define BEEP_RAMP    (AUDIO_RATE_HZ * 4 / 1000)
#define BEEP_RISE    (AUDIO_RATE_HZ * 30)
#define SOUND_FRAME  (AUDIO_RATE_HZ / 100)       /* 10 ms */
#define SOUND_AHEAD  (AUDIO_RATE_HZ / 7)         /* ~150 ms past the pre-roll: app_main's loop is 50 ms */

static int16_t beep_sample(uint32_t n)
{
    const uint32_t p = n % BEEP_CYCLE;
    const uint32_t k = p / BEEP_N;                 /* 0 2 4 6: a beep; 1 3 5, between */
    if (k > 6 || k % 2) return 0;
    const uint32_t q = p % BEEP_N;
    float g = 1.0f;
    if (q < BEEP_RAMP)          g = (float)q / BEEP_RAMP;
    if (BEEP_N - q < BEEP_RAMP) g = (float)(BEEP_N - q) / BEEP_RAMP;
    /* The phase within the second: a whole number of cycles, and exact for
     * as long as it rings, as a float of n alone would not stay. */
    const float rise = n >= BEEP_RISE ? 1.0f : 0.33f + 0.67f * (float)n / BEEP_RISE;
    return (int16_t)(0.6f * 32767.0f * g * rise * sinf(2.0f * (float)M_PI * BEEP_HZ * (float)(n % AUDIO_RATE_HZ) / AUDIO_RATE_HZ));
}

void clock_client_sound(bool ringing)
{
    static bool     on;
    static uint32_t n;                              /* samples since the ring began */
    if (!ringing) {
        if (on) {
            on = false;
            audio_out_flush();
            audio_out_dac_mute(false);
            bt_link_want_audio(false);              /* the device rests till the next */
        }
        return;
    }
    if (!on) {
        on = true;
        n  = 0;
        bt_link_want_audio(true);                   /* a second or so to open, the jack meanwhile */
    }
    /* A headset's or a speaker's audio open: the beeps there alone. */
    audio_out_dac_mute(bt_link_headset_audio() || bt_link_speaker_audio());
    int16_t pcm[SOUND_FRAME];
    for (int i = 0; i < 40 && audio_out_queued() < audio_out_preroll() + SOUND_AHEAD; i++) {
        for (int j = 0; j < SOUND_FRAME; j++) pcm[j] = beep_sample(n++);
        if (!audio_out_feed_pcm16(pcm, SOUND_FRAME, 1)) break;
    }
}
