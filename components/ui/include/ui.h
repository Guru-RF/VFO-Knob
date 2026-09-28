/* Round 360x360 VFO display.
 *
 * Geometry: 21.5% of a square layout falls off this glass, concentrated exactly
 * where rectangular conventions put the important things. Every widget is kept
 * inside r=168 (12 px of slack for bezel and lens misalignment).
 *
 * The frequency readout is NINE SEPARATE LABELS on a fixed pitch, not one
 * label. A single label invalidates the whole 322x76 band -- about 49 kB and
 * 2.5 ms of QSPI -- on every detent; per-digit touches roughly 6 kB. That is
 * the single most important implementation decision on this screen.
 */
#ifndef VFO_UI_H
#define VFO_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t ui_init(void);

typedef struct {
    int64_t  freq_hz;
    int32_t  step_hz;
    uint8_t  accel_mult;
    const char *mode;
    int32_t  filt_lo, filt_hi;
    int32_t  rit_hz;
    float    smeter_dbm;
    float    tx_mic_dbm, tx_fwd_w, tx_peak_w, tx_swr, tx_alc;
    bool     tx;         /* the radio is transmitting, whoever keyed it */
    bool     tx_remote;  /* ...and it was not us, so we cannot stop it */
    bool     link_ok;
    bool     slice_locked;
    uint32_t tot_remain_ms;
    bool     may_key;
    /* Transient banner: AetherSDR's refusal reason, or ours. NULL for none. */
    const char *warn;
} ui_state_t;

void ui_update(const ui_state_t *st);

/* Step chosen by tapping a frequency digit. Returns 0 if nothing changed. */
int32_t ui_take_step_request(void);

/* A tap landed on the PTT pill. Consumed by the caller. */
bool ui_take_ptt_tap(void);

/* --- knob-driven field editors -------------------------------------------
 * Tapping band, mode, filter, RIT or volume opens a large editor; the knob
 * chooses a value and a tap anywhere accepts it. While an editor is open the
 * knob must NOT tune, so the caller checks ui_edit_active() first.
 *
 * Selection happens on the precise rotary and commitment on the imprecise
 * touch, which is what makes the whole thing usable on 45 mm of round glass. */
bool ui_edit_active(void);
void ui_edit_rotate(int32_t detents);

typedef struct {
    bool     have_mode;    char    mode[8];
    bool     have_filter;  int32_t filt_lo, filt_hi;
    bool     have_rit;     int32_t rit_hz;
    bool     have_freq;    int64_t freq_hz;
} ui_commit_t;

/* Non-zero if the operator accepted an edit. Consumed by the caller. */
bool ui_take_commit(ui_commit_t *out);

/* Placeholder until v2 streams RX audio to the onboard DAC. The control and
 * its icon exist now so the interaction is settled before the audio path
 * arrives. */
uint8_t ui_volume(void);
uint8_t ui_mic_gain(void);

/* Restore persisted levels at boot, before ui_init() draws anything. */
void ui_set_levels(uint8_t volume, uint8_t mic_gain);

/* Text shown when the meter arc is tapped: where the knob is on the network.
 * Kept as formatted text so the UI needs no networking headers. */
void ui_set_netinfo(const char *text);

/* Firmware-update screen. Takes over the display for the duration of an
 * upload: it frees the device for the transfer, tells the operator not to
 * unplug, and -- being a separate screen -- puts PTT out of reach. */
/* Idle blanking, in two stages. Any knob movement, touch or transmit is
 * "use": after dim_minutes the backlight drops but the dial stays readable,
 * after blank_minutes it goes dark, and the next use restores it. Either
 * value at 0 disables that stage. */
void ui_note_activity(void);
void ui_dim_set_minutes(uint16_t dim_minutes, uint16_t blank_minutes);
void ui_dim_tick(bool transmitting);

void ui_updating_show(void);
void ui_updating_progress(int percent);
void ui_updating_result(bool ok, const char *message);
void ui_updating_hide(void);

/* Step through 0/90/180/270. Orientation is a physical property of how the
 * panel is mounted, and guessing it costs a flash cycle each time -- so make
 * it switchable from the console instead. */
void    ui_cycle_rotation(void);
uint8_t ui_rotation(void);

#endif /* VFO_UI_H */
