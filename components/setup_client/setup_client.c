/* The setup firmware's radio.h: there is no radio. The knob's WiFi setup and
 * its firmware picker are the application's (main/app_main.c); the dial and
 * the rest of the knob only need something that answers for a radio, and
 * this is it -- never ready, never transmitting, taking no settings. */
#include "radio.h"

#include <string.h>

const char *radio_link_name(void) { return "SETUP"; }

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
    o->link = RADIO_LINK_DOWN;
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
void radio_set_rf_gain(uint8_t pct)              { (void)pct; }
void radio_set_rf_power(uint8_t pct)             { (void)pct; }
void radio_set_tuner(bool on)                    { (void)on; }
void radio_set_squelch(uint8_t pct)              { (void)pct; }
void radio_tg_lock(bool locked)                    { (void)locked; }
void radio_mute(bool muted)                        { (void)muted; }

bool radio_get_choice(uint8_t i, char *title, size_t tn, char *name, size_t nn)
{
    (void)i; (void)title; (void)tn; (void)name; (void)nn;
    return false;
}
void radio_choose(uint8_t i) { (void)i; }
