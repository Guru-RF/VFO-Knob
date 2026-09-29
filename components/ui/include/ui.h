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
    uint8_t  filter_no;  /* the radio's preset (FIL1-3) where it has them; 0 = none */
    /* Beside the S-unit readout: the AGC on the left, and on the right the
     * front end's gain -- P.AMP on an Icom, RF.G on AetherSDR. See radio.h. */
    char     agc[6];     /* "" = not known */
    bool     have_gain;
    int8_t   gain, gain_min, gain_max, gain_step;
    /* Memory mode (radio.h): the knob selects channels instead of tuning, and
     * the channel takes the frequency readout's place. mem_state is UI_MEM_*,
     * in the order of radio_mem_state_t. */
    bool     has_memories;
    uint8_t  mem_state, mem_group, mem_ch;
    char     mem_name[17];
    int8_t   mem_duplex;     /* 0 simplex, -1 DUP-, +1 DUP+ */
    int32_t  mem_offset_hz;
    uint16_t mem_tone_dhz;
    /* A second receiver and a choice of antennas (radio.h): the swipe down
     * chooses those instead of memory mode -- the receiver, then its antenna. */
    uint8_t  n_rx, rx;       /* 0 MAIN, 1 SUB */
    uint8_t  n_ant, ant;     /* 0 ANT1 */
    bool     has_rx_ant, ant_rx, have_ant;
    /* A reflector (radio.h): the talkgroup and its name in place of band and
     * frequency, the talker in place of the S-units, lock and mute either
     * side of it, and the audio level on the arc. */
    bool     reflector;
    bool     connecting;     /* the link is being made, not down for good */
    uint32_t tg;
    char     tg_name[32];
    char     talker[16], talker_info[32], last_talker[16];
    uint32_t talker_ms;
    bool     tg_locked, muted;
    float    rx_level_db;
    char     server[40];
    int32_t  rit_hz;
    float    smeter_dbm;
    float    tx_mic_dbm, tx_fwd_w, tx_peak_w, tx_swr, tx_alc;
    bool     tx;         /* the radio is transmitting, whoever keyed it */
    bool     tx_remote;  /* ...and it was not us, so we cannot stop it */
    bool     link_ok;
    bool     slice_locked;
    bool     may_key;
    /* Transient banner: AetherSDR's refusal reason, or ours. NULL for none. */
    const char *warn;
} ui_state_t;

enum { UI_MEM_OFF = 0, UI_MEM_READING, UI_MEM_READY, UI_MEM_EMPTY };

void ui_update(const ui_state_t *st);

/* Step chosen by tapping a frequency digit. Returns 0 if nothing changed. */
int32_t ui_take_step_request(void);

/* A tap landed on the PTT pill. Consumed by the caller. */
bool ui_take_ptt_tap(void);

/* A swipe down the face: memory mode on, or off again. Only with a link, in
 * receive, on a radio with memories, and with nothing else asking for the
 * finger. Consumed by the caller. On a radio with a second receiver or a
 * choice of antennas the swipe opens their editors instead, and what they
 * choose comes as a commit.
 *
 * Everything but PTT and the update question acts when the finger lifts,
 * not when it lands, so that a swipe is not first taken for a tap on
 * whatever it started on. */
bool ui_take_swipe(void);

/* A reflector's lock or mute symbol was tapped. Consumed by the caller. */
bool ui_take_lock_tap(void);
bool ui_take_mute_tap(void);

/* --- knob-driven field editors -------------------------------------------
 * Tapping band, mode, filter, AGC, gain, RIT or volume opens a large editor;
 * the knob chooses a value and a tap anywhere accepts it. While an editor is
 * open the knob must NOT tune, so the caller checks ui_edit_active() first.
 *
 * Selection happens on the precise rotary and commitment on the imprecise
 * touch, which is what makes the whole thing usable on 45 mm of round glass. */
bool ui_edit_active(void);
void ui_edit_rotate(int32_t detents);

typedef struct {
    bool     have_mode;    char    mode[8];
    bool     have_filter;  int32_t filt_lo, filt_hi;
    bool     have_filter_no; uint8_t filter_no;
    bool     have_agc;     char    agc[6];
    bool     have_gain;    int8_t  gain;
    bool     have_mem_group; uint8_t mem_group;
    bool     have_rx;      uint8_t rx;
    bool     have_ant;     uint8_t ant;  bool ant_rx;
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

/* Text shown after a long press on the meter arc: where the knob is on the
 * network. A tap on the card puts it away. Kept as formatted text so the UI
 * needs no networking headers. */
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

/* Ask on the dial whether to install a newer release. A tap on the question
 * says yes. Turning the knob, a tap anywhere else, the radio going into
 * transmit, or ten seconds of nothing all say no -- with no countdown: it
 * simply goes away and the dial carries on. While it is up it takes the tap,
 * so nothing behind it -- PTT least of all -- is touched by answering it; a
 * tap on it in its first 0.8 s is ignored, being aimed at what was there
 * before; and a yes puts the update screen up at once, taking PTT away. */
bool ui_ask_update(const char *version, const char *running);   /* false: not shown */
/* 1 = install, -1 = no, 0 = still asking or nothing asked. Each answer is
 * returned once. */
int  ui_take_update_answer(void);
/* From the knob task: the knob turned, so an open question is answered no. */
void ui_ask_knob_moved(void);

/* Step through 0/90/180/270. Orientation is a physical property of how the
 * panel is mounted, and guessing it costs a flash cycle each time -- so make
 * it switchable from the console instead. */
void    ui_cycle_rotation(void);
uint8_t ui_rotation(void);

#endif /* VFO_UI_H */
