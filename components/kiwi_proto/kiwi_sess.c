/* KiwiSDR's protocol on the wire. See kiwi_sess.h.
 *
 * Written from the protocol as kiwiclient and the servers' own sources speak
 * it, from what the web SDR beside a radio had learnt against a KiwiSDR 2
 * (v1.902) and a Web-888 -- and that one (components/sdr_rx) runs on it now,
 * as the kiwi firmware's receiver does:
 *
 *   GET /<ts>/SND HTTP/1.1 (Upgrade)  the app path: 101, or a status classed
 *   -> SET auth t=kiwi p=<pw> [ipl=<time-limit pw>]    ("#" for no pw with ipl)
 *   <- MSG rx_chans= chan_no_pwd= ... badp=0, or ip_limit=, too_busy= ...;
 *      a KiwiSDR 1.9 whose door for apps is shut says nothing at all
 *   -> SET ident_user=<who the owner sees> compression=1 squelch= agc= [nr ...]
 *   <- MSG center_freq= bandwidth= audio_rate= sample_rate= ...
 *   -> SET AR OK in=<rate> out=24000, SET mod=<m> low_cut= high_cut= freq=<kHz>
 *   <- SND: flags(1) seq(4 LE) S-meter(2 BE) data -- IMA-ADPCM with 0x10
 *   -> SET keepalive every 5 s; SET mod ... as the dial turns; SET
 *      inactivity_ack once someone has touched or turned the knob, a minute
 *      apart at most
 *
 * Before its login the receiver takes only keepalive, options and auth (it
 * kicks anything else), so the rest waits for badp=0; and every compressed
 * frame is decoded, even unheard, so the decoder never parts from the
 * receiver's encoder.
 *
 * In CW the receiver is told the carrier, as the Kiwi's own page tells it: the
 * centre of its CW passband below the signal, which is then heard at that
 * tone -- 500 Hz on a KiwiSDR (its 300..700). The receiver's configuration
 * (load_cfg, read whole or as it passes) says where it centres CW, and an
 * UberSDR's Kiwi input (port 8073 on its own address) centres it on the
 * carrier (-400..400), making the tone itself. That input also makes its
 * channel from the first SET mod with its own passband, and takes the one
 * asked for only from the next; an AGC or a squelch before it, it lets go
 * by: the tune, the AGC and the squelch go again once the audio flows.
 *
 * A carrier beyond where the receiver tunes is kept at its edge, which it
 * would play as if it were the dial. A caller that would rather hear
 * nothing there (kiwi_link_t's reach) has a dial out of reach not followed
 * and silence meanwhile, the session and its channel kept.
 *
 * Its frames come a whole one at a time, 171 ms of audio at 12 kHz, and the
 * network may hold them up and then hand them over at once. Into a ring
 * that sits full, each early frame would lose a piece, for minutes on end
 * (TerraBooster, 2026-10-03): so the ring is kept at its target (flow()),
 * the receiver's clock followed with the drift trim, and the frames a stall
 * held up are left out in one jump, whole ones, where the stall's silence
 * already is: the audio picks up at the live point. Where the network keeps
 * breaking the stream up, a short stall every few seconds, the target grows
 * until the ring rides them out, and eases back once it is calm.
 *
 * Over TLS (https://, wss://: a receiver behind the kiwisdr.com proxy or
 * Cloudflare) the protocol is the same, on 443 and with the Host the front
 * routes on; kiwi_tls.c does the TLS, on the socket connected here, which
 * is then non-blocking: every wait below is a select() as it was, after a
 * look at what TLS holds decrypted already. An http:// receiver that answers
 * with a redirect to https:// on its own host is spoken to there at once. */
#include "kiwi_sess.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "kiwi_mark.h"
#include "kiwi_tls.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#define BUF_BYTES     (16 * 1024)   /* frames up to this; bigger are let go by */
#define MSG_MAX       512           /* of one MSG, as text: the ones that count are short */
#define STATUS_BYTES  4096          /* a /status, read whole */
#define LOOP_MS       50
/* The login's answer, waited for whatever else happens meanwhile: one that
 * comes late may be a refusal the receiver has counted, and only one read
 * is one the knob can count too. A KiwiSDR whose door for apps is shut says
 * nothing at all: that is told by this long a silence. */
#define ANSWER_US     (10 * 1000000LL)
#define CONNECT_US    (5 * 1000000LL)   /* a connection, or the upgrade's answer */
#define SLICE_MS      100               /* ...asked in slices, so a switch is quick */
/* TLS's handshake: software ECDHE and the certificate's chain, 1.5-2 s on
 * the S3, a resumed one a round trip -- and this long at most of the front's
 * own time: the knob's computing, and its wait for other connections' steps
 * (kiwi_tls.h), not counted. */
#define HANDSHAKE_US  (10 * 1000000LL)
#define SEND_US       (5 * 1000000LL)   /* a full socket, over TLS: as SO_SNDTIMEO waits in the clear */
#define BW_WAIT_US    (2 * 1000000LL)   /* its bandwidth, before the first tune */
#define QUIET_US      (10 * 1000000LL)  /* nothing at all: gone */
#define KEEPALIVE_US  (5 * 1000000LL)
#define MOD_GAP_US    (50 * 1000LL)     /* the dial's newest, no oftener */
#define AGC_GAP_US    (200 * 1000LL)
#define SQ_GAP_US     (200 * 1000LL)
#define NR_REST_US    (500 * 1000LL)    /* the noise filter's choice rests first */
#define STREAMED_US   (10 * 1000000LL)  /* streamed this long: no day limit now */
#define ACK_GAP_US    (60 * 1000000LL)  /* someone listening, said no oftener */
/* An ip_limit this soon after the login is counted as the login's refusal:
 * the knob's count may run ahead of the receiver's, never behind it. */
#define LOGIN_SLACK_US (10 * 1000000LL)
#define FRAME_SANE    (1u << 20)        /* a frame claiming more is no Kiwi's */
#define TEST_AFTER_US (3 * 1000000LL)   /* a test, logged in: how long it listens on */
/* A test's way to its login -- the /status, a redirect, each address's
 * connection and its TLS, the upgrade -- this long at most: the web server
 * waits for the test. The login's answer is waited for as ever. */
#define TEST_BEFORE_US (25 * 1000000LL)
#define DEFAULT_BW    30000000LL        /* bandwidth not said: a KiwiSDR's */
#define JUMP_PCT      80                /* a backlog that would fill the ring past this: left out */
#define AVG_K         0.05f             /* the ring's level, smoothed frame by frame */
#define TRIM_GAIN     0.002f            /* the trim, per the level's distance from its target */
#define REPORT_US     (30 * 1000000LL)  /* how the audio came, said this often */
#define LIVE_CREEP    1000.0            /* the live point's reference, crept on 1 us a ms */
#define REFILL_US     (1000000LL)       /* after a stall's jump, the ring's refill: no other in it */
#define MORE_MS       LOOP_MS           /* a frame near the live point: the next, looked for this long */
#define USUAL_EBB     16.0              /* the usual lateness ebbs a 16th of a frame a frame: ~3 s */
/* The target the network sets (kiwi_link_t's target_max): a break this soon
 * after the last grows it; this long without one, it eases back, a step at a
 * time this far apart. */
#define AGAIN_US      (120 * 1000000LL)
#define CALM_US       (180 * 1000000LL)
#define EASE_US       (30 * 1000000LL)

struct kiwi_sess {
    uint8_t    buf[BUF_BYTES];
    int16_t    pcm[KIWI_PCM_MAX];
    int16_t    out[KIWI_OUT_MAX];
    kiwi_dsp_t dsp;
    char       msg[MSG_MAX];
};

kiwi_sess_t *kiwi_sess_new(void)
{
    return heap_caps_calloc(1, sizeof(kiwi_sess_t), MALLOC_CAP_SPIRAM);
}

/* ------------------------------------------------------------ who it is */

/* Who the receivers' owners see (kiwi_ident_set), and how often it was
 * set: a session logged in tells its receiver a new one. */
static portMUX_TYPE s_id_mux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static char s_ident[KIWI_IDENT_MAX];
static volatile uint32_t s_ident_gen;

void kiwi_ident_set(const char *who)
{
    portENTER_CRITICAL(&s_id_mux);
    strlcpy(s_ident, who ? who : "", sizeof s_ident);
    s_ident_gen++;
    portEXIT_CRITICAL(&s_id_mux);
}

void kiwi_ident(char *out, size_t cap)
{
    if (!out || !cap) return;
    portENTER_CRITICAL(&s_id_mux);
    strlcpy(out, s_ident, cap);
    portEXIT_CRITICAL(&s_id_mux);
}

/* "SET ident_user=..." as it stands now; the generation it is of. */
static uint32_t ident_cmd(char *out, size_t cap)
{
    char who[KIWI_IDENT_MAX];
    portENTER_CRITICAL(&s_id_mux);
    strlcpy(who, s_ident, sizeof who);
    const uint32_t g = s_ident_gen;
    portEXIT_CRITICAL(&s_id_mux);
    if (kiwi_ident_cmd(out, cap, who) < 0) kiwi_ident_cmd(out, cap, NULL);
    return g;
}

/* --------------------------------------------------------- the connection */

/* The receiver's addresses. A name can stand for several, and not all of
 * them need be the receiver -- kiwi.on4cdj.be has a second that resets -- so
 * each is tried in turn, as a browser would (CONFIG_LWIP_DNS_MAX_HOST_IP). */
typedef struct {
    struct sockaddr_in a[4];
    int n;
} addrs_t;

static kiwi_end_t resolve(const char *host, uint16_t port, addrs_t *out, const char *tag)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    char ps[8];
    snprintf(ps, sizeof ps, "%u", (unsigned)port);
    out->n = 0;
    if (getaddrinfo(host, ps, &hints, &ai) != 0 || !ai) {
        ESP_LOGW(tag, "%s: name not found", host);
        return KIWI_END_NOT_FOUND;
    }
    for (const struct addrinfo *p = ai; p && out->n < 4; p = p->ai_next)
        if (p->ai_family == AF_INET && p->ai_addrlen >= sizeof out->a[0])
            memcpy(&out->a[out->n++], p->ai_addr, sizeof out->a[0]);
    freeaddrinfo(ai);
    return out->n ? KIWI_END_NONE : KIWI_END_NOT_FOUND;
}

/* A connected socket, or -1 with `why` saying what stopped it -- WANT when
 * the caller let go of it meanwhile (`go_on`, asked every SLICE_MS: a
 * receiver that does not answer must not hold up a switch to another). */
static int tcp_connect(const struct sockaddr_in *sa, int timeout_ms, kiwi_end_t *why, const char *tag,
                       bool (*go_on)(void *), void *ctx)
{
    kiwi_end_t w = KIWI_END_NO_ANSWER;
    int fd = socket(AF_INET, SOCK_STREAM, 0), e = errno;
    if (fd < 0) { w = KIWI_END_NO_SOCKET; goto fail; }
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    const int r = connect(fd, (const struct sockaddr *)sa, sizeof *sa);
    e = errno;
    if (r != 0 && e != EINPROGRESS) {
        if (e == EHOSTUNREACH || e == ENETUNREACH) w = KIWI_END_NO_ROUTE;
        goto fail;
    }
    int sr = r == 0;
    for (int left = timeout_ms; !sr && left > 0; left -= SLICE_MS) {
        if (go_on && !go_on(ctx)) {
            close(fd);
            if (why) *why = KIWI_END_WANT;
            return -1;
        }
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(fd, &wf);
        struct timeval tv = { .tv_sec = 0, .tv_usec = (left < SLICE_MS ? left : SLICE_MS) * 1000 };
        sr = select(fd + 1, NULL, &wf, NULL, &tv);
        if (sr < 0) break;
    }
    if (sr <= 0) { e = sr < 0 ? errno : 0; goto fail; }
    socklen_t el = sizeof e;
    e = 0;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
    if (e) {
        if (e == EHOSTUNREACH || e == ENETUNREACH) w = KIWI_END_NO_ROUTE;
        goto fail;
    }
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    struct timeval io = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io, sizeof io);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io, sizeof io);
    return fd;
fail: {
        char ip[16];
        inet_ntoa_r(sa->sin_addr, ip, sizeof ip);
        ESP_LOGW(tag, "%s:%u: %s (errno %d)", ip, (unsigned)ntohs(sa->sin_port),
                 kiwi_end_note(w), e);
    }
    if (fd >= 0) close(fd);
    if (why) *why = w;
    return -1;
}

/* A connection to a receiver: its socket -- and over TLS its session, the
 * socket non-blocking under it. */
typedef struct {
    int         fd;
    kiwi_tls_t *tls;            /* NULL: in the clear */
} conn_t;
#define CONN_NONE { .fd = -1, .tls = NULL }

static void conn_close(conn_t *c)
{
    if (c->tls) kiwi_tls_free(c->tls);
    if (c->fd >= 0) close(c->fd);
    c->tls = NULL;
    c->fd = -1;
}

/* What TLS has decrypted and not handed out yet: the socket has it no more,
 * so select() would wait for the next record with this one unread. */
static bool conn_held(const conn_t *c) { return c->tls && kiwi_tls_pending(c->tls) > 0; }

/* Up to `us` for something to read: >0 there is, 0 not yet, <0 select failed. */
static int conn_wait(const conn_t *c, int64_t us)
{
    if (conn_held(c)) return 1;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(c->fd, &rf);
    struct timeval tv = { .tv_sec = (time_t)(us / 1000000), .tv_usec = (suseconds_t)(us % 1000000) };
    return select(c->fd + 1, &rf, NULL, NULL, &tv);
}

/* As recv(): bytes; 0 once the receiver has closed its side; -1 and errno --
 * EAGAIN, none yet (over TLS: always, with nothing decrypted to hand out). */
static int conn_recv(conn_t *c, void *b, size_t n, int flags)
{
    if (!c->tls) return recv(c->fd, b, n, flags);
    const int k = kiwi_tls_read(c->tls, b, n);
    if (k > 0) return k;
    if (k == -2) return 0;
    errno = k == 0 ? EAGAIN : EIO;
    return -1;
}

static bool conn_send(conn_t *c, const void *b, size_t n)
{
    const uint8_t *p = b;
    if (!c->tls) {
        while (n) {
            const int k = send(c->fd, p, n, 0);
            if (k <= 0) return false;
            p += k;
            n -= (size_t)k;
        }
        return true;
    }
    /* A full socket is waited for, as SO_SNDTIMEO waits in the clear, and
     * mbedTLS given the same bytes again -- once it can take them: writable,
     * or readable should it want to read first. Never on both: audio still
     * coming in would have the wait spin, the core kept from its idle task. */
    const int64_t until = esp_timer_get_time() + SEND_US;
    while (n) {
        bool wr = true;
        const int k = kiwi_tls_write(c->tls, p, n, &wr);
        if (k < 0) return false;
        if (k > 0) {
            p += k;
            n -= (size_t)k;
            continue;
        }
        const int64_t left = until - esp_timer_get_time();
        if (left <= 0) return false;
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(c->fd, &fs);
        struct timeval tv = { .tv_sec = 0, .tv_usec = left < SLICE_MS * 1000LL ? (long)left : SLICE_MS * 1000L };
        if (select(c->fd + 1, wr ? NULL : &fs, wr ? &fs : NULL, NULL, &tv) < 0) return false;
    }
    return true;
}

/* TLS on the connected socket, for the receiver at host:port: the handshake
 * a step at a time, and its waits a slice at a time, `go_on` asked in
 * between, HANDSHAKE_US of the front's time at most -- its heaviest step,
 * up to a second or so of software on the S3, is the longest it is not asked
 * (kiwi_tls.h). NONE; or why not: WANT, CERT (its certificate not valid for
 * that name), NO_ANSWER for the rest -- each said in the log, and how long a
 * handshake took, the knob's own computing and its turn apart. */
static kiwi_end_t tls_open(conn_t *c, const char *host, uint16_t port, const char *tag, bool (*go_on)(void *),
                           void *ctx)
{
    const int64_t t0 = esp_timer_get_time();
    c->tls = kiwi_tls_new(c->fd, host, kiwi_hp(host, port));
    if (!c->tls) {
        ESP_LOGE(tag, "%s:%u: no memory for TLS", host, (unsigned)port);
        return KIWI_END_NO_SOCKET;
    }
    for (;;) {
        bool wr = false;
        const int r = kiwi_tls_shake(c->tls, &wr);
        const int64_t own = kiwi_tls_work_us(c->tls) + kiwi_tls_turn_us(c->tls);
        if (r == 1) {
            ESP_LOGI(tag, "%s:%u: TLS in %.2f s, %.2f s of it computing, %.2f s waiting its turn, %s", host,
                     (unsigned)port, (double)(esp_timer_get_time() - t0) / 1e6,
                     (double)kiwi_tls_work_us(c->tls) / 1e6, (double)kiwi_tls_turn_us(c->tls) / 1e6,
                     kiwi_tls_suite(c->tls));
            return KIWI_END_NONE;
        }
        if (r < 0) {
            char why[160];
            kiwi_tls_why(c->tls, why, sizeof why);
            const bool cert = kiwi_tls_cert_refused(c->tls);
            ESP_LOGW(tag, "%s:%u: %s: %s", host, (unsigned)port,
                     cert ? "its certificate is not valid -- not spoken to" : "TLS failed", why);
            return cert ? KIWI_END_CERT : KIWI_END_NO_ANSWER;
        }
        if (go_on && !go_on(ctx)) return KIWI_END_WANT;
        const int64_t left = t0 + own + HANDSHAKE_US - esp_timer_get_time();
        if (left <= 0) {
            ESP_LOGW(tag, "%s:%u: TLS: no answer to the handshake", host, (unsigned)port);
            return KIWI_END_NO_ANSWER;
        }
        if (r == 2) continue;                       /* a step done, or another's waited for: on */
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(c->fd, &fs);
        struct timeval tv = { .tv_sec = 0, .tv_usec = left < SLICE_MS * 1000LL ? (long)left : SLICE_MS * 1000L };
        if (select(c->fd + 1, wr ? NULL : &fs, wr ? &fs : NULL, NULL, &tv) < 0) return KIWI_END_NO_ANSWER;
    }
}

/* A connection to host:port, over TLS or not, from the first of its
 * addresses that takes one -- a TLS handshake included: NONE, and `c`; or
 * why none did. A certificate refused is the name's, whatever the address:
 * no other is tried. `*reached`: a connection was made at all. */
static kiwi_end_t conn_open(conn_t *c, const char *host, uint16_t port, bool tls, const char *tag, bool *reached,
                            bool (*go_on)(void *), void *ctx)
{
    addrs_t ad;
    kiwi_end_t why = resolve(host, port, &ad, tag);
    if (why != KIWI_END_NONE) return why;
    why = KIWI_END_NO_ANSWER;
    for (int i = 0; i < ad.n; i++) {
        kiwi_end_t w;
        c->fd = tcp_connect(&ad.a[i], ad.n > 1 ? 3000 : 5000, &w, tag, go_on, ctx);
        if (c->fd >= 0) {
            if (reached) *reached = true;
            w = tls ? tls_open(c, host, port, tag, go_on, ctx) : KIWI_END_NONE;
            if (w == KIWI_END_NONE) return w;
            conn_close(c);
        }
        why = w;
        if (w == KIWI_END_WANT || w == KIWI_END_CERT) break;
    }
    return why;
}

/* The upgrade, on the app path: the HTTP status it answers (101 is in), 0
 * for none within CONNECT_US, -1 when the caller let go of it meanwhile. The
 * headers are read past, to the blank line where the frames begin -- a
 * redirect's Location kept in `loc`. */
static int ws_open(conn_t *c, const char *host, uint16_t port, bool tls, bool (*go_on)(void *), void *ctx,
                   char *loc, size_t lc)
{
    uint8_t key[16];
    esp_fill_random(key, sizeof key);
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char k64[25];
    int o = 0;
    for (int i = 0; i < 16; i += 3) {
        const uint32_t v = (uint32_t)key[i] << 16 | (i + 1 < 16 ? key[i + 1] << 8 : 0) |
                           (i + 2 < 16 ? key[i + 2] : 0);
        k64[o++] = B64[v >> 18 & 63];
        k64[o++] = B64[v >> 12 & 63];
        k64[o++] = i + 1 < 16 ? B64[v >> 6 & 63] : '=';
        k64[o++] = i + 2 < 16 ? B64[v & 63] : '=';
    }
    k64[o] = 0;
    /* 32 bits of timestamp, as kiwiclient: a real Kiwi takes no more. Ten
     * digits, always: an UberSDR's Kiwi input takes the app path only so. */
    const uint32_t ts = 1000000000u + esp_random() % 3000000000u;
    char hh[80], req[320];
    kiwi_host_hdr(hh, sizeof hh, host, port, tls);
    const int n = snprintf(req, sizeof req,
        "GET /%lu/SND HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
        "User-Agent: VFO-Knob\r\n\r\n", (unsigned long)ts, hh, k64);
    if (n <= 0 || n >= (int)sizeof req || !conn_send(c, req, (size_t)n)) return 0;
    if (loc && lc) loc[0] = 0;
    /* What has come, a byte at a time and no further than the blank line, a
     * line at a time: the status, and a redirect's Location; then wait for
     * more a slice at a time. */
    char line[160];
    size_t got = 0;
    int code = -2;                              /* -2: no status line yet */
    const int64_t until = esp_timer_get_time() + CONNECT_US;
    for (int i = 0; i < 4096;) {
        char ch;
        const int k = conn_recv(c, &ch, 1, MSG_DONTWAIT);
        if (k == 1) {
            i++;
            if (ch != '\n') {
                if (got < sizeof line - 1) line[got++] = ch;
                continue;
            }
            while (got && line[got - 1] == '\r') got--;
            line[got] = 0;
            got = 0;
            if (code == -2) {
                code = !strncmp(line, "HTTP/1.", 7) && line[8] == ' ' ? atoi(line + 9) : 0;
            } else if (!line[0]) {
                return code;                    /* the blank line: the frames from here */
            } else if (loc && lc && !strncasecmp(line, "location:", 9)) {
                const char *v = line + 9;
                while (*v == ' ' || *v == '\t') v++;
                size_t vn = strlen(v);
                if (vn >= lc) vn = lc - 1;
                memcpy(loc, v, vn);
                loc[vn] = 0;
            }
            continue;
        }
        if (k == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return 0;
        if (go_on && !go_on(ctx)) return -1;
        const int64_t left = until - esp_timer_get_time();
        if (left <= 0) return 0;
        if (conn_wait(c, left < SLICE_MS * 1000LL ? left : SLICE_MS * 1000LL) < 0) return 0;
    }
    return 0;
}

static bool ws_send(conn_t *c, uint8_t op, const uint8_t *t, size_t n)
{
    if (n > 256) return false;               /* the longest is the login */
    uint8_t f[256 + 8];
    size_t h = 0;
    f[h++] = 0x80 | op;
    if (n < 126) f[h++] = 0x80 | (uint8_t)n;
    else { f[h++] = 0x80 | 126; f[h++] = (uint8_t)(n >> 8); f[h++] = (uint8_t)n; }
    uint8_t m[4];
    esp_fill_random(m, 4);
    memcpy(f + h, m, 4);
    h += 4;
    for (size_t i = 0; i < n; i++) f[h + i] = t[i] ^ m[i & 3];
    return conn_send(c, f, h + n);
}

static bool ws_text(conn_t *c, const char *t, const kiwi_link_t *l)
{
    if (l && l->counts) l->counts->sent++;
    return ws_send(c, 0x1, (const uint8_t *)t, strlen(t));
}

/* Frames as they come in: complete ones from the buffer, one too big for it
 * let go by as it passes. */
typedef struct {
    conn_t  *c;
    uint8_t *b;
    size_t   have;
    uint64_t skip;          /* bytes of a frame too big for b, still to pass */
    size_t   passed;        /* ...and those of it in b[0..passed) just now */
    size_t   head;          /* ...the first of them its header's, the first time; 0 after */
} rd_t;

/* 1 and the frame (the caller drops `total` after it), 0 when none is all
 * in yet, -1 for one no Kiwi would send. */
static int rd_frame(rd_t *r, uint8_t *op, uint8_t **p, size_t *n, size_t *total, bool *skipped)
{
    *skipped = false;
    if (r->skip || r->have < 2) return 0;
    uint8_t *b = r->b;
    uint64_t len = b[1] & 0x7F;
    size_t hl = 2;
    if (len == 126) {
        if (r->have < 4) return 0;
        len = (uint64_t)b[2] << 8 | b[3];
        hl = 4;
    } else if (len == 127) {
        if (r->have < 10) return 0;
        len = 0;
        for (int i = 2; i < 10; i++) len = len << 8 | b[i];
        hl = 10;
    }
    if (len > FRAME_SANE) return -1;
    const bool masked = b[1] & 0x80;           /* never from a server, but undone if so */
    if (masked) hl += 4;
    if (hl + len > BUF_BYTES) {                /* too big: let it go by */
        r->skip = hl + len - r->have;
        r->passed = r->have;
        r->head = hl < r->have ? hl : r->have;
        r->have = 0;
        *skipped = true;
        return 0;
    }
    if (r->have < hl + len) return 0;
    if (masked)
        for (size_t i = 0; i < len; i++) b[hl + i] ^= b[hl - 4 + (i & 3)];
    *op = b[0] & 0x0F;
    *p = b + hl;
    *n = (size_t)len;
    *total = hl + (size_t)len;
    return 1;
}

/* Whether more of the stream is here, past the frame of `total` bytes in
 * front: in the buffer, or in the socket within `ms`. */
static bool rd_more(const rd_t *r, size_t total, int ms)
{
    if (r->have > total) return true;
    return conn_wait(r->c, ms * 1000LL) > 0;
}

static void rd_drop(rd_t *r, size_t total)
{
    memmove(r->b, r->b + total, r->have - total);
    r->have -= total;
}

/* recv()'s end of the stream: -2 the receiver closed or reset its end --
 * after everything it sent, which is read first -- -1 anything else (a
 * timeout, the knob's own network gone); 0 nothing after all -- what TLS
 * read was not a whole record yet. */
static int rd_end(int k)
{
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return k == 0 || errno == ECONNRESET ? -2 : -1;
}

/* Waits up to `ms` for more: 1 had some, 0 none yet, -2 the receiver closed
 * its end, -1 failed. Over a frame being let go by, it reads no further than
 * its end. */
static int rd_fill(rd_t *r, int ms)
{
    const int sr = conn_wait(r->c, ms * 1000LL);
    if (sr < 0) return -1;
    if (sr == 0) return 0;
    if (r->skip) {
        const size_t want = r->skip < BUF_BYTES ? (size_t)r->skip : BUF_BYTES;
        const int k = conn_recv(r->c, r->b, want, 0);
        if (k <= 0) return rd_end(k);
        r->skip -= (uint64_t)k;
        r->passed = (size_t)k;
        r->head = 0;
        return 1;
    }
    if (r->have >= BUF_BYTES) return 1;        /* a frame in it waits to be taken */
    const int k = conn_recv(r->c, r->b + r->have, BUF_BYTES - r->have, 0);
    if (k <= 0) return rd_end(k);
    r->have += (size_t)k;
    return 1;
}

/* Connected and upgraded on the app path, from the first of its addresses
 * that will: true and `c`, or false and why -- an HTTP answer counts for
 * more than an address that did not answer at all. `*reached`: a connection
 * was made, whatever came of it. An http:// receiver's redirect to https://
 * on its own host is followed at once, once (`mv->tls_port`, where TLS was
 * spoken -- the caller keeps it); any other, or one over TLS, is not:
 * MOVED, `mv->to` saying where. */
static bool open_app(conn_t *c, const kiwi_addr_t *a, kiwi_moved_t *mv, const char *tag, kiwi_end_t *why,
                     bool *reached, bool (*go_on)(void *), void *ctx)
{
    const char *host = a->host;
    uint16_t port = a->port;
    bool tls = a->tls;
    *reached = false;
    memset(mv, 0, sizeof *mv);
    for (int hop = 0; hop < 2; hop++) {
        addrs_t ad;
        *why = resolve(host, port, &ad, tag);
        if (*why != KIWI_END_NONE) return false;
        bool http = false;
        uint16_t to = 0;
        *why = KIWI_END_NO_ANSWER;
        for (int i = 0; i < ad.n && !to; i++) {
            kiwi_end_t w;
            c->fd = tcp_connect(&ad.a[i], ad.n > 1 ? 3000 : 5000, &w, tag, go_on, ctx);
            if (c->fd >= 0) {
                *reached = true;
                w = tls ? tls_open(c, host, port, tag, go_on, ctx) : KIWI_END_NONE;
                /* TLS spoken where the redirect sent it: kept from now. */
                if (w == KIWI_END_NONE && hop) mv->tls_port = port;
            }
            if (c->fd >= 0 && w == KIWI_END_NONE) {
                char loc[128];
                const int code = ws_open(c, host, port, tls, go_on, ctx, loc, sizeof loc);
                const kiwi_end_t h = code < 0 ? KIWI_END_WANT : kiwi_http(code);
                if (h == KIWI_END_NONE) return true;
                conn_close(c);
                if (h == KIWI_END_WANT) { *why = h; return false; }
                if (kiwi_redirect_code(code) && !hop && kiwi_redirect(loc, host, tls, &to, mv->to, sizeof mv->to)) {
                    ESP_LOGI(tag, "%s:%u: HTTP %d to https://, port %u: followed", host, (unsigned)port, code,
                             (unsigned)to);
                    continue;
                }
                if (h == KIWI_END_MOVED) {
                    kiwi_redirect(loc, host, true, NULL, mv->to, sizeof mv->to);
                    ESP_LOGW(tag, "%s:%u: HTTP %d -- moved to %s", host, (unsigned)port, code,
                             mv->to[0] ? mv->to : "?");
                } else if (code > 0) {
                    ESP_LOGW(tag, "%s:%u: HTTP %d -- %s", host, (unsigned)port, code, kiwi_end_note(h));
                } else {
                    ESP_LOGW(tag, "%s:%u: no answer to the upgrade", host, (unsigned)port);
                }
                if (code > 0 || !http) *why = h;
                http |= code > 0;
            } else {
                conn_close(c);
                if (w == KIWI_END_WANT || w == KIWI_END_CERT) { *why = w; return false; }
                if (!http) *why = w;
            }
            if (go_on && !go_on(ctx)) {
                *why = KIWI_END_WANT;
                return false;
            }
        }
        if (!to) return false;
        port = to;
        tls = true;
    }
    return false;
}

static void auth_cmd(char *out, size_t cap, const kiwi_addr_t *a)
{
    const bool pw = a->pass && a->pass[0], ipl = a->ipl && a->ipl[0];
    snprintf(out, cap, "SET auth t=kiwi p=%s%s%s", pw ? a->pass : ipl ? "#" : "", ipl ? " ipl=" : "",
             ipl ? a->ipl : "");
}

/* ------------------------------------------------------------ the session */

/* What was last sent of each setting, and when. */
typedef struct {
    kiwi_tune_t t;
    bool        tuned;                  /* false: the tune goes again */
    bool        out;                    /* the dial out of the receiver's reach, as last said */
    uint32_t    ident_gen;              /* who the owner sees, as of kiwi_ident_set's */
    uint8_t     agc, nr, sq;
    bool        agc_cw, sq_nbfm;
    int64_t     t_mod, t_agc, t_sq;
    uint8_t     nr_seen;                /* the noise filter's choice, and since when */
    int64_t     t_nr_seen;
} sent_t;

static bool is_cw(const char *m)
{
    const char *k = kiwi_mode_of(m);
    return !strcmp(k, "cw") || !strcmp(k, "cwn");
}

static bool is_nbfm(const char *m)
{
    const char *k = kiwi_mode_of(m);
    return !strcmp(k, "nbfm") || !strcmp(k, "nnfm");
}

static bool send_nr(conn_t *c, uint8_t nr, const kiwi_link_t *l)
{
    char cmd[64];
    for (int i = 0; kiwi_nr_cmd(cmd, sizeof cmd, nr, i); i++)
        if (!ws_text(c, cmd, l)) return false;
    return true;
}

/* What the knob asks for now, as far as the receiver has not been told: the
 * tune, as often as the pacing lets it -- while the receiver can reach the
 * dial; the AGC and the squelch, again when a mode changes what they mean or
 * the receiver let them go by; the noise filter once its choice has rested
 * half a second. False: the socket failed. */
static bool follow(conn_t *cn, const kiwi_link_t *l, sent_t *z, const kiwi_said_t *said, double rate,
                   int64_t now, const char *tag)
{
    kiwi_ctl_t c;
    l->ctl(l->ctx, &c);
    char cmd[128];
    const int64_t bw = said->bw_hz > 0 ? said->bw_hz : DEFAULT_BW;
    /* A dial out of the receiver's reach (the link's reach): said once, and
     * not followed until it is back -- no tune at every turn of it. The
     * receiver keeps the tune it has -- or, never tuned yet, goes to its
     * edge nearest the dial -- and the AGC and the squelch keep to its mode. */
    const bool out = l->reach && !kiwi_tune_reaches(&c.t, said->offset_khz, bw);
    if (out != z->out) {
        z->out = out;
        l->reach(l->ctx, out);
    }
    const kiwi_tune_t t = out && z->t_mod ? z->t : c.t;
    const bool cw = is_cw(t.mode), nbfm = is_nbfm(t.mode);
    const bool moved = t.hz != z->t.hz || t.lo != z->t.lo || t.hi != z->t.hi || strcmp(t.mode, z->t.mode);
    /* Not told yet, or told what it now has otherwise -- a rate, an offset,
     * where it centres CW, a channel made with a passband of its own: at
     * once, as it plays its own frequency meanwhile. The dial, paced. */
    if (!z->tuned || (moved && now - z->t_mod >= MOD_GAP_US)) {
        if (kiwi_tune_cmd(cmd, sizeof cmd, &t, said->offset_khz, bw, rate, said->cw_hz) > 0) {
            if (!ws_text(cn, cmd, l)) return false;
            ESP_LOGD(tag, "%s", cmd);
        }
        z->t = t;
        z->tuned = true;
        z->t_mod = now;
    }
    if ((c.agc != z->agc || cw != z->agc_cw) && now - z->t_agc >= AGC_GAP_US) {
        if (kiwi_agc_cmd(cmd, sizeof cmd, c.agc, cw) > 0 && !ws_text(cn, cmd, l)) return false;
        z->agc = c.agc;
        z->agc_cw = cw;
        z->t_agc = now;
    }
    if ((c.sq_pct != z->sq || nbfm != z->sq_nbfm) && now - z->t_sq >= SQ_GAP_US) {
        if (kiwi_squelch_cmd(cmd, sizeof cmd, c.sq_pct, nbfm) > 0 && !ws_text(cn, cmd, l)) return false;
        z->sq = c.sq_pct;
        z->sq_nbfm = nbfm;
        z->t_sq = now;
    }
    if (c.nr != z->nr_seen) {
        z->nr_seen = c.nr;
        z->t_nr_seen = now;
    }
    if (z->nr != z->nr_seen && now - z->t_nr_seen >= NR_REST_US) {
        if (!send_nr(cn, z->nr_seen, l)) return false;
        z->nr = z->nr_seen;
    }
    /* Who the owner sees, given anew on the page: told at once, as its own
     * page tells it when its listener types another name. */
    if (z->ident_gen != s_ident_gen) {
        z->ident_gen = ident_cmd(cmd, sizeof cmd);
        if (!ws_text(cn, cmd, l)) return false;
        ESP_LOGI(tag, "%s", cmd);
    }
    return true;
}

/* The audio's way into the ring, frame by frame: how it came, and the ring
 * kept at the live point. */
enum { JUMP_NONE, JUMP_STALE, JUMP_FULL };
typedef struct {
    float    avg;                       /* the ring's level, smoothed: samples */
    int      fed;                       /* frames played since the level was taken afresh */
    uint8_t  jump;                      /* JUMP_*: frames being left out, and why */
    uint32_t left;                      /* ...the samples left out so far */
    uint32_t jump_n;                    /* ...the frames */
    int64_t  t_jump;                    /* ...since when (or when the last one ended) */
    bool     silent;                    /* ...it began with the ring run dry */
    bool     refill;                    /* a stall's jump over, the ring filling again */
    bool     any;
    uint32_t seq;                       /* the last frame's sequence number */
    int64_t  t_last, t_rep;             /* when it came; when the last report went */
    /* The live point: each frame's arrival against its place in the stream
     * (`pos`, the lengths of the frames before it, the missing ones too).
     * The frame that came soonest for its place is the reference, crept
     * forward LIVE_CREEP: a receiver whose clock runs a little slow is
     * followed. A frame's lateness is how much later it came than that. */
    double   pos_us, period_us;
    double   ref_us;                    /* the reference: arrival less place */
    int64_t  t_ref;                     /* ...when it came */
    /* The lateness its frames come with as a rule -- their unevenness, two
     * or three at a time, whatever the way does -- the most of late, ebbing:
     * only what comes later still is a stall's. A stall's jump teaches it
     * too: the lateness its first frame came with, two frames and a half at
     * most, so a way that hands the frames over three at a time, steadily,
     * costs one frame left out, not one in every three -- and a real stall
     * leaves the next one, seconds on, seen as it should be. */
    double   usual_us, first_us;
    /* The target the ring is kept at now: the link's own, grown while the
     * stream keeps breaking up (its target_max); the last break, and the
     * last step back down. `roof`: what a stall's lateness and the room a
     * backlog may fill over the target are reckoned from -- the target, or,
     * while the ring comes down after an eased one, its level, so that easing
     * never cuts. */
    size_t   tgt, roof;
    int64_t  t_brk, t_ease;
    uint32_t ur;                        /* the ring's underruns, as the last frame found them */
    kiwi_report_t rep;                  /* these 30 s */
} flow_t;

static unsigned long ms_of(size_t n) { return (unsigned long)((uint64_t)n * 1000 / KIWI_OUT_HZ); }

/* Where the ring is kept now: the target -- or, while the ring comes down
 * after an eased one, its level as the trim brings it after (the roof). A
 * frame no later than that level rides out is no stall's, as at the target:
 * easing never cuts. A ring run dry has no such level left: a stall's
 * backlog is the target's to judge then, as ever (flow()). */
static size_t flow_top(const flow_t *w) { return w->roof > w->tgt ? w->roof : w->tgt; }

/* A break: the ring ran dry since the last frame, its playing stopped -- a
 * stall it could not ride out -- or a stall's backlog about to be cut short
 * with sound still in the ring. Counted; and one within AGAIN_US of the
 * last, the network breaking the stream up, grows the target (the link's
 * target_max): to hold a stall as late as this frame came, with a quarter
 * frame to spare -- half a frame more at least. The ring then waits for that
 * much before it plays again. True when it grew. */
static bool flow_break(flow_t *w, const kiwi_link_t *l, double late_n, size_t len, int64_t now, const char *tag)
{
    const bool again = w->t_brk && now - w->t_brk < AGAIN_US;
    w->t_brk = now;
    w->rep.breaks++;
    if (!again || w->tgt >= l->target_max) return false;
    size_t want = late_n > 0 ? (size_t)late_n + len * 3 / 4 : 0;
    if (want < w->tgt + len / 2) want = w->tgt + len / 2;
    if (want > l->target_max) want = l->target_max;
    w->tgt = want;
    if (l->preroll) l->preroll(l->ctx, want);
    ESP_LOGW(tag, "the stream breaks up again and again: the ring kept at %lu ms from now", ms_of(want));
    return true;
}

/* Calm a while: the target back down half a frame every EASE_US, to the
 * link's own -- the trim brings the ring's level after it, a hair faster,
 * never a cut: what a stall is told by, and the room over the target, come
 * down with the level, not with the target (flow()'s roof, flow_top()). */
static void flow_ease(flow_t *w, const kiwi_link_t *l, size_t len, int64_t now, const char *tag)
{
    if (w->tgt <= l->target || now - w->t_brk < CALM_US || now - w->t_ease < EASE_US) return;
    w->tgt = w->tgt > l->target + len / 2 ? w->tgt - len / 2 : l->target;
    w->t_ease = now;
    if (l->preroll) l->preroll(l->ctx, w->tgt);
    ESP_LOGI(tag, "calm a while: the ring kept at %lu ms from now", ms_of(w->tgt));
}

static void jump_end(flow_t *w, int64_t now, const char *tag)
{
    const unsigned long ms = (unsigned long)((uint64_t)w->left * 1000 / KIWI_OUT_HZ);
    if (w->jump == JUMP_STALE)
        ESP_LOGW(tag, "a stall's backlog: %lu ms left out in one jump%s, on from the live point", ms,
                 w->silent ? ", in its silence" : "");
    else
        ESP_LOGW(tag, "the ring full: %lu ms left out in one jump, back to its target", ms);
    w->refill = w->jump == JUMP_STALE;
    w->jump = JUMP_NONE;
    w->t_jump = now;
    w->rep.jumps++;
    w->rep.left_ms += (uint32_t)ms;
    w->fed = 0;
}

/* An SND frame in: its arrival counted, and whether its audio is played.
 * `len` is that audio at 24 kHz, `period_us` how long it lasts.
 *
 * A stall the ring could not ride out leaves it dry, and the frames held
 * up meanwhile then come at once, all of them late. Played, they would keep
 * the audio behind by the whole stall. So a frame stale beyond what the
 * ring is for -- later than the stream's frames come as a rule by over half
 * a frame, and its lateness and the ring's level together more than a
 * quarter frame over where the ring is kept (flow_top(): the target, or the
 * level an eased one's ring is still coming down from) -- starts a jump: it
 * and those after it are decoded and not played, oldest first, until one
 * comes near the live point -- and one near it that could not start the
 * ring by itself, with nothing more come yet, is let go as well: it would
 * only wait in the ring for the next, aging. The frames left out are those
 * the silence already stands for, so the stall is the only break: one cut,
 * at the frame where the audio stopped. They are left out only while they
 * come at least twice as fast as they play -- each one costs less silence
 * than the lag it takes away (`r`: the frames still to come, looked into).
 * A backlog handed over more slowly is played as it comes, the live point
 * taken from it (a receiver's own time that moved is followed alike). Nor
 * does another such jump begin while the ring fills again after one.
 *
 * A frame that would fill the ring past JUMP_PCT all the same -- such a
 * slow backlog -- starts a jump of its own, down to the target: a cut each
 * time the ring comes up there again, rather than a piece lost at every
 * frame while the ring sits full.
 *
 * Where the link lets the target grow (target_max), a stall the ring could
 * not ride out is a break, and breaks that repeat grow it (flow_break()):
 * a stall's backlog the grown target holds is then played, not left out,
 * and the next such stall is ridden out. Calm a while, it eases back
 * (flow_ease()).
 *
 * Otherwise the trim follows the ring's level: above its target the audio
 * comes out a hair shorter, below it a hair longer. */
static bool flow(flow_t *w, const kiwi_link_t *l, kiwi_dsp_t *d, const kiwi_snd_t *f, size_t len,
                 double period_us, int64_t now, bool play, const rd_t *r, size_t total, const char *tag)
{
    /* How it came: the longest wait for a frame, and its number -- one
     * missing is a frame the receiver let go; one late with none missing,
     * the frames the network held up behind it. */
    const int64_t p = (int64_t)period_us;
    int32_t ahead = 0;
    if (w->any) {
        const int64_t gap = now - w->t_last;
        if (gap / 1000 > w->rep.gap_ms) w->rep.gap_ms = (uint32_t)(gap / 1000);
        ahead = (int32_t)(f->seq - w->seq);
        if (ahead > 1)
            w->rep.lost += (uint32_t)(ahead - 1);
        else if (ahead == 1 && p > 0 && gap > 2 * p)
            w->rep.held += (uint32_t)((gap + p / 2) / p - 1);
    }
    /* Its lateness (none for a frame with no audio). The reference starts
     * afresh with the first frame, a number gone back, a frame of another
     * length (another rate). */
    double e = 0, late_us = 0;
    if (p > 0) {
        if (ahead < 1 || period_us != w->period_us) {
            w->pos_us = 0;
            w->ref_us = (double)now;
            w->t_ref = now;
            w->usual_us = 0;
        } else {
            w->pos_us += ahead * period_us;
        }
        w->period_us = period_us;
        e = (double)now - w->pos_us;
        double ref = w->ref_us + (double)(now - w->t_ref) / LIVE_CREEP;
        if (e < ref) {
            w->ref_us = ref = e;
            w->t_ref = now;
        }
        late_us = e - ref;
    }
    w->any = true;
    w->seq = f->seq;
    w->t_last = now;
    w->rep.frames++;
    if (!play || !l->queued || !len || !l->room) return play;

    const size_t q = l->queued(l->ctx);
    const bool dry = q < len / 8;
    /* Run dry while it came down after an eased target: a stall all the
     * same, its backlog left out down to the target -- not to the level the
     * ring had, or the frames after the jump, judged by the target, would be
     * a second cut. */
    if (dry && w->roof > w->tgt) w->roof = w->tgt;
    const double over_us = late_us - w->usual_us;   /* later than its frames come as a rule */
    const double late_n = late_us * KIWI_OUT_HZ / 1e6;
    bool stale = over_us > period_us / 2 && (double)q + late_n > (double)(flow_top(w) + len / 4);
    bool backlog = false;
    if (w->refill && (q >= w->tgt || now - w->t_jump > REFILL_US)) w->refill = false;
    /* A break -- one heard: the ring's playing stopped since the last frame,
     * not merely ran low -- and if it grew the target, this frame as the
     * grown one sees it. */
    const uint32_t ur = l->underruns ? l->underruns(l->ctx) : w->ur;
    const bool ran_dry = ur != w->ur;
    w->ur = ur;
    const bool broke = !w->jump && (ran_dry || (stale && !dry && !w->refill));
    if (broke && flow_break(w, l, late_n, len, now, tag))
        stale = over_us > period_us / 2 && (double)q + late_n > (double)(flow_top(w) + len / 4);
    flow_ease(w, l, len, now, tag);
    if (!w->jump && stale && !w->refill) {
        w->jump = JUMP_STALE;
        w->left = w->jump_n = 0;
        w->t_jump = now;
        w->silent = dry;
        w->first_us = late_us;
    }
    if (w->jump == JUMP_STALE) {
        const bool fast = !w->jump_n || (double)w->jump_n * period_us >= 2.0 * (double)(now - w->t_jump);
        const bool aging = !stale && over_us > period_us * 3 / 4 && q + len < w->tgt &&
                           !rd_more(r, total, MORE_MS);
        if ((stale || aging) && fast) {
            w->left += (uint32_t)len;
            w->jump_n++;
            return false;
        }
        jump_end(w, now, tag);
        backlog = true;
        const double taught = w->first_us < 2.5 * period_us ? w->first_us : 2.5 * period_us;
        if (taught > w->usual_us) w->usual_us = taught;
        /* Stale still, but coming at the stream's pace: as near the live
         * point as this stream gets now. */
        if (stale) {
            w->ref_us = e;
            w->t_ref = now;
        }
    }
    /* The ring's room over the target, a grown one's as the link's own: the
     * ring holds as much more as the target may grow. Reckoned from the
     * roof: the target -- or, the target eased down, the ring's level as the
     * trim brings it after, down and never back up: a backlog the network
     * hands over slowly is cut back as ever, and easing cuts nothing. */
    const size_t lvl = w->fed >= 2 ? (size_t)w->avg : w->tgt;
    if (w->roof > lvl) w->roof = lvl;
    if (w->roof < w->tgt) w->roof = w->tgt;
    const size_t grown = w->roof - l->target;
    size_t hi = l->room * JUMP_PCT / 100 + grown;
    if (hi < w->tgt + len) hi = w->tgt + len;             /* a frame longer than the room between */
    if (hi > l->room + grown) hi = l->room + grown;
    if (!w->jump && q + len > hi) {
        w->jump = JUMP_FULL;
        w->left = 0;
    }
    if (w->jump == JUMP_FULL) {
        if (q + len / 2 > w->tgt) {
            w->left += (uint32_t)len;
            return false;
        }
        jump_end(w, now, tag);
    }
    /* A frame played as the stream comes -- not a backlog's -- tells how
     * late its frames come as a rule; that ebbs a 16th of a frame a frame. */
    if (!backlog) {
        w->usual_us -= period_us / USUAL_EBB;
        if (!stale && late_us > w->usual_us) w->usual_us = late_us;
        if (w->usual_us < 0) w->usual_us = 0;
    }
    /* A ring run dry -- a gap, or nothing played a while -- fills again
     * through the pre-roll: the level is taken afresh, not averaged with
     * what it was, and the trim kept meanwhile. */
    if (dry) w->fed = 0;
    const float level = (float)(q + len / 2), target = (float)w->tgt;
    if (w->fed < 2) {
        w->avg = target;
    } else {
        w->avg += AVG_K * (level - w->avg);
        if (l->trim) {
            /* As stiff at a grown target as at the link's own; and the ring
             * coming down after an eased one (its roof over the target), as
             * fast as the trim goes, until it is near: minutes, not ten. */
            float t = TRIM_GAIN * (w->avg - target) / (float)l->target;
            if (w->roof > w->tgt && w->avg > target + (float)(len / 4)) t = 0.002f;
            if (t > 0.002f) t = 0.002f;
            if (t < -0.002f) t = -0.002f;
            kiwi_dsp_trim(d, 1.0f + t);
        }
    }
    w->fed++;
    return true;
}

/* Logged in: who the owner sees, the audio compressed, the squelch and the
 * AGC, the noise filter if it is on. The first tune waits for where it
 * tunes. False: the socket failed. */
static bool greet(conn_t *cn, const kiwi_link_t *l, sent_t *z, int64_t now)
{
    kiwi_ctl_t c;
    l->ctl(l->ctx, &c);
    char cmd[128];
    const bool cw = is_cw(c.t.mode), nbfm = is_nbfm(c.t.mode);
    z->ident_gen = ident_cmd(cmd, sizeof cmd);
    if (!ws_text(cn, cmd, l) || !ws_text(cn, "SET compression=1", l)) return false;
    if (kiwi_squelch_cmd(cmd, sizeof cmd, c.sq_pct, nbfm) > 0 && !ws_text(cn, cmd, l)) return false;
    if (kiwi_agc_cmd(cmd, sizeof cmd, c.agc, cw) > 0 && !ws_text(cn, cmd, l)) return false;
    if (c.nr != KIWI_NR_OFF && !send_nr(cn, c.nr, l)) return false;
    z->sq = c.sq_pct;
    z->sq_nbfm = nbfm;
    z->agc = c.agc;
    z->agc_cw = cw;
    z->nr = z->nr_seen = c.nr;
    z->t_sq = z->t_agc = z->t_nr_seen = now;
    return true;
}

/* Where the receiver centres CW, as far as its load_cfg has come: the tune
 * goes again where that is not where it went. */
static void cw_heard(const kiwi_cw_t *c, kiwi_said_t *said, sent_t *z, const kiwi_addr_t *a, const char *tag)
{
    const int32_t ctr = kiwi_cw_centre(c, said->cw_hz);
    if (ctr == said->cw_hz) return;
    ESP_LOGI(tag, "%s:%u centres CW on %ld Hz", a->host, (unsigned)a->port, (long)ctr);
    said->cw_hz = ctr;
    z->tuned = false;
}

kiwi_end_t kiwi_sess_run(kiwi_sess_t *s, const kiwi_addr_t *a, const kiwi_status_t *st,
                         const kiwi_link_t *l, kiwi_said_t *said)
{
    const char *tag = l->tag ? l->tag : "kiwi";
    kiwi_said_init(said);
    l->state(l->ctx, KIWI_ST_CONNECTING, said);
    kiwi_end_t end;
    conn_t cn = CONN_NONE;
    const bool open = open_app(&cn, a, &said->moved, tag, &end, &said->connected, l->go_on, l->ctx);
    /* Followed to https://, and TLS spoken there: the caller keeps it. */
    if (said->moved.tls_port && l->moved) l->moved(l->ctx, said->moved.tls_port);
    if (!open) return end;

    const uint8_t kind = st && st->ok ? st->kind : KIWI_KIND_UNKNOWN;
    const int ext_api = st && st->ok ? st->ext_api : -1;
    const bool pass_given = a->pass && a->pass[0];
    const uint32_t hp = kiwi_addr_hp(a);
    char cmd[200];
    auth_cmd(cmd, sizeof cmd, a);
    if (!ws_text(&cn, cmd, l)) {
        conn_close(&cn);
        return KIWI_END_NO_ANSWER;
    }
    /* The login is out. From here the session ends only on its answer, or
     * on the receiver closing, or on ANSWER_US of silence -- never because
     * the knob wants another receiver meanwhile: a refusal it has counted
     * is read, so the knob counts it too. `clean`: the receiver closed or
     * reset its end, which it does after any answer it gives (read first),
     * never before one. */
    bool answered = false, clean = false;

    rd_t r = { .c = &cn, .b = s->buf };
    kiwi_dsp_init(&s->dsp);
    sent_t z = { .agc = 0xFF, .sq = 0xFF };
    /* Each session starts at the link's own target: a grown one was the last
     * receiver's network's. */
    flow_t w = { .tgt = l->target, .roof = l->target, .ur = l->underruns ? l->underruns(l->ctx) : 0 };
    if (l->preroll) l->preroll(l->ctx, l->target);
    bool web888 = kind == KIWI_KIND_WEB888;
    kiwi_cw_t cw;                               /* its load_cfg, for where it centres CW */
    kiwi_cw_init(&cw);
    bool setup = false, ar_due = false, streaming = false, said_stereo = false, cleared = false;
    bool meter_seen = false;
    int  ar_rate = 0;
    double rate = 0;                            /* the resampler's, as the receiver said */
    int64_t off_seen = 0, bw_seen = 0;
    const int64_t t_open = esp_timer_get_time();
    int64_t t_rx = t_open, t_in = 0, t_ka = 0, t_stream = 0, t_ack = 0;
    end = KIWI_END_NONE;

    while (end == KIWI_END_NONE) {
        /* Every frame in. */
        uint8_t op = 0, *p = NULL;
        size_t n = 0, total = 0;
        bool skipped = false;
        int k = 0;
        while (end == KIWI_END_NONE && (k = rd_frame(&r, &op, &p, &n, &total, &skipped)) == 1) {
            const int64_t now = esp_timer_get_time();
            if (op == 0x8) {
                clean = true;
                end = kiwi_quiet(said, kind, ext_api, true);
            } else if (op == 0x9) {
                ws_send(&cn, 0xA, p, n < 125 ? n : 125);
            } else if ((op == 0x1 || op == 0x2) && n >= 4 && !memcmp(p, "MSG ", 4)) {
                /* Its configuration, longer than msg[]: read whole here. */
                kiwi_cw_init(&cw);
                kiwi_cw_feed(&cw, p, n);
                cw_heard(&cw, said, &z, a, tag);
                const size_t ml = n - 4 < MSG_MAX - 1 ? n - 4 : MSG_MAX - 1;
                memcpy(s->msg, p + 4, ml);
                s->msg[ml] = 0;
                const bool was_in = said->logged_in;
                end = kiwi_said(said, s->msg, pass_given);
                if (end != KIWI_END_NONE || said->logged_in) answered = true;
                /* An ip_limit right after the login: as good as at it. */
                if (end == KIWI_END_DAY_LIMIT && was_in && now - t_in < LOGIN_SLACK_US)
                    said->limit_at_login = true;
                if (end == KIWI_END_KICKED)
                    ESP_LOGW(tag, "%s:%u: kicked: %s", a->host, (unsigned)a->port, said->kick);
                if (end == KIWI_END_NONE && !was_in && said->logged_in) {
                    t_in = t_ka = now;
                    if (!greet(&cn, l, &z, now)) end = KIWI_END_CLOSED;
                    ESP_LOGI(tag, "%s:%u: logged in (%d channels)", a->host, (unsigned)a->port, said->rx_chans);
                }
                /* What it is: a Web-888 numbers its versions by the year. */
                if (kind == KIWI_KIND_UNKNOWN && said->version_maj >= 2000) web888 = true;
                if (said->audio_rate > 0 && said->audio_rate != ar_rate) {
                    ar_rate = said->audio_rate;
                    ar_due = true;
                }
                /* Its audio's rate, exactly as it says it, and never used
                 * unchecked: a rate no Kiwi has ends the session. */
                const double want = said->rate > 0 ? said->rate : ar_rate;
                if (end == KIWI_END_NONE && want > 0 && want != rate) {
                    if (!kiwi_dsp_reset(&s->dsp, want)) {
                        ESP_LOGE(tag, "%s:%u: audio at %.3f Hz: no rate a Kiwi has", a->host,
                                 (unsigned)a->port, want);
                        end = KIWI_END_PROTOCOL;
                    }
                    rate = want;
                    z.tuned = false;            /* the passband's edges move with it */
                }
                /* Where it tunes, said again after the first tune. */
                const int64_t off = (int64_t)(said->offset_khz * 1000.0);
                if (setup && (off != off_seen || (said->bw_hz > 0 && said->bw_hz != bw_seen))) {
                    off_seen = off;
                    if (said->bw_hz > 0) bw_seen = said->bw_hz;
                    l->range(l->ctx, off_seen, off_seen + bw_seen);
                    z.tuned = false;
                }
            } else if ((op == 0x1 || op == 0x2) && n >= 3 && !memcmp(p, "SND", 3)) {
                kiwi_snd_t f;
                if (l->counts) l->counts->frames++;
                if (!said->logged_in || !kiwi_snd(p, n, &f)) {
                    if (l->counts) l->counts->dropped++;
                } else if (f.flags & KIWI_SND_STEREO) {
                    /* I/Q: never asked for. */
                    if (l->counts) l->counts->dropped++;
                    if (!said_stereo) ESP_LOGW(tag, "%s:%u: a stereo frame, let go", a->host, (unsigned)a->port);
                    said_stereo = true;
                } else {
                    /* Heard once the first tune is out: before it, it was
                     * the receiver's own frequency. Decoded either way, and
                     * silenced, never left out, while its squelch is closed;
                     * a backlog's frames are decoded and not played. */
                    const bool sq = f.flags & KIWI_SND_SQUELCH;
                    const double in_n = (double)kiwi_snd_len(&f, n);
                    const bool play = flow(&w, l, &s->dsp, &f, (size_t)(in_n * KIWI_OUT_HZ / s->dsp.rate),
                                           in_n * 1e6 / s->dsp.rate, now, setup, &r, total, tag);
                    /* The dial out of its reach: silence in the audio's
                     * place, as its squelch closed would have it -- the ring
                     * fed at the stream's pace all the same. */
                    kiwi_snd_t g = f;
                    if (z.out) g.flags |= KIWI_SND_SQUELCH;
                    const int m = kiwi_snd_audio(&s->dsp, &g, p, n, play, s->pcm, s->out, l->audio, l->ctx);
                    if (m <= 0 && l->counts) l->counts->dropped++;
                    if (setup && m > 0 && !streaming) {
                        streaming = true;
                        t_stream = w.t_rep = now;
                        memset(&w.rep, 0, sizeof w.rep);
                        l->state(l->ctx, KIWI_ST_STREAMING, said);
                        ESP_LOGI(tag, "streaming from %s:%u, %.0f Hz %s", a->host, (unsigned)a->port,
                                 (double)s->dsp.rate, f.flags & KIWI_SND_ADPCM ? "ADPCM" : "PCM");
                        /* The passband, again: an UberSDR's Kiwi input
                         * opened its channel with its own -- and the AGC
                         * and the squelch, which it let go by while it had
                         * no channel. */
                        z.tuned = false;
                        z.agc = z.sq = 0xFF;
                    }
                    /* The first frame's reading is the channel's last user's;
                     * out of reach, the edge's: neither is the dial's. */
                    if (meter_seen && !z.out)
                        l->meter(l->ctx, kiwi_dbm(f.smeter, web888), f.flags & KIWI_SND_ADC_OVFL, sq);
                    meter_seen = true;
                }
            }
            rd_drop(&r, total);
        }
        if (end != KIWI_END_NONE) break;
        if (k < 0) {
            end = KIWI_END_PROTOCOL;
            break;
        }
        /* A frame too big for the buffer: the settings a receiver sends at
         * every login, no audio -- counted apart, and its load_cfg read as it
         * passes, a piece at a time, its header left out. */
        if (skipped && l->counts) l->counts->big++;
        if (r.passed) {
            if (r.head) kiwi_cw_init(&cw);
            kiwi_cw_feed(&cw, r.b + r.head, r.passed - r.head);
            cw_heard(&cw, said, &z, a, tag);
        }
        r.passed = 0;

        const int64_t now = esp_timer_get_time();
        if (said->logged_in && !l->go_on(l->ctx)) {
            end = KIWI_END_WANT;
            break;
        }
        if ((!said->logged_in && now - t_open > ANSWER_US) || now - t_rx > QUIET_US) {
            end = kiwi_quiet(said, kind, ext_api, false);
            break;
        }
        if (said->logged_in) {
            if (ar_due) {
                snprintf(cmd, sizeof cmd, "SET AR OK in=%d out=%d", ar_rate, KIWI_OUT_HZ);
                if (!ws_text(&cn, cmd, l)) { end = KIWI_END_CLOSED; break; }
                ar_due = false;
            }
            /* Where it tunes, then the first tune: once it has said its
             * bandwidth, or after two seconds a KiwiSDR's 30 MHz. */
            if (!setup && (said->bw_hz > 0 || now - t_in > BW_WAIT_US)) {
                setup = true;
                off_seen = (int64_t)(said->offset_khz * 1000.0);
                bw_seen = said->bw_hz > 0 ? said->bw_hz : DEFAULT_BW;
                l->range(l->ctx, off_seen, off_seen + bw_seen);
                if (!follow(&cn, l, &z, said, rate, now, tag)) { end = KIWI_END_CLOSED; break; }
                l->state(l->ctx, KIWI_ST_LOGGED_IN, said);
            } else if (setup && !follow(&cn, l, &z, said, rate, now, tag)) {
                end = KIWI_END_CLOSED;
                break;
            }
            if (now - t_ka >= KEEPALIVE_US) {
                if (!ws_text(&cn, "SET keepalive", l)) { end = KIWI_END_CLOSED; break; }
                t_ka = now;
            }
            /* Someone at the knob since the last word of it: the receiver's
             * idle timer is told, as its own page tells it -- once a minute
             * at most. Nobody touching it, nothing is said: the owner's
             * limit on idle listening is theirs to keep. */
            if (setup && l->ack && (!t_ack || now - t_ack >= ACK_GAP_US) && l->ack(l->ctx)) {
                if (!ws_text(&cn, "SET inactivity_ack", l)) { end = KIWI_END_CLOSED; break; }
                t_ack = now;
                ESP_LOGD(tag, "SET inactivity_ack");
            }
            /* Ten seconds of audio: whatever day limit it had is behind it. */
            if (streaming && !cleared && now - t_stream > STREAMED_US) {
                cleared = true;
                if (kiwi_mark_get(hp, NULL)) kiwi_mark_clear(hp, tag);
            }
            /* How the audio came, every 30 s. */
            if (streaming && l->report && now - w.t_rep >= REPORT_US) {
                w.rep.level_ms = l->queued ? (uint32_t)(w.avg * 1000 / KIWI_OUT_HZ) : 0;
                w.rep.target_ms = (uint32_t)ms_of(w.tgt);
                w.rep.trim = s->dsp.trim;
                l->report(l->ctx, &w.rep);
                memset(&w.rep, 0, sizeof w.rep);
                w.t_rep = now;
            }
        }
        const int got = rd_fill(&r, LOOP_MS);
        if (got < 0) {
            clean = got == -2;
            end = kiwi_quiet(said, kind, ext_api, true);
            break;
        }
        if (got > 0) t_rx = now;
    }
    /* No answer to the login, and the receiver did not close it either --
     * silence, a socket that failed, something no Kiwi says: it may have
     * refused the login and counted that, its answer lost on the way. */
    said->unanswered = !answered && !clean;
    /* The knob leaving: said, so its channel is free at once. */
    if (end == KIWI_END_WANT) ws_send(&cn, 0x8, (const uint8_t *)"\x03\xE8", 2);
    conn_close(&cn);
    ESP_LOGI(tag, "%s:%u: session over: %s%s", a->host, (unsigned)a->port,
             end == KIWI_END_WANT ? "left" : kiwi_end_note(end),
             said->unanswered ? " (its login unanswered)" : "");
    return end;
}

/* ------------------------------------------------------------ /status */

uint32_t kiwi_addr_hp(const kiwi_addr_t *a)
{
    return a ? kiwi_hp(a->host, a->kport ? a->kport : a->port) : 0;
}

/* One /status request on an open connection, its answer read to its end --
 * the receiver closes after it -- or as far as STATUS_BYTES - 1, the
 * receiver's status lines into `out` on a 200. NULL; "left" (the caller let
 * go meanwhile); or the HTTP status in `*code` with its Location in `loc`. */
static const char *status_ask(conn_t *c, const char *host, uint16_t port, bool tls, char *txt, kiwi_status_t *out,
                              int *code, char *loc, size_t lc, bool (*go_on)(void *), void *ctx)
{
    char hh[80], req[160];
    kiwi_host_hdr(hh, sizeof hh, host, port, tls);
    const int rn = snprintf(req, sizeof req, "GET /status HTTP/1.0\r\nHost: %s\r\nUser-Agent: VFO-Knob\r\n\r\n", hh);
    *code = 0;
    if (rn <= 0 || rn >= (int)sizeof req || !conn_send(c, req, (size_t)rn)) return "no answer";
    /* As it comes, a slice at a time: a receiver chosen meanwhile is not
     * kept waiting by one slow to answer. 0 in `k`: read to its end. */
    size_t got = 0;
    int k = -1;
    int64_t until = esp_timer_get_time() + CONNECT_US;
    while (got < STATUS_BYTES - 1) {
        k = conn_recv(c, txt + got, STATUS_BYTES - 1 - got, MSG_DONTWAIT);
        if (k > 0) {
            got += (size_t)k;
            until = esp_timer_get_time() + CONNECT_US;
            continue;
        }
        if (k == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) break;
        k = -1;
        if (go_on && !go_on(ctx)) return "left";
        const int64_t left = until - esp_timer_get_time();
        if (left <= 0) break;
        if (conn_wait(c, left < SLICE_MS * 1000LL ? left : SLICE_MS * 1000LL) < 0) break;
    }
    txt[got] = 0;
    /* Its body on a 200 only: a 403 is a receiver that will not talk to
     * this address. */
    char *body = strstr(txt, "\r\n\r\n");
    *code = !strncmp(txt, "HTTP/1.", 7) && txt[8] == ' ' ? atoi(txt + 9) : 0;
    if (*code != 200 || !body) {
        if (body) body[2] = 0;                      /* the head alone, for its Location */
        if (!kiwi_http_header(txt, "Location", loc, lc) && lc) loc[0] = 0;
        return "no answer";
    }
    /* Read to its end, where the receiver closes -- or cut short by a
     * timeout or a full buffer: then the line it was in the middle of is
     * left out. An sdr_hw cut before its hourglass would say "no time
     * limits", which can lift a day-limit mark. */
    if (k != 0) {
        char *nl = strrchr(body + 4, '\n');
        if (nl) nl[1] = 0;
        else    body[4] = 0;
    }
    kiwi_status_parse(body + 4, out);
    return NULL;
}

const char *kiwi_status_read(const kiwi_addr_t *a, kiwi_status_t *out, kiwi_moved_t *mv, bool (*go_on)(void *),
                             void *ctx, const char *tag)
{
    if (!tag) tag = "kiwi";
    kiwi_status_parse(NULL, out);               /* nothing known */
    kiwi_moved_t m0;
    if (!mv) mv = &m0;
    memset(mv, 0, sizeof *mv);
    char *txt = heap_caps_calloc(1, STATUS_BYTES, MALLOC_CAP_SPIRAM);
    if (!txt) return "no memory";
    /* Its own address, then -- an http:// one's redirect to https:// on its
     * own host -- that, once. */
    const char *host = a->host, *res = "no answer";
    uint16_t port = a->port;
    bool tls = a->tls;
    for (int hop = 0; hop < 2; hop++) {
        conn_t c = CONN_NONE;
        const kiwi_end_t why = conn_open(&c, host, port, tls, tag, NULL, go_on, ctx);
        if (why != KIWI_END_NONE) {
            res = why == KIWI_END_WANT ? "left" : why == KIWI_END_NOT_FOUND ? "name not found"
                : why == KIWI_END_CERT ? "certificate not valid" : kiwi_end_note(why);
            break;
        }
        if (hop) mv->tls_port = port;           /* TLS spoken where it was sent: kept from now */
        int code = 0;
        char loc[128];
        res = status_ask(&c, host, port, tls, txt, out, &code, loc, sizeof loc, go_on, ctx);
        conn_close(&c);
        if (!res || !strcmp(res, "left") || !code) break;
        uint16_t to = 0;
        if (kiwi_redirect_code(code) && !hop && kiwi_redirect(loc, host, tls, &to, mv->to, sizeof mv->to)) {
            ESP_LOGI(tag, "%s:%u: /status: HTTP %d to https://, port %u: followed", host, (unsigned)port, code,
                     (unsigned)to);
            port = to;
            tls = true;
            memset(txt, 0, STATUS_BYTES);
            continue;
        }
        if (kiwi_http(code) == KIWI_END_MOVED) {
            kiwi_redirect(loc, host, true, NULL, mv->to, sizeof mv->to);
            ESP_LOGW(tag, "%s:%u: /status: HTTP %d -- moved to %s", host, (unsigned)port, code,
                     mv->to[0] ? mv->to : "?");
            res = "moved";
        } else {
            res = code == 403 ? "refused" : "not a kiwi";
        }
        break;
    }
    free(txt);
    return res;
}

/* What the sessions' tasks have read of the receivers' /status this boot,
 * for one another (kiwi_status_keep): the newest of each, KEPT_MAX at most,
 * the oldest let go. Copied whole under the lock, as the ears copy theirs. */
#define KEPT_MAX 4
static portMUX_TYPE s_kept_mux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static struct { uint32_t hp; int64_t t; kiwi_status_t st; } s_kept[KEPT_MAX];

void kiwi_status_keep(uint32_t hp, const kiwi_status_t *st)
{
    if (!hp || !st) return;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_kept_mux);
    int at = 0;
    for (int i = 0; i < KEPT_MAX; i++) {
        if (s_kept[i].hp == hp) { at = i; break; }
        if (s_kept[i].t < s_kept[at].t) at = i;     /* the oldest, or a free place (0) */
    }
    s_kept[at].hp = hp;
    s_kept[at].t  = now;
    s_kept[at].st = *st;
    portEXIT_CRITICAL(&s_kept_mux);
}

bool kiwi_status_kept(uint32_t hp, kiwi_status_t *out, int64_t *at)
{
    bool got = false;
    portENTER_CRITICAL(&s_kept_mux);
    for (int i = 0; hp && i < KEPT_MAX && !got; i++) {
        if (s_kept[i].hp != hp) continue;
        if (out) *out = s_kept[i].st;
        if (at) *at = s_kept[i].t;
        got = true;
    }
    portEXIT_CRITICAL(&s_kept_mux);
    return got;
}


/* ------------------------------------------------------------ the test */

/* ,"key":"value" into json at *o, the value escaped for JSON. */
static void jstr(char *json, size_t cap, int *o, const char *key, const char *v)
{
    if (*o < 0 || (size_t)*o >= cap) return;
    int k = snprintf(json + *o, cap - *o, ",\"%s\":\"", key);
    if (k < 0 || (size_t)(*o + k) >= cap) { *o = (int)cap; return; }
    *o += k;
    for (; v && *v && (size_t)*o + 3 < cap; v++) {
        if (*v == '"' || *v == '\\') json[(*o)++] = '\\';
        if ((unsigned char)*v >= 0x20) json[(*o)++] = *v;
    }
    if ((size_t)*o + 2 > cap) { *o = (int)cap; return; }
    json[(*o)++] = '"';
    json[*o] = 0;
}

/* A test's answer, as the page puts it (index.html's LOGIN). */
static const char *test_word(kiwi_end_t e)
{
    switch (e) {
    case KIWI_END_NONE:      return "ok";
    case KIWI_END_PASSWORD:  return "wrong password";
    case KIWI_END_FULL:
    case KIWI_END_PWD_FULL:  return "busy";
    case KIWI_END_REFUSED:   return "refused";
    case KIWI_END_NOT_KIWI:  return "not a kiwi";
    case KIWI_END_DUP_IP:    return "one per ip";
    case KIWI_END_SILENT:
    case KIWI_END_APPS_FULL: return "apps full";
    case KIWI_END_NO_APPS:   return "no apps";
    case KIWI_END_DAY_LIMIT: return "day limit";
    case KIWI_END_DOWN:      return "down";
    case KIWI_END_UPDATING:  return "updating";
    case KIWI_END_TRY_LATER: return "try later";
    case KIWI_END_KICKED:    return "kicked";
    case KIWI_END_MOVED:     return "moved";
    case KIWI_END_CERT:      return "certificate";
    default:                 return "no answer";
    }
}

/* ext_api_nchans in a Web-888's load_cfg (URL-encoded JSON), wherever the
 * frames and reads break it: the channels its owner lets apps have. -1 not
 * seen yet. */
typedef struct { char tail[40]; size_t n; int apps; } scan_t;

static void scan(scan_t *sc, const uint8_t *p, size_t n)
{
    static const char KEY[] = "ext_api_nchans%22%3a";
    const size_t kl = sizeof KEY - 1;
    char w[sizeof sc->tail + 64];
    while (n && sc->apps < 0) {
        const size_t take = n < 64 ? n : 64;
        memcpy(w, sc->tail, sc->n);
        memcpy(w + sc->n, p, take);
        const size_t wn = sc->n + take;
        for (size_t i = 0; i + kl < wn; i++) {
            if (memcmp(w + i, KEY, kl)) continue;
            size_t d = i + kl;
            if (d + 3 <= wn && !memcmp(w + d, "%20", 3)) d += 3;
            size_t e = d;
            while (e < wn && w[e] >= '0' && w[e] <= '9') e++;
            if (e > d && e < wn && w[e] == '%') sc->apps = atoi(w + d);   /* the number, whole */
            break;
        }
        /* The end kept: a key across the break. */
        const size_t keep = wn < sizeof sc->tail ? wn : sizeof sc->tail;
        memmove(sc->tail, w + wn - keep, keep);
        sc->n = keep;
        p += take;
        n -= take;
    }
}

/* A test still in time (ctx: until when) on its way to the login. */
static bool test_in_time(void *ctx) { return esp_timer_get_time() < *(const int64_t *)ctx; }

/* The login, for its answer: up to ANSWER_US for it, then a few seconds more
 * on a Web-888 for how many channels apps may have -- the way there no
 * further than `until`. A day limit -- refused, or left unanswered where it
 * may count -- marks the receiver, as a session's does: a test is no try, so
 * it is one strike more. */
static kiwi_end_t test_login(const kiwi_addr_t *a, const kiwi_status_t *st, int64_t t_read, scan_t *sc,
                             kiwi_moved_t *mv, int64_t *until, const char *tag)
{
    const uint32_t hp = kiwi_addr_hp(a);
    kiwi_end_t end;
    bool reached;
    conn_t cn = CONN_NONE;
    if (!open_app(&cn, a, mv, tag, &end, &reached, test_in_time, until)) return end;
    char cmd[200];
    auth_cmd(cmd, sizeof cmd, a);
    /* A frame's room, and a MSG's, in PSRAM. */
    uint8_t *b = heap_caps_malloc(BUF_BYTES + MSG_MAX, MALLOC_CAP_SPIRAM);
    if (!b || !ws_text(&cn, cmd, NULL)) {
        free(b);
        conn_close(&cn);
        return KIWI_END_NO_ANSWER;
    }
    char *msg = (char *)b + BUF_BYTES;
    kiwi_said_t said;
    kiwi_said_init(&said);
    rd_t r = { .c = &cn, .b = b };
    const uint8_t kind = st->ok ? st->kind : KIWI_KIND_UNKNOWN;
    const int64_t t0 = esp_timer_get_time();
    int64_t t_in = 0;
    bool answered = false, clean = false;       /* as in kiwi_sess_run */
    end = KIWI_END_NONE;
    while (end == KIWI_END_NONE) {
        uint8_t op = 0, *p = NULL;
        size_t n = 0, total = 0;
        bool skipped = false;
        int k = 0;
        while (end == KIWI_END_NONE && (k = rd_frame(&r, &op, &p, &n, &total, &skipped)) == 1) {
            if (op == 0x8) {
                clean = true;
                end = kiwi_quiet(&said, kind, st->ext_api, true);
            }
            if ((op == 0x1 || op == 0x2) && n >= 4 && !memcmp(p, "MSG ", 4)) {
                scan(sc, p, n);
                const size_t ml = n - 4 < MSG_MAX - 1 ? n - 4 : MSG_MAX - 1;
                memcpy(msg, p + 4, ml);
                msg[ml] = 0;
                const bool was_in = said.logged_in;
                end = kiwi_said(&said, msg, a->pass && a->pass[0]);
                if (end != KIWI_END_NONE || said.logged_in) answered = true;
                /* An ip_limit right after the login: as good as at it. */
                if (end == KIWI_END_DAY_LIMIT && was_in && esp_timer_get_time() - t_in < LOGIN_SLACK_US)
                    said.limit_at_login = true;
                if (end == KIWI_END_NONE && !was_in && said.logged_in) {
                    t_in = esp_timer_get_time();
                    ident_cmd(cmd, sizeof cmd);
                    ws_text(&cn, cmd, NULL);
                }
            }
            rd_drop(&r, total);
        }
        /* A frame too big to take whole -- load_cfg -- read as it passes. */
        if (r.passed) scan(sc, b, r.passed);
        r.passed = 0;
        if (end != KIWI_END_NONE || k < 0) break;
        const int64_t now = esp_timer_get_time();
        if (!said.logged_in && now - t0 > ANSWER_US) {
            end = kiwi_quiet(&said, kind, st->ext_api, false);
            break;
        }
        if (said.logged_in && (sc->apps >= 0 || now - t_in > TEST_AFTER_US)) break;
        const int got = rd_fill(&r, 200);
        if (r.passed) scan(sc, b, r.passed);
        r.passed = 0;
        if (got < 0) {
            clean = got == -2;
            end = kiwi_quiet(&said, kind, st->ext_api, true);
            break;
        }
    }
    /* Its day limit: a refusal it counted -- or a login left unanswered, by
     * one whose count it may be in. The mark, and nothing more asked of it. */
    const int64_t age = esp_timer_get_time() - t_read;
    if (end == KIWI_END_DAY_LIMIT)
        kiwi_mark_set(hp, said.limit_at_login || !said.logged_in ? KIWI_MARK_REFUSED : KIWI_MARK_MIDWAY, false,
                      st, age, tag);
    else if (!answered && !clean && kiwi_mark_may_count(hp, st))
        kiwi_mark_set(hp, KIWI_MARK_NO_ANSWER, false, st, age, tag);
    if (said.logged_in && end == KIWI_END_NONE) ws_send(&cn, 0x8, (const uint8_t *)"\x03\xE8", 2);
    free(b);
    conn_close(&cn);
    if (end == KIWI_END_NONE && sc->apps == 0) end = KIWI_END_NO_APPS;
    return end;
}

/* The last Test's /status, for another Test of the same receiver within a
 * minute -- a second press, a double click -- rather than a second read.
 * Only the web server's task runs Tests, one at a time. */
#define TEST_STATUS_US (60 * 1000000LL)
EXT_RAM_BSS_ATTR static struct { uint32_t hp; int64_t t; kiwi_status_t st; uint16_t tls_port; } s_test_st;

/* The receiver a Test is on its way to log in to, 0 none (kiwi_test_busy). */
static portMUX_TYPE s_test_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_test_hp;

static void test_busy(uint32_t hp)
{
    portENTER_CRITICAL(&s_test_mux);
    s_test_hp = hp;
    portEXIT_CRITICAL(&s_test_mux);
}

bool kiwi_test_busy(uint32_t hp)
{
    portENTER_CRITICAL(&s_test_mux);
    const bool b = hp && s_test_hp == hp;
    portEXIT_CRITICAL(&s_test_mux);
    return b;
}

esp_err_t kiwi_test(const kiwi_addr_t *a, bool (*in_use)(uint32_t hp), char *json, size_t cap, kiwi_moved_t *mv,
                    const char *tag)
{
    if (!tag) tag = "kiwi";
    if (!a || !a->host || !a->host[0] || !json || cap < 64) return ESP_ERR_INVALID_ARG;
    kiwi_moved_t m0;
    if (!mv) mv = &m0;
    memset(mv, 0, sizeof *mv);
    /* The marks, even before any receiver has played: a test never logs in
     * to a marked receiver. */
    kiwi_mark_init(tag);
    const uint32_t hp = kiwi_addr_hp(a);
    kiwi_status_t *st = &s_test_st.st;
    const int64_t now = esp_timer_get_time();
    int64_t until = now + TEST_BEFORE_US;
    const bool again = s_test_st.hp == hp && s_test_st.t && now - s_test_st.t < TEST_STATUS_US;
    const char *why = again ? NULL : kiwi_status_read(a, st, mv, test_in_time, &until, tag);
    if (why && !strcmp(why, "left")) why = "no answer";     /* its time up on the way */
    if (again) mv->tls_port = s_test_st.tls_port;
    else {
        s_test_st.hp = why ? 0 : hp;
        s_test_st.t  = esp_timer_get_time();
        s_test_st.tls_port = mv->tls_port;
    }
    const int64_t t_read = s_test_st.t;
    if (why) {
        char e[176];
        int o = snprintf(json, cap, "{\"ok\":false");
        if (!strcmp(why, "moved"))
            snprintf(e, sizeof e, "%s:%u: moved%s%s", a->host, (unsigned)a->port, mv->to[0] ? " to " : "", mv->to);
        else
            snprintf(e, sizeof e, "%s:%u: %s", a->host, (unsigned)a->port, why);
        jstr(json, cap, &o, "error", e);
        /* Sent to https:// on its own host on the way, and TLS spoken there:
         * kept all the same, and said, for the page to show it. */
        if (mv->tls_port && o > 0 && (size_t)o < cap)
            o += snprintf(json + o, cap - o, ",\"tls\":true,\"port\":%u", (unsigned)mv->tls_port);
        if (o > 0 && (size_t)o + 1 < cap) snprintf(json + o, cap - o, "}");
        ESP_LOGI(tag, "test of %s:%u: %s", a->host, (unsigned)a->port, e);
        return ESP_OK;
    }
    /* Its /status sent it to https:// on its own host: the login goes there,
     * its key the same. */
    kiwi_addr_t at = *a;
    if (mv->tls_port && !a->tls) {
        at.kport = a->kport ? a->kport : a->port;
        at.port  = mv->tls_port;
        at.tls   = true;
    }
    if (!again) kiwi_mark_check(hp, st, tag);

    const char *login;
    bool tried = false;
    scan_t sc = { .apps = -1 };
    /* On its way to the receiver, said before asking whether it is the one
     * in use: its session's task says that before it looks here, so either
     * this sees the session and answers from it, or the session waits for
     * this login's answer -- a refusal marks the receiver -- before its own. */
    test_busy(hp);
    if (in_use && in_use(hp))                               login = "in use";
    else if (kiwi_mark_get(hp, NULL))                       login = "day limit";
    else if (st->ok && st->kind == KIWI_KIND_KIWISDR && st->ext_api == 0) login = "no apps";
    else {
        tried = true;
        kiwi_moved_t m2 = { 0 };
        login = test_word(test_login(&at, st, t_read, &sc, &m2, &until, tag));
        if (m2.tls_port) {
            mv->tls_port = m2.tls_port;
            at.kport = at.kport ? at.kport : at.port;
            at.port  = m2.tls_port;
            at.tls   = true;
        }
        if (m2.to[0]) memcpy(mv->to, m2.to, sizeof mv->to);
    }
    test_busy(0);

    kiwi_mark_t mk;
    const bool marked = kiwi_mark_get(hp, &mk);
    const uint8_t kind = st->kind ? st->kind : marked ? mk.kind : (uint8_t)KIWI_KIND_UNKNOWN;
    int o = snprintf(json, cap, "{\"ok\":%s", !strcmp(login, "ok") || !strcmp(login, "in use") ? "true" : "false");
    char users[12] = "?", umax[12] = "?";
    if (st->users >= 0)     snprintf(users, sizeof users, "%d", st->users);
    if (st->users_max >= 0) snprintf(umax, sizeof umax, "%d", st->users_max);
    /* What to call it on the dial where the page has no name for it yet: as
     * many characters as the dial has room for. */
    char fill[16];
    kiwi_status_name(fill, sizeof fill, st);
    jstr(json, cap, &o, "name", st->name);
    jstr(json, cap, &o, "fill", fill);
    jstr(json, cap, &o, "antenna", st->antenna);
    jstr(json, cap, &o, "loc", st->loc);
    jstr(json, cap, &o, "sw", st->sw);
    jstr(json, cap, &o, "model", kind == KIWI_KIND_WEB888 ? "Web-888" : kind == KIWI_KIND_KIWISDR ? "KiwiSDR" : "");
    jstr(json, cap, &o, "users", users);
    jstr(json, cap, &o, "users_max", umax);
    jstr(json, cap, &o, "login", login);
    jstr(json, cap, &o, "path", tried ? "app" : "");
    if (mv->to[0]) jstr(json, cap, &o, "moved", mv->to);
    /* The address it was spoken on: https:// where a redirect sent it. */
    if (o > 0 && (size_t)o < cap)
        o += snprintf(json + o, cap - o, ",\"tls\":%s,\"port\":%u", at.tls ? "true" : "false", (unsigned)at.port);
    if (o > 0 && (size_t)o < cap)
        o += snprintf(json + o, cap - o, ",\"apps\":%d,\"ext_api\":%d,\"limits\":%s,\"bands\":[%lld,%lld]",
                      sc.apps, st->ext_api, st->tlimits ? "true" : "false", (long long)st->bands_lo,
                      (long long)st->bands_hi);
    /* The mark: its strikes, the tries left, and whether it is held -- until
     * the receiver restarts, or on a KiwiSDR a day at most -- and whether it
     * is only a login left unanswered. */
    if (marked && o > 0 && (size_t)o < cap)
        o += snprintf(json + o, cap - o, ",\"mark\":{\"strikes\":%u,\"tries\":%d,\"held\":%s,\"unsure\":%s}",
                      (unsigned)mk.strikes, mk.strikes < KIWI_STRIKES_MAX ? KIWI_STRIKES_MAX - mk.strikes : 0,
                      mk.strikes >= KIWI_STRIKES_MAX ? "true" : "false",
                      mk.flags & KIWI_MARK_UNSURE ? "true" : "false");
    if (o > 0 && (size_t)o + 1 < cap) snprintf(json + o, cap - o, "}");
    ESP_LOGI(tag, "test of %s:%u: %s", a->host, (unsigned)a->port, login);
    return ESP_OK;
}
