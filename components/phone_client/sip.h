/* A small SIP user agent: one account, one call at a time, over UDP.
 *
 * Enough of RFC 3261 for a telephone behind a home router: REGISTER with
 * digest authentication (RFC 2617, qop=auth), kept alive and refreshed; a
 * call placed (INVITE, its 1xx and 2xx, ACK, CANCEL) or taken (INVITE answered
 * 180 then 200, retransmitted until its ACK); BYE either way, and a re-INVITE
 * answered as it stands. NAT without STUN: rport (RFC 3581) tells the knob its
 * public address, which goes in its Contact and its SDP, and a CRLF keeps the
 * router's mapping open. The media is negotiated here (SDP, RFC 3264) and
 * carried by the caller -- see sip_media_t.
 *
 * Everything runs on the phone task: sip_on_readable() when the socket has
 * something, sip_tick() every few tens of milliseconds. Not thread-safe. */
#ifndef SIP_H
#define SIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char     user[48];        /* the account: its user name and password */
    char     pass[64];
    char     domain[64];      /* registrar and proxy, sip1-d.voice.weepee.io */
    uint16_t port;            /* 5060 */
    char     number[24];      /* the account's own number, for the face */
} sip_account_t;

typedef enum {
    SIP_REG_OFF = 0,          /* no account */
    SIP_REG_TRYING,
    SIP_REG_OK,
    SIP_REG_FAILED,           /* refused, or no answer: tried again later */
} sip_reg_t;

typedef enum {
    SIP_CALL_IDLE = 0,
    SIP_CALL_OUT,             /* placed: the far end has not answered yet */
    SIP_CALL_IN,              /* ringing here */
    SIP_CALL_UP,              /* talking */
    SIP_CALL_ENDED,           /* over, for a moment: why says how */
} sip_call_t;

/* The negotiated audio: where to send RTP, in which codec. */
typedef struct {
    bool     on;              /* send and receive */
    uint32_t ip;              /* the far end's, network order */
    uint16_t port;
    uint8_t  pt;              /* 0 PCMU, 8 PCMA, 9 G.722 */
    int16_t  dtmf_pt;         /* telephone-event's (RFC 4733); -1 none */
} sip_media_t;

typedef struct {
    /* The call's state moved on: `peer` is who (a name, or the number),
     * `why` a word for an ending ("busy", "declined"; "" for a plain one). */
    void (*on_call)(sip_call_t state, const char *peer, const char *number, const char *why);
    /* Media to start, change or stop (m->on false). */
    void (*on_media)(const sip_media_t *m);
} sip_events_t;

/* The account, and the even local port the caller has bound for RTP: starts
 * registering at once. Again with another account starts over. */
void sip_start(const sip_account_t *a, const sip_events_t *ev, uint16_t rtp_port);
void sip_stop(void);

int  sip_socket(void);                /* -1 while stopped */
void sip_on_readable(uint64_t now_ms);
void sip_tick(uint64_t now_ms);

sip_reg_t sip_reg_state(char *why, size_t cap);
/* The address the far end sees us at, once a registrar has said: for the
 * SDP's c= line, and the log. "" until then. */
const char *sip_public_ip(void);

/* A number to call: digits, a leading + allowed, nothing else kept. False if
 * there is a call already, or no registration. */
bool sip_call(const char *number);
void sip_answer(void);
/* Hang up, cancel or decline -- whichever the call is in. */
void sip_hangup(void);
sip_call_t sip_call_state(void);

#endif /* SIP_H */
