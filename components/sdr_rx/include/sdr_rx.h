/* A web SDR as a second receiver beside the radio's own: a KiwiSDR, a
 * Web-888, or an UberSDR's Kiwi input -- all three speak KiwiSDR's protocol,
 * a WebSocket carrying text commands up and IMA-ADPCM audio down.
 *
 * The receiver follows the radio: its frequency, mode and passband, retuned
 * as the dial turns -- in CW the station on the dial heard at the receiver's
 * CW tone: 500 Hz on a KiwiSDR, unless its owner set another. Where it cannot
 * reach the dial -- a KiwiSDR's 30 MHz under a radio on 6 m -- it goes quiet
 * and says so, its session kept, until the dial is back within its range.
 * Its audio goes to audio_out's second source -- the right ear, the radio in
 * the left -- and its S-meter reading comes with it, each kind of receiver's
 * from its own reference. The session is kiwi_sess.h's, on the app path:
 * the ring kept at the live point, kept fuller while the network keeps
 * breaking the stream up, and every 30 s the log says how the audio came.
 *
 * The list of receivers is kept in NVS, up to SDR_MAX of them, and chosen on
 * the dial (a swipe down) or on the configuration page, which can test each.
 * On the kiwi firmware the same list is its receivers: one in use in the
 * left ear (components/kiwi_client), and this one, another of them, in the
 * right -- an antenna against another (sdr_rx_primary_cb).
 *
 * Their owners' limits are kept: a receiver that refused a login for its day
 * limit is marked through restarts (kiwi_mark.h) and logged in to again only
 * when the operator chooses it again, twice at most -- and so is one with
 * time limits that left a login unanswered, which may have been such a
 * refusal; one that ended the session for idling (time up), sent the knob
 * away (kicked) or will not talk to this address (refused) also waits for
 * that choice, through a crash too, and a wrong password, an address that is
 * no Kiwi and a KiwiSDR that lets no apps in wait for it or the list saved.
 * A knob whose flash cannot keep a mark logs in on its own only to receivers
 * without time limits, and on its own it tries one receiver at most six
 * times in ten minutes.
 *
 * A receiver behind a front -- the kiwisdr.com proxy, Cloudflare -- is
 * spoken to over TLS: its address an https:// link (443 unless it says), as
 * the page takes it whole. An http:// one that answers with a redirect to
 * https:// on its own host is followed at once, and keeps https from then
 * on (sdr_moved): in RAM at once, in flash at a quiet moment -- never while
 * a receiver's audio plays -- and under the key it had, so its marks and
 * holds stay its; a page that still shows it at its old address saves it as
 * it is now (sdr_same).
 */
#ifndef SDR_RX_H
#define SDR_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kiwi_sess.h"

#define SDR_MAX 4

typedef struct {
    char     name[24];      /* on the dial; "" = the receiver's own name */
    char     host[64];
    uint16_t port;
    uint16_t kport;         /* the port it is known by (kiwi_hp), where an https:// redirect
                               moved it from that one; 0: `port` */
    char     pass[32];      /* the receiver's user password; "" = none */
    char     ipl[32];       /* its time-limit exemption password; "" = none */
    bool     tls;           /* https://: its /status and its session over TLS */
} sdr_cfg_t;

/* Its key wherever the knob keeps something of it -- the marks, the holds,
 * the one in use: kiwi_hp of its host and the port it is known by. */
uint32_t sdr_hp(const sdr_cfg_t *c);

/* The session's view of it. */
static inline kiwi_addr_t sdr_addr(const sdr_cfg_t *c)
{
    return (kiwi_addr_t){ .host = c->host, .port = c->port, .pass = c->pass, .ipl = c->ipl, .tls = c->tls,
                          .kport = c->kport };
}

/* An address as the page takes it -- an https:// or http:// link whole, or
 * host:port, 8073 for a host alone -- into c's host, port and tls (kport 0,
 * a new address's). False: no host there. */
bool sdr_parse_addr(const char *in, sdr_cfg_t *c);

/* Whether `c`, a receiver at the address a page or the API gives, is the
 * list's `old`: at its address -- or at the one an https:// redirect moved
 * it from, which a page loaded before the move still shows. True: c then
 * keeps the move (its https:// port) and the key old is known by; its
 * passwords, where none are given, are old's to carry (the caller's). */
bool sdr_same(const sdr_cfg_t *old, sdr_cfg_t *c);

/* An http:// receiver's redirect to https:// on its own host, followed and
 * spoken (kiwi_sess.h): the one at this key speaks TLS on `tls_port` from
 * now on, under the same key. The list in RAM at once -- no session ended,
 * no list generation moved -- and in flash at a quiet moment: never while a
 * receiver's audio plays (either ear's on the kiwi firmware, the web SDR's
 * beside a radio, whose own receive audio may play on, as for its other
 * settings), nor on the air. Any task; quick, no flash. */
void sdr_moved(uint32_t hp, uint16_t tls_port);

/* The list, from NVS at boot (sdr_rx_init, or sdr_list_init alone where no
 * second receiver plays) and whenever it is saved: each save moves
 * sdr_list_gen. A marked receiver given a new time-limit password counts as
 * chosen again (sdr_rx_choose); the marks themselves outlive any save.
 * `sel`: the receiver listened to, by its place in the new list (-1 none) --
 * the one before, wherever the list put it: set with the list, so no task
 * ever reads the new list with the old place, nor with none meanwhile. It
 * is no choice (sdr_rx_select's), and a session on it ends all the same, to
 * start again from the list as it is now. */
esp_err_t sdr_rx_init(void);
esp_err_t sdr_list_init(void);
uint32_t  sdr_list_gen(void);
int       sdr_count(void);
bool      sdr_get(int i, sdr_cfg_t *out);
esp_err_t sdr_save(const sdr_cfg_t *list, int n, int sel);

/* Its name on the dial, at most cap - 1 bytes (15 fit the chooser): the
 * page's name; else the antenna its /status gave this boot ("RF.Guru " left
 * off); else host:port, or where that does not fit the start of the host --
 * or, its host shared with another receiver, the end, the port with it, so
 * four receivers on one address are told apart (kiwi_label). */
bool sdr_rx_label(int i, char *out, size_t cap);

/* Another receiver in the list has receiver i's host, on another port: only
 * the port tells them apart. */
bool sdr_host_shared(int i);

/* Who every receiver's owner sees of the knob in its list of listeners (SET
 * ident_user): the operator's callsign, say; "" for "VFO-Knob". One for all
 * of them -- the web SDR beside a radio's and the kiwi firmware's alike --
 * kept with the list (NVS "sdrid", loaded by sdr_list_init) and handed to
 * the session (kiwi_ident_set), which tells a receiver logged in at once.
 * Control characters are left out, and spaces at either end; at most
 * KIWI_IDENT_MAX - 1 bytes, a character cut at the end left out whole.
 * Saving writes flash: the page's Save, from the web server's task. */
void      sdr_ident(char *out, size_t cap);
esp_err_t sdr_ident_save(const char *who);

/* Listen to receiver `i`, or to none (-1). Kept across a restart, by its
 * address: a list that moved under it is followed. */
void sdr_rx_select(int i);
int  sdr_rx_selected(void);

/* The operator's act -- the dial's commit, the page's sel=, the API's sdr= --
 * which counts even when `i` is already chosen: it lets one attempt through
 * what held the receiver back (time up, kicked, no apps), and spends one try
 * of a day-limited one. sdr_rx_select is the plain setter, and sdr_save's
 * `sel` a list save's reshuffle: neither ever counts. False, nothing chosen,
 * for the primary's receiver (sdr_rx_primary_cb). */
bool sdr_rx_choose(int i);

/* The kiwi firmware's: its own receiver in the left ear, from this same
 * list, with this one in the right (components/kiwi_client). `uses` says
 * whether that one has the receiver at this address (kiwi_hp): `chosen`,
 * whether it is the one in use there; else whether it is that or the one
 * its session still has, on its way off it. The right ear never logs in to
 * it beside it: choosing the one in use there is refused, and one the left
 * ear has meanwhile -- a list saved under both, a session not over yet --
 * is let go of and waited out ("in the left ear"). Any task; quick. */
void sdr_rx_primary_cb(bool (*uses)(uint32_t hp, bool chosen));

/* Whether the receiver at this address is the one chosen here -- the left
 * ear refuses it -- and whether this one's session is on it, on its way
 * from its /status read before the login, or playing -- the left ear waits
 * for that to end before it logs in. */
bool sdr_rx_uses(uint32_t hp);
bool sdr_rx_in_session(uint32_t hp);

/* While this says true -- the kiwi firmware's left ear playing -- the choice
 * and the balance go to flash at a quiet moment, 30 s after the change at
 * most: a flash write holds the audio's interrupts up. Nor is this one's
 * session ending a quiet moment for a mark at rest (kiwi_mark_flush) then. */
void sdr_rx_quiet_cb(bool (*audible)(void));

/* Whether this one may be heard now, for the kiwi firmware's quiet moments:
 * its audio playing, unless its squelch -- or a dial out of its receiver's
 * reach -- has kept it silent a second, when what it let through before has
 * played. Any task; quick. */
bool sdr_rx_audible(void);

/* The mix, as audio_out takes it: -100 the radio alone, 0 the radio left and
 * the SDR right, +100 the SDR alone. Kept across a restart too. */
void   sdr_rx_set_balance(int8_t balance);
int8_t sdr_rx_balance(void);

/* The radio, as the receiver should follow it: Hz, the radio's mode name
 * ("usb", "cwr", "digu" ...) and its passband relative to the carrier -- on
 * the kiwi firmware the left ear's dial, its mode and passband the Kiwi's
 * own. Cheap: the receiver is retuned only when something changed. */
void sdr_rx_tune(int64_t hz, const char *mode, int32_t lo, int32_t hi);

/* The kiwi firmware's: `get` gives the left ear's AGC (KIWI_AGC_*), noise
 * filter (KIWI_NR_*) and squelch (0-100 %), which the right ear plays with
 * too, so one antenna is heard against another fairly. Asked at every pass
 * of the session, as the left ear's own session asks its settings: each
 * change sent as the left ear's is, the noise filter once it has rested half
 * a second. None, the receiver plays with its own defaults: the AGC at MED,
 * no noise filter, the squelch open -- the web SDR beside a radio. `get`:
 * from the session's task, quick. */
void sdr_rx_settings_cb(void (*get)(uint8_t *agc, uint8_t *nr, uint8_t *sq_pct));

typedef struct {
    int   sel;              /* -1 none */
    bool  streaming;        /* logged in, audio arriving -- and the dial within its reach */
    bool  trouble;          /* not reached, refused, busy, out of range ... */
    char  note[12];         /* the trouble in a word, for the dial: "day limit", "time up" ... */
    char  state[48];        /* "connecting", "streaming", "busy", "out of range", "moved to <host>" ... */
    char  name[24];         /* what the dial calls it */
    float smeter_dbm;
    /* Where the session's receiver tunes, once it has said, Hz (0 0 not
     * known): what "out of range" says it covers. */
    int64_t lo_hz, hi_hz;
} sdr_status_t;
void sdr_rx_status(sdr_status_t *out);

/* The configuration page's test of one receiver (kiwi_sess.h's kiwi_test):
 * its /status -- the one the last Test of it read, within a minute -- then a
 * login on the app path, and whether its owner lets apps listen (the knob,
 * without a waterfall, is one). No login to the receiver in use while its
 * session is on its way or playing ("in use": `in_use`, NULL for this one's
 * own -- the kiwi firmware passes either ear's), nor to a day-limited one
 * ("day limit", with its mark), nor to a KiwiSDR whose /status lets no apps
 * in ("no apps"): a test never costs a refusal it can spare. A refusal for
 * the day limit marks it, as a login left unanswered does where it may
 * count. A redirect to https:// that it follows is kept for the receiver at
 * that address in the list, as a session's is (sdr_moved). Run on a task of
 * its own, its stack in PSRAM with room for a TLS handshake, which the web
 * server's has not; blocks the caller for some 40 s at most (kiwi_test). The
 * result as JSON into `json`, which wants 1 KB -- an error there too when
 * no task could be made for it (ESP_ERR_NO_MEM); `tag` the log's (NULL:
 * "sdr"). The caller's task may touch flash. */
esp_err_t sdr_test(const sdr_cfg_t *c, bool (*in_use)(uint32_t hp), char *json, size_t cap, const char *tag);

#endif /* SDR_RX_H */
