/* The radio, whichever one this firmware is for.
 *
 * One firmware per radio (VFO_RADIO in the top-level CMakeLists.txt), and each
 * brings its own client behind this interface: components/tci_client speaks
 * TCI to AetherSDR, components/icom_client speaks Icom's network protocol to
 * an IC-705. Only the chosen one is built. Everything else -- the dial, the
 * meters, PTT and its safety ladder, audio, the configuration page -- is the
 * same code for both.
 *
 * What every client promises:
 *
 *  - Tuning is OPTIMISTIC. The glass and the motor answer in 10-20 ms and the
 *    wire takes longer, with an unbounded tail, so f_display moves at once and
 *    the client reconciles with what the radio reports afterwards.
 *
 *  - The radio is authoritative on every (re)connect. Pushing a stale
 *    frequency at a rig the operator has since retuned would be the rudest
 *    possible bug.
 *
 *  - PTT goes through ptt_fsm (components/vfo_core), permits, confirmation
 *    deadline, teardown ladder and all.
 */
#ifndef VFO_RADIO_H
#define VFO_RADIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    RADIO_LINK_DOWN = 0,
    RADIO_LINK_CONNECTING,
    RADIO_LINK_GREETING,     /* connected, logging in / being greeted */
    RADIO_LINK_READY,
    RADIO_LINK_DEGRADED,     /* up, but holding back (TCI: after a backlog close) */
} radio_link_t;

#define RADIO_GAIN_NAMES 6

/* Memory mode, for a radio with memory channels (has_memories). */
typedef enum {
    RADIO_MEM_OFF = 0,       /* the dial tunes: VFO mode */
    RADIO_MEM_READING,       /* reading the group's channels from the radio */
    RADIO_MEM_READY,         /* the dial selects channels */
    RADIO_MEM_EMPTY,         /* nothing is programmed in this group */
} radio_mem_state_t;

typedef struct {
    radio_link_t link;
    uint8_t    ptt_state;      /* ptt_state_t */
    uint8_t    ptt_rung;       /* teardown ladder position, 0 = not in it */
    uint8_t    ptt_reason;     /* ptt_abort_t */
    uint32_t   permit;         /* PERMIT_* bitmask; all bits = may key */
    uint32_t   ptt_refusals;
    char       tx_why[16];     /* why the radio will not transmit now, as a
                                  refusal says it ("OUT OF BAND"); "" = none */
    int32_t    pong_age_ms;    /* since the radio last answered a ping */
    int64_t    f_display;      /* what the glass shows -- optimistic */
    int64_t    f_server;       /* newest authoritative value; never drawn */
    char       mode[8];
    int32_t    filt_lo, filt_hi;
    uint8_t    filter_no;      /* the radio's filter preset (Icom FIL1-3); 0 = none */
    /* Either side of the S-meter's readout: the AGC, and the front end's gain
     * -- the IC-705's preamp (0 off, 1 and 2 for P.AMP1 and P.AMP2), or an RF
     * gain in dB -- stepping by gain_step from gain_min to gain_max. */
    char       agc[6];         /* as the radio names it ("fast", "mid"); "" = not known */
    bool       have_gain;      /* the radio reports its gain; nothing to show if not */
    int8_t     gain, gain_min, gain_max, gain_step;
    /* A gain whose steps have names rather than numbers -- the ubersdr
     * firmware's noise filter: OFF, NR2, RN2, NR4, as the receiver offers
     * them -- indexed by gain, from 0. */
    uint8_t    n_gain_names;
    char       gain_names[RADIO_GAIN_NAMES][6];
    /* A receiver's signal-to-noise ratio, where it measures one (the ubersdr
     * firmware's, in the AGC's place). */
    bool       have_snr;
    float      snr_db;
    /* Why there is no link, in a word or two, where the client knows
     * ("RECEIVER FULL"); "" = it does not say. */
    char       link_why[16];
    int32_t    rit_hz;
    /* Memory channels: the group the dial uses, the channel selected, and
     * what the radio holds in it -- a repeater's name, shift and tone. */
    bool       has_memories;
    uint8_t    mem_state;      /* radio_mem_state_t */
    uint8_t    mem_group, mem_ch;
    char       mem_name[17];   /* "" if it has none */
    int8_t     mem_duplex;     /* 0 simplex, -1 DUP-, +1 DUP+ */
    int32_t    mem_offset_hz;
    uint16_t   mem_tone_dhz;   /* the tone it sends, in 0.1 Hz; 0 = none */
    /* A second receiver and a choice of antennas (the IC-7610's MAIN and SUB,
     * ANT1 and ANT2, each with or without its RX ANT input). The dial works
     * the receiver the radio has selected; a radio with either has the swipe
     * choose them in place of memory mode. */
    char       model[16];      /* what the radio calls itself; "" = not said */
    int64_t    f_max;          /* the highest it tunes; 0 = not known */
    /* A receiver (the IC-R8600): nothing to key -- the slab says RECEIVER,
     * and neither the glass nor a headset's button or boom arm keys. */
    bool       rx_only;
    uint8_t    n_rx, rx;       /* receivers (0 or 1 = just the one); 0 MAIN, 1 SUB */
    uint8_t    n_ant, ant;     /* antennas to choose from (0 = no choice); 0 = ANT1 */
    bool       has_rx_ant;     /* ...each also with the RX ANT input */
    bool       ant_rx;         /* receiving on the RX ANT input */
    bool       have_ant;       /* the radio has said which */
    /* A tune carrier and an antenna tuner the dial can start (the FlexRadio),
     * as SmartSDR's TX panel has them: TUNE, ATU, and the tuner's memories
     * (MEM). The swipe down offers them. */
    bool       has_tune, has_atu;
    bool       atu_mem;        /* the tuner recalls its memories */
    /* RF gain and RF power, 0-100 %, and the power's full scale in watts to
     * show it in (0 = show %): a swipe from the left offers them. */
    bool       has_levels;     /* the radio takes both */
    bool       have_levels;    /* ...and has said what they are */
    uint8_t    rf_gain_pct, rf_power_pct;
    uint16_t   max_w;
    /* An antenna tuner in the line or out of it (the IC-7610's): a swipe
     * from the right. Not a tune cycle, which transmits (has_atu). */
    bool       has_tuner, have_tuner, tuner_on;
    /* A squelch the dial sets, 0-100 % (the IC-R8600's, in every mode): a
     * swipe from the right, where a radio without a tuner has room. */
    bool       has_squelch, have_squelch;
    uint8_t    squelch_pct;
    /* Something to say once, for a moment ("ATU FAILED"): shown whenever
     * note_seq moves on. */
    char       note[16];
    uint32_t   note_seq;
    /* A question for the operator before the client can go on (see
     * radio_get_choice): asked while n_choices is non-zero, the options
     * changed whenever choices_seq moves on. */
    uint8_t    n_choices, choice_default;
    uint32_t   choices_seq;
    /* An SvxLink reflector (the svxconnect firmware) in place of a radio: the
     * talkgroup takes the band's place, its name the frequency's, and who is
     * talking the S-meter's. The dial steps through talkgroups. */
    bool       reflector;
    uint32_t   tg;             /* the selected talkgroup; 0 = monitoring only */
    char       tg_name[32];    /* from the reflector's portal; "" if unnamed */
    char       talker[16];     /* talking on it now; "" = nobody */
    char       talker_info[32];/* where they are, when the reflector says */
    char       last_talker[16];/* the last one heard, while nobody is */
    uint32_t   talker_ms;      /* how long they have been talking, or since */
    bool       tg_locked;      /* no switching, by the dial or by priority */
    bool       muted;          /* nothing heard */
    float      rx_level_db;    /* the received audio, dBFS */
    char       server[40];     /* the reflector, as the face names it */
    float      smeter_dbm;
    float      tx_mic_dbm, tx_fwd_w, tx_swr, tx_alc;
    float      tx_peak_w;
    bool       slice_locked;
    bool       tx;
    uint8_t    my_trx;
    uint8_t    n_trx;
    /* Counters for the status shade; all of these are diagnostics you want
     * when something feels wrong but nothing is obviously broken. What each
     * counts is the client's to say; zero where it does not apply. */
    uint32_t   connects, closes, reconciles, rejects;
    uint32_t   unknown_cmds, sends, echoes;
    /* TX audio: pacing ticks, frames sent, frames that failed, frames skipped
     * for want of room, and the slowest single send. */
    uint32_t   chronos, txa_sent, txa_failed, txa_skipped, txa_max_us;
    char       last_close[48];
} radio_status_t;

/* What the status page calls this link: "TCI", "LAN". */
const char *radio_link_name(void);

/* Connect to the radio at host:port. `user` and `pass` are for radios that
 * log in; a client that needs neither ignores them. */
esp_err_t radio_start(const char *host, uint16_t port,
                      const char *user, const char *pass);

/* Called from the knob task. Moves f_display immediately; the wire catches up.
 * Returns the new f_display so the caller can base haptic decisions (MHz
 * rollover, decade decimation) on the SAME frequency the radio is being told
 * about -- a second local copy silently drifts, and then the rollover accent
 * fires on a boundary the operator never crossed.
 *
 * In memory mode the knob steps through the group's programmed channels
 * instead, one per detent. */
int64_t radio_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz);

/* Adopt a step size (tapping a digit). Re-latches the anchor. */
void radio_set_step(int32_t step_hz);

/* Stops the RX audio stream while something else needs the link -- currently
 * a firmware upload, which may share the socket and the pipe with it. */
void radio_audio_suspend(bool suspend);

void radio_get_status(radio_status_t *out);
bool radio_is_ready(void);

/* The radio is transmitting, or our PTT is anywhere but idle: what the
 * haptic gate asks, from any task, without a whole status on its stack. */
bool radio_on_air(void);

/* PTT is TOGGLE: tap to key, tap to unkey. Deliberate to enter, forgiving to
 * exit -- you should never have to aim carefully to STOP transmitting. */
void radio_ptt_key(void);
void radio_ptt_unkey(void);
void radio_ptt_toggle(void);

/* Force an abort with a specific reason, for testing the teardown ladder. */
void radio_ptt_force_abort(uint8_t reason);

/* --- setters for the on-screen editors ---------------------------------
 * All are fire-and-forget; what the radio reports back is what the glass
 * shows. Mode names are the lower-case ones the editors offer ("usb", "cw"). */
void radio_set_mode(const char *mode);
void radio_set_filter(int32_t lo, int32_t hi);

/* Pick the radio's own filter preset, where it has them (the IC-705's FIL1-3,
 * reported as filter_no). A no-op for a radio without presets. */
void radio_select_filter(uint8_t n);
void radio_set_rit(int32_t hz);

/* The AGC, by one of the names the radio reports ("fast", "mid", "slow" on an
 * IC-705; AetherSDR has "off", "slow", "med" and "fast"). */
void radio_set_agc(const char *agc);

/* The front end's gain, within gain_min..gain_max. A no-op for a radio that
 * does not report one. */
void radio_set_gain(int8_t gain);

/* Jump to a frequency (band change). Goes through the same optimistic model
 * as knob tuning, so it cannot be mistaken for a remote change. */
void radio_goto_freq(int64_t hz);

/* Memory mode, where the radio has memory channels: on puts the radio on a
 * channel of the dial's group -- the last one used there, else the first
 * programmed -- and off puts it back on its VFO, simplex. No-ops without. */
void radio_memory_mode(bool on);

/* Which memory group the dial steps through (0-99). Remembered. */
void radio_memory_group(uint8_t group);

/* The receiver the dial works (0 MAIN, 1 SUB), and the antenna it uses
 * (0 = ANT1), on the RX ANT input or not -- where the radio has them (n_rx,
 * n_ant); no-ops without. Neither is acted on while transmitting. */
void radio_select_rx(uint8_t rx);
void radio_set_antenna(uint8_t ant, bool rx_ant);

/* A tune carrier at the radio's tune power (has_tune): keyed and unkeyed
 * through the PTT machine like any over -- confirmed, laddered, dropped with
 * the link -- and never for longer than half a minute. PTT stops it. */
void radio_tune(void);
/* One tune cycle of the radio's antenna tuner (has_atu). It keys the
 * transmitter itself, briefly; PTT stops it. */
void radio_atu_tune(void);
/* Whether the tuner recalls a setting it has tuned before (has_atu). */
void radio_atu_memories(bool on);
/* RF gain and RF power, 0-100 % (has_levels). */
void radio_set_rf_gain(uint8_t pct);
void radio_set_rf_power(uint8_t pct);
/* The antenna tuner in the line or out of it (has_tuner); transmits nothing. */
void radio_set_tuner(bool on);
/* The squelch, 0-100 % (has_squelch); 0 is open. */
void radio_set_squelch(uint8_t pct);

/* A question the client puts to the operator before it can go on -- the
 * multiflex firmware's at boot: be a station of its own, or the dial for one
 * already on the radio. radio_get_choice() gives option i as a title and a
 * name ("DIAL FOR", "SHACK-PC"); radio_choose() answers. */
#define RADIO_CHOICES 6
bool radio_get_choice(uint8_t i, char *title, size_t tn, char *name, size_t nn);

/* --- radios the client finds for itself -------------------------------
 * Beside the radios configured on the page, a client may find others -- the
 * FlexRadio firmware, those of a SmartLink account. The dial's RADIO chooser
 * and the radio page list them after the configured ones, with how they are
 * reached (radio_found_via). Choosing one makes it the radio in use from the
 * next boot, and the caller restarts the knob; choosing a configured radio
 * gives it up (radio_found_use(-1)). By default a client finds none. */
int         radio_found_count(void);
bool        radio_found_get(int i, char *name, size_t cap);
const char *radio_found_via(void);             /* "SmartLink" */
int         radio_found_active(void);          /* -1: a configured radio is in use */
esp_err_t   radio_found_use(int i);
void radio_choose(uint8_t i);

/* A reflector's talkgroup lock and mute (reflector in the status); no-ops for
 * a radio. */
void radio_tg_lock(bool locked);
void radio_mute(bool muted);

#endif /* VFO_RADIO_H */
