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

#define UI_SDR_MAX 4
#define UI_RADIOS_MAX 8
#define UI_GAIN_NAMES 6

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
    /* A gain whose steps have names -- the ubersdr firmware's noise filter,
     * OFF, NR2, RN2, NR4 -- and, in the AGC's place, the receiver's SNR. */
    uint8_t  n_gain_names;
    char     gain_names[UI_GAIN_NAMES][6];
    bool     have_snr;
    float    snr_db;
    /* The ubersdr firmware's slab, where PTT is on a transmitter: the spots
     * and voices on the band (ui_set_spots), where the receiver has either;
     * and its SSTV pictures, a swipe from the right (-1: it has no gallery). */
    bool     has_spots;
    int16_t  n_sstv;
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
    /* A tune carrier and an antenna tuner to start (radio.h): with them the
     * swipe down opens a menu -- TUNE, ATU, and the tuner's memories. */
    bool     has_tune, has_atu, atu_mem;
    /* RF gain and power, and a tuner in the line (radio.h): a swipe from the
     * left opens RF GAIN, then POWER; a swipe from the right, TUNER. */
    bool     has_levels, have_levels;
    uint8_t  rf_gain_pct, rf_power_pct;
    uint16_t max_w;
    bool     has_tuner, have_tuner, tuner_on;
    /* Web SDRs as a second receiver (components/sdr_rx): a swipe down chooses
     * LOCAL or one of them; while one plays, the radio is heard left and the
     * SDR right, and BALANCE follows POWER on the swipe from the left. */
    uint8_t  n_sdr;
    char     sdr_name[UI_SDR_MAX][16];
    int8_t   rxsrc;          /* -1 LOCAL, else the SDR */
    bool     sdr_streaming;
    bool     sdr_trouble;    /* not reached, refused, busy ... */
    char     sdr_note[12];   /* ...in a word */
    float    sdr_dbm;        /* its S-meter */
    int8_t   balance;        /* -100 radio .. 0 split .. +100 SDR */
    /* The radios the knob knows: a swipe up chooses another, when there is
     * one, and the knob restarts into it (see ui_commit_t.have_radio). */
    uint8_t  n_radios;       /* 0 or 1: no chooser */
    char     radio_name[UI_RADIOS_MAX][16];
    int8_t   radio_sel;
    /* The first n_radios_direct are reached directly, on the LAN; the rest
     * through radio_via ("SmartLink"): the chooser says which. */
    uint8_t  n_radios_direct;
    char     radio_via[12];
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
    bool     keyed;      /* our PTT is not idle: keying, on the air, unkeying */
    bool     link_ok;
    bool     slice_locked;
    bool     may_key;
    /* Transient banner: AetherSDR's refusal reason, or ours. NULL for none. */
    const char *warn;
} ui_state_t;

enum { UI_MEM_OFF = 0, UI_MEM_READING, UI_MEM_READY, UI_MEM_EMPTY };

/* The swipe menu's items: SmartSDR's TX panel -- a tune carrier, a tuner
 * cycle, and the tuner's memories on or off. */
enum { UI_ACT_NONE = 0, UI_ACT_TUNE, UI_ACT_ATU, UI_ACT_MEM };

void ui_update(const ui_state_t *st);

/* Step chosen by tapping a frequency digit. Returns 0 if nothing changed. */
int32_t ui_take_step_request(void);

/* A tap landed on the PTT pill. Consumed by the caller. */
bool ui_take_ptt_tap(void);

/* --- a receiver's slab (the ubersdr firmware) ------------------------------
 * The spots and voices on the dial's band, in frequency order: the slab
 * shows the one nearest the dial, and a tap on it opens them all on the knob
 * -- turn to one, tap its panel, and the receiver goes there, in its mode. */
#define UI_SPOTS_MAX 24
typedef struct {
    char     call[12];      /* "" for a voice nobody has named */
    uint32_t hz;
    char     mode[5];
    char     what[24];      /* "DX  4m", "voice 12 dB", "CW 22 wpm 14 dB  1m" */
    bool     heard;         /* talking now: the receiver hears a voice there */
} ui_spot_t;
void ui_set_spots(const ui_spot_t *spots, uint8_t n);

/* When the knob or the glass was last used (lv ticks): what an idle timer
 * on the far end may want to hear about. */
uint32_t ui_last_use(void);

/* The SSTV viewer, opened from the swipe from the right: the knob steps
 * through the receiver's pictures, newest first, and any tap closes it.
 * ui_sstv_wanted() is the picture it wants (-1 closed); ui_sstv_show() puts
 * one up -- RGB565, w x h, kept by the caller until the next -- or, failed,
 * says why in the caption. */
int  ui_sstv_wanted(void);
void ui_sstv_show(const uint16_t *px, int w, int h, int idx, const char *title,
                  const char *caption, bool failed);

/* Memory mode on, or off again: V/M, last on the swipe down, on a radio with
 * memories. Consumed by the caller. (The swipe down chooses what is heard --
 * LOCAL or a web SDR, then a second receiver and the antenna, then V/M; the
 * swipe up, the radio; what they choose comes as a commit.)
 *
 * Everything but PTT and the update question acts when the finger lifts,
 * not when it lands, so that a swipe is not first taken for a tap on
 * whatever it started on. */

/* A reflector's lock or mute symbol was tapped. Consumed by the caller. */
bool ui_take_lock_tap(void);
bool ui_take_mute_tap(void);

/* --- knob-driven field editors -------------------------------------------
 * Tapping band, mode, filter, AGC, gain, RIT or volume opens a large editor
 * and the knob chooses a value. Filter, AGC, gain and RIT (and volume and mic
 * gain) take effect as the knob turns, and a tap anywhere closes them; the
 * others -- band, mode, VFO, antenna, the swipe's menu -- act on a tap on
 * their panel, and a tap anywhere else closes them untouched. While an
 * editor is open the knob must NOT tune, so the caller checks
 * ui_edit_active() first.
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
    uint8_t  action;       /* UI_ACT_*: chosen from the swipe's menu */
    bool     atu_mem;      /* with UI_ACT_MEM: the tuner's memories on */
    bool     live;         /* sent as the knob turns, not on a tap */
    bool     have_rit;     int32_t rit_hz;
    bool     have_freq;    int64_t freq_hz;
    bool     have_rf_gain;  uint8_t rf_gain_pct;
    bool     have_rf_power; uint8_t rf_power_pct;
    bool     have_tuner;    bool    tuner_on;
    bool     have_rxsrc;    int8_t  rxsrc;
    bool     have_balance;  int8_t  balance;
    bool     have_radio;    int8_t  radio;      /* another radio: restart into it */
    bool     have_vm;       bool    vm_mem;     /* V/M: memory mode, or the VFO */
    bool     have_spot;     uint32_t spot_hz;  char spot_mode[5];  /* a spot: tune there */
} ui_commit_t;

/* Non-zero if the operator accepted an edit, or a live editor moved (live
 * set): only the newest is kept. Consumed by the caller. */
bool ui_take_commit(ui_commit_t *out);

/* A question from the radio's client that needs an answer before it can go
 * on (the multiflex firmware at boot: a station of its own, or the dial for
 * one already on the radio). Shown like an editor -- the knob chooses, a tap
 * on its panel answers -- but a tap anywhere else leaves it up. Each option
 * is a title over a name ("DIAL FOR" / "SHACK-PC"). n = 0 takes it down. */
#define UI_CHOICES 10
void ui_ask_choice(const char titles[][12], const char names[][24], uint8_t n, uint8_t def);
bool ui_choice_active(void);
/* The answer, once: the option's index, or -1 while there is none. */
int  ui_take_choice(void);

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
/* The same screen for the moment before a restart that installs at boot:
 * REBOOTING, into update mode -- no percentage yet, which read as a download
 * stuck at 0%. The install after the restart shows its progress as usual. */
void ui_updating_reboot(void);
/* ...and for a restart into another radio: SWITCHING TO, and its name. Being
 * that screen, it puts PTT out of reach until the restart. */
void ui_switching(const char *radio);
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
/* `restart_first`: a yes restarts the knob, which installs at boot, so the
 * screen it puts up says so rather than showing a percentage. */
bool ui_ask_update(const char *version, const char *running,
                   bool restart_first);                        /* false: not shown */
/* 1 = install, -1 = no, 0 = still asking or nothing asked. Each answer is
 * returned once. */
int  ui_take_update_answer(void);
/* The same question panel, for a yes that must not come from a stray tap:
 * `title` on it, `hint` under it, and a turn of the knob is the yes -- not in
 * its first 0.8 s, and that turn, and the turns for two seconds after it,
 * tune nothing. A tap anywhere, the radio going into transmit or ten seconds
 * of nothing say no. Answered through ui_take_update_answer(). */
bool ui_ask_turn(const char *title, const char *hint);

/* The address card came up under a finger held on the S-meter: the motor
 * says it can let go. Once per press; consumed here. */
bool ui_take_card_shown(void);

/* The operator asks for the firmware picker (the setup firmware): with the
 * addresses on screen, a finger held three seconds on the S-meter or on them.
 * Once per press; consumed here. */
bool ui_take_picker_request(void);

/* The setup firmware's screen: a title and a few lines over the whole face,
 * under the dial's questions and editors. */
void ui_setup_show(const char *title, const char *text);
/* From the knob task: the knob turned, so an open question is answered --
 * no, or yes to ui_ask_turn(). True when the turn is spent on that yes and
 * must not tune. */
bool ui_ask_knob_moved(void);

/* Step through 0/90/180/270. Orientation is a physical property of how the
 * panel is mounted, and guessing it costs a flash cycle each time -- so make
 * it switchable from the console instead. */
void    ui_cycle_rotation(void);
uint8_t ui_rotation(void);

#endif /* VFO_UI_H */
