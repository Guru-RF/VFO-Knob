/* The phone firmware's own calls, beside radio.h: what the dial's keypad and
 * the web page ask of the telephone. Any task; the phone task acts on them. */
#ifndef PHONE_CLIENT_H
#define PHONE_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

/* A number: digits, a leading +, * and #. False with a call up already, or
 * no registration. */
bool phone_dial(const char *number);
void phone_answer(void);
/* Hang up, cancel or decline: whichever the call is in. */
void phone_hangup(void);
/* A key, in a call: its DTMF -- RFC 4733 events, or the tones in the audio
 * where the far end takes none. Ignored outside a call. */
void phone_dtmf(char key);

/* A test switch, for now: the call face's split arc, its two meters. On;
 * GET /api/phone/meters?on=0 takes them off until the next restart -- the
 * spectra they once shared the face with are gone (2026-10-02). */
void phone_set_meters(bool on);
bool phone_meters(void);

/* The calls, newest first: out, in, missed (it rang, nobody answered) or
 * declined, with when and for how long. Kept in flash across restarts. */
#define PHONE_HIST_MAX 20
enum { PHONE_CALL_OUT, PHONE_CALL_IN, PHONE_CALL_MISSED, PHONE_CALL_DECLINED };
typedef struct {
    char     number[24];
    char     name[24];          /* a favourite's, or the caller's own; "" for none */
    uint32_t when;              /* seconds since 1970; 0: the clock was not set yet */
    uint16_t secs;              /* how long it was up; 0: never answered */
    uint8_t  kind;              /* PHONE_CALL_* */
    uint8_t  fresh;             /* missed, and not looked at yet */
} phone_call_t;
/* A copy of the history; and a count that moves with every change to it. */
int      phone_history(phone_call_t *out, int max);
uint32_t phone_history_seq(void);
/* The history was looked at: its missed calls are no longer new. */
void     phone_history_seen(void);

#endif /* PHONE_CLIENT_H */
