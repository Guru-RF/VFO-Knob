/* An Icom radio's power over its network (the icom firmware, not icombt):
 * waking one in standby, and putting it back there. Plain C, so test/host
 * checks it on the PC (test_icom_power); the CI-V itself is icom_client's.
 *
 * Icom's CI-V reference guides (IC-705, IC-9700, IC-7610, IC-R8600):
 * 18 00 "Turn OFF the transceiver", 18 01 "Turn ON the transceiver" -- on the
 * IC-705 "when the transceiver is OFF (Standby/Shutdown)". The FE preamble
 * the guides ask before 18 01 is for a serial port at a given baud rate;
 * over the radio's own LAN protocol wfview sends none, and neither does the
 * knob. The radio keeps its network up in standby only with SET > Network
 * (IC-705: WLAN Set > Remote Settings) > Power OFF Setting (for Remote
 * Control) at Standby/Shutdown, and the operator choosing "Standby (for
 * Remote Control)" when switching it off: then the knob logs in as ever,
 * the CI-V stream opens, and the radio answers nothing until it is on.
 * That setting is readable over CI-V (1A 05, its number per model): 00
 * Shutdown only, 01 Standby/Shutdown. */
#ifndef ICOM_POWER_H
#define ICOM_POWER_H

#include <stdbool.h>
#include <stdint.h>

/* CI-V open this long with no frequency answered: the radio is taken to be
 * in standby -- at once, when the operator asked for it on. A radio that is
 * on answers its first 03 within a few hundred milliseconds (the client asks
 * again at 1.5 s), and 18 01 to one that is on does nothing but answer FB.
 * Under the greeting's 8 s, which would otherwise end the session first. */
#define ICOM_WAKE_AFTER_MS 4000
/* ...then this long for it to come up and answer: Hamlib waits 5.5 s for an
 * IC-9700, wfview 5 s before it asks anything. With room for the radio's
 * network coming back after it. */
#define ICOM_WAKE_BOOT_MS  30000

typedef struct {
    bool     sent;             /* 18 01 went out, this session */
    uint32_t t_sent;
} icom_wake_t;

typedef enum {
    ICOM_WAKE_WAIT = 0,        /* nothing to do now */
    ICOM_WAKE_SEND,            /* send 18 01 now */
    ICOM_WAKE_GIVE_UP,         /* it did not come up: end the session */
    ICOM_WAKE_ASLEEP,          /* in standby, and not to be woken unasked:
                                  leave it, and ask the operator */
} icom_wake_do_t;

/* A new session: nothing sent. */
void icom_wake_reset(icom_wake_t *w);

/* Who may wake it. The knob wakes the radio by itself when it starts -- a
 * radio left in standby for it -- and after that only when the operator
 * asks: a radio switched to standby at the radio, under a running knob, is
 * not switched back on behind its operator's back. */
typedef enum {
    ICOM_WAKE_NEVER = 0,       /* only ask: ICOM_WAKE_ASLEEP */
    ICOM_WAKE_AUTO,            /* the first session since the knob started */
    ICOM_WAKE_ASKED,           /* the operator tapped POWER ON: at once */
} icom_wake_may_t;

/* One pass of the client's loop. civ_open and t_civ_open: the CI-V stream,
 * and when it opened; have_freq: the radio has answered a frequency, which
 * is what "on" looks like. */
icom_wake_do_t icom_wake_step(icom_wake_t *w, uint32_t t, bool civ_open, uint32_t t_civ_open,
                              bool have_freq, icom_wake_may_t may);

/* 18 01 is out and the radio has yet to answer: the session's greeting may
 * take longer than the usual few seconds. */
bool icom_wake_pending(const icom_wake_t *w, bool have_freq);

/* The Power OFF Setting (for Remote Control), as 1A 05 reads it back. */
typedef enum {
    ICOM_OFF_UNKNOWN = 0,      /* not read (yet), or a model without its number */
    ICOM_OFF_SHUTDOWN,         /* 00, Shutdown only: 18 00 and it is gone */
    ICOM_OFF_STANDBY,          /* 01, Standby/Shutdown */
} icom_off_t;

icom_off_t icom_off_setting(uint8_t v);

/* Whether the knob may put the radio in standby: the link up, nothing on the
 * air, and the radio set to keep its network in standby -- never on a guess,
 * as a radio that shuts down can only be switched on at the radio. */
bool icom_standby_allowed(icom_off_t setting, bool ready, bool on_air);

#endif /* ICOM_POWER_H */
