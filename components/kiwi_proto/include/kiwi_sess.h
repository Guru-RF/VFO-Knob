/* KiwiSDR's protocol on the wire: one session with a receiver, its /status,
 * and the configuration page's Test -- for the kiwi firmware's receiver
 * (components/kiwi_client) and the web SDR beside a radio (components/sdr_rx).
 *
 * A session runs on the app path, ws://host:port/<ts>/SND, as kiwirecorder
 * does: never the browser paths, which would present the knob as a browser
 * and get round its owner's limits for apps. Every refusal is classed
 * (kiwi_end_t): the HTTP status of the upgrade, the MSG that ends it, a
 * KiwiSDR's silent door for apps.
 *
 * A receiver behind a front -- the kiwisdr.com proxy, Cloudflare -- speaks
 * the same protocol over TLS only: https:// for its /status, wss:// for the
 * session, on 443, with the Host the front routes on. An address says which
 * (kiwi_addr_t's tls), and an http:// receiver's redirect to https:// on its
 * own host is followed at once, once, the caller told so to keep https for
 * it from then on; any other redirect is not ("moved to <host>", MOVED). A
 * certificate that does not verify -- signed by no authority in the bundle,
 * or for another name -- is refused (CERT). TLS is mbedTLS on the knob's own
 * socket (kiwi_tls.h), all of it in PSRAM, and a handshake -- a second or two
 * of software on the S3 -- goes a step at a time between the caller's go_on
 * slices: a receiver chosen meanwhile waits for the step under way at most,
 * up to a second or so. The newest receivers' TLS sessions are kept for the
 * next connection: resumed, where the front allows it, it costs none of
 * that.
 *
 * Everything runs on the caller's task, which may have its stack in PSRAM:
 * nothing here writes flash (the day-limit marks go through kiwi_mark's own
 * timer). Its buffers are one PSRAM block, kiwi_sess_new()'s.
 */
#ifndef KIWI_SESS_H
#define KIWI_SESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kiwi_proto.h"

/* A receiver: its address -- over TLS (`tls`: https://, wss://) or in the
 * clear -- and its two passwords ("" for none). `kport`: the port the knob
 * keeps what it knows of it under (kiwi_hp), where that is not its own -- the
 * port it had before an https:// redirect moved it; 0 for its own. */
typedef struct {
    const char *host;
    uint16_t    port;
    const char *pass, *ipl;
    bool        tls;
    uint16_t    kport;
} kiwi_addr_t;

/* Its key: kiwi_hp of its host and the port it is known by. */
uint32_t kiwi_addr_hp(const kiwi_addr_t *a);

/* Who the receivers' owners see of the knob in their lists of listeners (SET
 * ident_user, kiwi_ident_cmd): one name for every session -- the kiwi
 * firmware's, the web SDR beside a radio's, a Test's -- "" for "VFO-Knob".
 * Sent at each login, and a new one at once to the sessions logged in.
 * Whoever keeps it sets it (components/sdr_rx, with the receivers' list);
 * any task, at most KIWI_IDENT_MAX - 1 bytes kept. */
void kiwi_ident_set(const char *who);
void kiwi_ident(char *out, size_t cap);

/* What to listen to, as the caller has it now. */
typedef struct { kiwi_tune_t t; uint8_t agc, nr, sq_pct; } kiwi_ctl_t;

typedef enum { KIWI_ST_CONNECTING, KIWI_ST_LOGGED_IN, KIWI_ST_STREAMING } kiwi_state_t;

/* For the status shade: text frames sent, SND frames had, and those of them
 * whose audio was lost -- not a Kiwi's, stereo, undecodable. `big`: frames
 * too big for the buffer, let go by unread: the settings a receiver sends at
 * every login (load_cfg, load_dxcfg, load_dxcomm_cfg), no audio. */
typedef struct { uint32_t sent, frames, dropped, big; } kiwi_counts_t;

/* How the audio came, these last 30 s: for the log. */
typedef struct {
    uint32_t frames;            /* SND frames */
    uint32_t gap_ms;            /* the longest between two arriving */
    uint32_t lost;              /* sequence numbers that never came: the receiver let them go */
    uint32_t held;              /* frames late with none missing: the network held them */
    uint32_t jumps;             /* to the live point, frames left out */
    uint32_t left_ms;           /* ...what they left out */
    uint32_t breaks;            /* stalls the ring could not ride out (target_max) */
    uint32_t level_ms;          /* the ring's level, its average; 0 with no ring */
    uint32_t target_ms;         /* ...and what it is kept at now */
    float    trim;              /* the drift trim now */
} kiwi_report_t;

/* The caller's side of a session, all called on the session's task. */
typedef struct {
    /* The newest settings: called every pass, so cheap. */
    void (*ctl)(void *ctx, kiwi_ctl_t *out);
    /* Where it tunes, offset..offset+bandwidth, once the receiver has said:
     * before the first tune, and whenever it says otherwise. */
    void (*range)(void *ctx, int64_t f_min, int64_t f_max);
    /* The dial out of the receiver's reach -- outside that range, what it
     * says it covers (kiwi_tune_reaches) -- or back within it: said once a
     * change. Meanwhile the dial is not followed: the receiver stays tuned
     * where it was, or, never tuned yet, at its edge nearest the dial; its
     * audio comes as silence, as a closed squelch's does, so the ring keeps
     * its time and nothing breaks; no S-meter. Back within it, the tune goes
     * at once. NULL: the dial is the caller's to keep inside (the kiwi
     * firmware's receiver moves it there), a dial outside played at the
     * edge. */
    void (*reach)(void *ctx, bool out);
    /* Its audio, 24 kHz mono; zeros while its squelch is closed. */
    void (*audio)(void *ctx, const int16_t *pcm, size_t n);
    /* The ring that audio goes into: the samples it holds now (NULL: none
     * known). With it the session keeps the ring near `target`, of `room`,
     * and the audio at the live point: after a stall the ring could not
     * ride out, the frames held up meanwhile -- handed over at once, or at
     * least twice as fast as they play -- are left out in one jump, whole
     * ones, oldest first, where the stall's silence already is. A backlog
     * handed over more slowly is played as it comes, and cut back to the
     * target each time it would fill the ring past 80 %: a cut every few
     * seconds while it lasts, never a piece lost at every frame. With
     * `trim` it also follows the receiver's clock, the audio a hair shorter
     * or longer, so the ring stays where it is for hours.
     *
     * With `target_max` over `target`, the target is the network's to set:
     * a stall the ring could not ride out -- it ran dry (`underruns`, the
     * times its playing stopped for want of audio), or a backlog was cut
     * short with sound still in it -- is a break, and a break within two
     * minutes of the last grows the target to hold a stall as long as that
     * one, a quarter frame to spare, half a frame at least, up to
     * target_max; the ring then waits for that much before it plays again
     * (`preroll`, NULL: none to tell). Three minutes without a break and it
     * eases back down, half a frame each 30 s, to `target`, the trim
     * bringing the ring's level after it: never a cut -- a frame late by no
     * more than the ring's level is judged by that level meanwhile; a stall
     * that runs the ring dry is a break as ever, its backlog left out down
     * to the eased target. More delay only while the network is bad, fewer
     * breaks. The ring holds room + target_max - target, and what may fill
     * it over the target rises with the target. */
    size_t (*queued)(void *ctx);
    size_t target, room;
    bool   trim;
    size_t target_max;
    void (*preroll)(void *ctx, size_t n);
    uint32_t (*underruns)(void *ctx);
    /* Its S-meter, about six times a second: dBm, the ADC overloaded, the
     * squelch closed. */
    void (*meter)(void *ctx, float dbm, bool ovl, bool squelched);
    void (*state)(void *ctx, kiwi_state_t st, const kiwi_said_t *said);
    /* False ends it: WANT. Called every pass (50 ms). */
    bool (*go_on)(void *ctx);
    /* Someone has used the knob -- a touch, a turn -- since this last said
     * true: the receiver's idle timer is told so (SET inactivity_ack), at
     * most once a minute. Asked only when one may go, so use in between
     * waits for it rather than being lost. Tuning resets that timer by
     * itself. NULL: never told. */
    bool (*ack)(void *ctx);
    /* Every 30 s while it plays: how the audio came. NULL: not told. */
    void (*report)(void *ctx, const kiwi_report_t *r);
    /* An http:// receiver's redirect to https:// on its own host, followed,
     * and TLS spoken there: `tls_port` is where. The caller keeps https for
     * the receiver from now on, under the key it had (kiwi_addr_t's kport):
     * quick, no flash -- it is said on the session's way in, before its
     * login. NULL: not told (said->moved says it after). */
    void (*moved)(void *ctx, uint16_t tls_port);
    kiwi_counts_t *counts;              /* NULL: not counted */
    void       *ctx;
    const char *tag;                    /* for the log */
} kiwi_link_t;

typedef struct kiwi_sess kiwi_sess_t;

/* Its buffers -- a 16 KB frame, the decoded and the resampled audio -- in
 * PSRAM, once, for every session after. NULL without the memory. */
kiwi_sess_t *kiwi_sess_new(void);

/* One session, until it ends: why (never NONE). `st` is the receiver's
 * /status as read this boot (NULL: not known), for what it is -- a Web-888's
 * S-meter is biased otherwise, and only a KiwiSDR shuts its door in silence.
 * `said` is what it was told, filled in as it goes. Once its login is out,
 * go_on is not asked until the login is answered: an answer the knob leaves
 * unread could be a refusal the receiver counted. A day limit's mark is the
 * caller's to set: said->limit_at_login says whether the receiver counted
 * it, said->unanswered whether it may have (the login had no answer, and the
 * receiver did not close it); a session that has streamed 10 s lets one
 * rest. said->connected: the attempt reached the receiver at all;
 * said->moved: where a redirect sent it -- followed to https://, or not:
 * MOVED. */
kiwi_end_t kiwi_sess_run(kiwi_sess_t *s, const kiwi_addr_t *a, const kiwi_status_t *st,
                         const kiwi_link_t *l, kiwi_said_t *said);

/* Its /status -- over https:// for a receiver spoken over TLS -- from the
 * first of its addresses that takes a connection: NULL once one answered
 * (out->ok says whether as a Kiwi), else why none did -- "left" when `go_on`
 * (NULL: never asked) said the caller wants another meanwhile; "moved" for a
 * redirect not followed, `mv->to` saying where; "certificate not valid".
 * An http:// receiver's redirect to https:// on its own host is followed
 * there and then: `mv->tls_port` says where TLS was spoken, which the caller
 * keeps for it (`mv` may be NULL). A read cut short keeps its whole lines
 * only. Owners drop an address that polls this: the caller reads it rarely.
 * `tag`: the caller's, for the log (NULL: "kiwi"). */
const char *kiwi_status_read(const kiwi_addr_t *a, kiwi_status_t *out, kiwi_moved_t *mv, bool (*go_on)(void *),
                             void *ctx, const char *tag);

/* A receiver's /status as a session's task read it (kiwi_hp), kept for this
 * boot -- and, kept, read back with when it was read (esp_timer_get_time):
 * the kiwi firmware's two ears read a receiver's once between them, each
 * one's first look at it, and a marked one's within the minute, as one ear
 * would have it. The newest four receivers'; any task. */
void kiwi_status_keep(uint32_t hp, const kiwi_status_t *st);
bool kiwi_status_kept(uint32_t hp, kiwi_status_t *out, int64_t *at);

/* The configuration page's Test: its /status -- the one read for the last
 * Test of it, within a minute -- then a login on the app path for the answer;
 * except to a receiver with a day-limit mark ("day limit": a test is not
 * choosing it and spends none of its tries), to a KiwiSDR that lets no apps
 * in ("no apps", from its /status), and to the receiver in use while its
 * session is on its way or playing ("in use", no second login: in_use(hp)
 * says so, asked once the /status is read; NULL: none is). A refusal for the
 * day limit marks it, as a login left unanswered does where it may count.
 * An http:// receiver's redirect to https:// on its own host is followed,
 * and said -- in `mv` (may be NULL), and in the JSON's "tls" and "port", the
 * address it was spoken on, even when the test fails after it; one not
 * followed is said as "moved to <host>". JSON into `json` (1 KB). Blocks
 * for some 40 s at most: 25 s on its way to the login -- the /status, a
 * redirect, each address's connection and TLS -- then the login's answer,
 * and a few seconds after it. Its task's stack wants room for a TLS
 * handshake: not the web server's.
 * `tag`: the caller's, for the log (NULL: "kiwi"). */
esp_err_t kiwi_test(const kiwi_addr_t *a, bool (*in_use)(uint32_t hp), char *json, size_t cap, kiwi_moved_t *mv,
                    const char *tag);

/* A Test about to log in to this receiver (kiwi_hp), or logging in: its
 * answer is still to come, and a refusal in it marks the receiver. A
 * session's task says first that it is on its way there -- what the Test's
 * in_use() reads -- then waits while this is true, and looks at the mark
 * again before its own login. Each side says its own before it looks at
 * the other's, so one of them always sees the other: a Test and a session
 * never log in at once, and two refusals are never spent together. */
bool kiwi_test_busy(uint32_t hp);

#endif /* KIWI_SESS_H */
