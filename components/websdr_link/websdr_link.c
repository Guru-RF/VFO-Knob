/* A web SDR's link: the connection, the upgrade, the frames. See
 * websdr_link.h.
 *
 * The connection is kiwi_sess.c's, as it has been tried on the receivers
 * the Kiwi firmware meets -- a name with an address that resets, fronts
 * that speak only https://, a TLS handshake of a second or two in software
 * -- with the path its own and the frames of any length. Every wait is a
 * select() a slice at a time, after a look at what TLS holds decrypted
 * already, the caller asked between. */
#include "websdr_link.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "kiwi_tls.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#define RX_BYTES     4096               /* a read's worth: frames of any length pass through it */
#define TX_BYTES     1024               /* the longest the knob sends */
#define CONNECT_US   (5 * 1000000LL)    /* a connection, or the HTTP answer */
#define SLICE_MS     100
#define HANDSHAKE_US (10 * 1000000LL)   /* TLS: this long of the front's own time */
#define SEND_US      (5 * 1000000LL)    /* a full socket, over TLS: as SO_SNDTIMEO waits in the clear */
#define ADDRS        4

struct wl {
    int         fd;
    kiwi_tls_t *tls;
    uint8_t     rx[RX_BYTES];
    size_t      have, at;               /* bytes in rx; where the next is */
    /* The frame being read: its header as it comes, then its payload. */
    uint8_t     hdr[14];
    uint8_t     hn, hl;                 /* header bytes had; needed */
    bool        in_payload;
    uint8_t     op, msg_op;             /* this frame's; the message's (continuations) */
    bool        fin, masked;
    uint8_t     mask[4];
    uint64_t    len, left;
    uint8_t     ctl[125];               /* a control frame, whole */
    uint8_t     tx[TX_BYTES + 14];
    uint64_t    bytes_in, bytes_out;
    const char *tag;
};

wl_t *wl_new(void)
{
    wl_t *w = heap_caps_calloc(1, sizeof *w, MALLOC_CAP_SPIRAM);
    if (w) w->fd = -1;
    return w;
}

/* ---------------------------------------------------------- the connection */

typedef struct {
    struct sockaddr_in a[ADDRS];
    int n;
} addrs_t;

static wl_end_t resolve(const char *host, uint16_t port, addrs_t *out, const char *tag)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    char ps[8];
    snprintf(ps, sizeof ps, "%u", (unsigned)port);
    out->n = 0;
    if (getaddrinfo(host, ps, &hints, &ai) != 0 || !ai) {
        ESP_LOGW(tag, "%s: name not found", host);
        return WL_NOT_FOUND;
    }
    for (const struct addrinfo *p = ai; p && out->n < ADDRS; p = p->ai_next)
        if (p->ai_family == AF_INET && p->ai_addrlen >= sizeof out->a[0])
            memcpy(&out->a[out->n++], p->ai_addr, sizeof out->a[0]);
    freeaddrinfo(ai);
    return out->n ? WL_OK : WL_NOT_FOUND;
}

static bool asked(const wl_ask_t *k) { return !k || !k->go_on || k->go_on(k->ctx); }

static const char *why_word(wl_end_t e)
{
    static const char *const W[] = { "ok", "left", "not found", "no route", "no socket", "no answer",
                                     "certificate", "http", "moved" };
    return (unsigned)e < sizeof W / sizeof W[0] ? W[e] : "?";
}

/* A connected socket, or -1 with `why`: NO_ROUTE for a connection refused,
 * unreachable or not taken in time -- the receiver out of reach. */
static int tcp_connect(const struct sockaddr_in *sa, int timeout_ms, wl_end_t *why, const wl_ask_t *k)
{
    wl_end_t w = WL_NO_ROUTE;
    int fd = socket(AF_INET, SOCK_STREAM, 0), e = errno;
    if (fd < 0) { w = WL_NO_SOCKET; goto fail; }
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    const int r = connect(fd, (const struct sockaddr *)sa, sizeof *sa);
    e = errno;
    if (r != 0 && e != EINPROGRESS) goto fail;
    int sr = r == 0;
    for (int left = timeout_ms; !sr && left > 0; left -= SLICE_MS) {
        if (!asked(k)) {
            close(fd);
            *why = WL_WANT;
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
    if (e) goto fail;
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    struct timeval io = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io, sizeof io);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io, sizeof io);
    return fd;
fail: {
        char ip[16];
        inet_ntoa_r(sa->sin_addr, ip, sizeof ip);
        ESP_LOGW(k && k->tag ? k->tag : "wl", "%s:%u: %s (errno %d)", ip, (unsigned)ntohs(sa->sin_port),
                 why_word(w), e);
    }
    if (fd >= 0) close(fd);
    *why = w;
    return -1;
}

typedef struct {
    int         fd;
    kiwi_tls_t *tls;
} conn_t;

static void conn_close(conn_t *c)
{
    if (c->tls) kiwi_tls_free(c->tls);
    if (c->fd >= 0) close(c->fd);
    c->tls = NULL;
    c->fd = -1;
}

static int conn_wait(const conn_t *c, int64_t us)
{
    if (c->tls && kiwi_tls_pending(c->tls) > 0) return 1;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(c->fd, &rf);
    struct timeval tv = { .tv_sec = (time_t)(us / 1000000), .tv_usec = (suseconds_t)(us % 1000000) };
    return select(c->fd + 1, &rf, NULL, NULL, &tv);
}

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

/* TLS on the connected socket, a step at a time, the caller asked between. */
static wl_end_t tls_open(conn_t *c, const wl_addr_t *a, const char *host, uint16_t port, const wl_ask_t *k)
{
    const char *tag = k && k->tag ? k->tag : "wl";
    const int64_t t0 = esp_timer_get_time();
    c->tls = kiwi_tls_new(c->fd, host, a->key ? a->key : kiwi_hp(host, port));
    if (!c->tls) {
        ESP_LOGE(tag, "%s:%u: no memory for TLS", host, (unsigned)port);
        return WL_NO_SOCKET;
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
            return WL_OK;
        }
        if (r < 0) {
            char why[160];
            kiwi_tls_why(c->tls, why, sizeof why);
            const bool cert = kiwi_tls_cert_refused(c->tls);
            ESP_LOGW(tag, "%s:%u: %s: %s", host, (unsigned)port,
                     cert ? "its certificate is not valid -- not spoken to" : "TLS failed", why);
            return cert ? WL_CERT : WL_NO_ANSWER;
        }
        if (!asked(k)) return WL_WANT;
        const int64_t left = t0 + own + HANDSHAKE_US - esp_timer_get_time();
        if (left <= 0) {
            ESP_LOGW(tag, "%s:%u: TLS: no answer to the handshake", host, (unsigned)port);
            return WL_NO_ANSWER;
        }
        if (r == 2) continue;
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(c->fd, &fs);
        struct timeval tv = { .tv_sec = 0, .tv_usec = left < SLICE_MS * 1000LL ? (long)left : SLICE_MS * 1000L };
        if (select(c->fd + 1, wr ? NULL : &fs, wr ? &fs : NULL, NULL, &tv) < 0) return WL_NO_ANSWER;
    }
}

/* An HTTP request's answer, read no further than its blank line: the
 * status (0 for none within CONNECT_US, -1 the caller let go), and its
 * Location and Content-Length (-1 none). */
static int http_head(conn_t *c, const wl_ask_t *k, char *loc, size_t lc, long long *clen)
{
    char line[160];
    size_t got = 0;
    int code = -2;
    if (loc && lc) loc[0] = 0;
    if (clen) *clen = -1;
    const int64_t until = esp_timer_get_time() + CONNECT_US;
    for (int i = 0; i < 8192;) {
        char ch;
        const int r = conn_recv(c, &ch, 1, MSG_DONTWAIT);
        if (r == 1) {
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
                if (code <= 0) return 0;
            } else if (!line[0]) {
                return code;
            } else if (loc && lc && !strncasecmp(line, "location:", 9)) {
                const char *v = line + 9;
                while (*v == ' ' || *v == '\t') v++;
                snprintf(loc, lc, "%s", v);
            } else if (clen && !strncasecmp(line, "content-length:", 15)) {
                *clen = atoll(line + 15);
            }
            continue;
        }
        if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return 0;
        if (!asked(k)) return -1;
        const int64_t left = until - esp_timer_get_time();
        if (left <= 0) return 0;
        if (conn_wait(c, left < SLICE_MS * 1000LL ? left : SLICE_MS * 1000LL) < 0) return 0;
    }
    return 0;
}

/* The request a connection carries: the upgrade on `path` (ws), or a GET. */
typedef wl_end_t (*ask_fn)(conn_t *c, const char *host, uint16_t port, bool tls, const char *path,
                           const wl_ask_t *k, int *code, char *loc, size_t lc, void *x);

static wl_end_t ask_ws(conn_t *c, const char *host, uint16_t port, bool tls, const char *path, const wl_ask_t *k,
                       int *code, char *loc, size_t lc, void *x)
{
    const wl_addr_t *a = x;
    uint8_t key[16];
    esp_fill_random(key, sizeof key);
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char k64[25];
    int o = 0;
    for (int i = 0; i < 16; i += 3) {
        const uint32_t v = (uint32_t)key[i] << 16 | (i + 1 < 16 ? key[i + 1] << 8 : 0) | (i + 2 < 16 ? key[i + 2] : 0);
        k64[o++] = B64[v >> 18 & 63];
        k64[o++] = B64[v >> 12 & 63];
        k64[o++] = i + 1 < 16 ? B64[v >> 6 & 63] : '=';
        k64[o++] = i + 2 < 16 ? B64[v & 63] : '=';
    }
    k64[o] = 0;
    char hh[80], og[100] = "", req[512];
    kiwi_host_hdr(hh, sizeof hh, host, port, tls);
    /* Where its own page is served from: the scheme spoken here, after a
     * redirect to https:// too. */
    if (a && a->origin) snprintf(og, sizeof og, "Origin: %s://%s\r\n", tls ? "https" : "http", hh);
    const int n = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%sUser-Agent: VFO-Knob\r\n\r\n", path, hh, k64,
        og);
    *code = 0;
    if (n <= 0 || n >= (int)sizeof req || !conn_send(c, req, (size_t)n)) return WL_NO_ANSWER;
    *code = http_head(c, k, loc, lc, NULL);
    if (*code < 0) return WL_WANT;
    if (*code == 0) return WL_NO_ANSWER;
    return *code == 101 ? WL_OK : WL_HTTP;
}

typedef struct {
    void (*body)(void *ctx, const uint8_t *p, size_t n);
    void *ctx;
    size_t max;
    uint8_t *buf;
} get_t;

static wl_end_t ask_get(conn_t *c, const char *host, uint16_t port, bool tls, const char *path, const wl_ask_t *k,
                        int *code, char *loc, size_t lc, void *x)
{
    get_t *g = x;
    char hh[80], req[320];
    kiwi_host_hdr(hh, sizeof hh, host, port, tls);
    /* HTTP/1.0: its body as it is, never chunked, to the close. */
    const int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: VFO-Knob\r\n"
                           "Accept: application/json\r\n\r\n", path, hh);
    *code = 0;
    if (n <= 0 || n >= (int)sizeof req || !conn_send(c, req, (size_t)n)) return WL_NO_ANSWER;
    long long clen;
    *code = http_head(c, k, loc, lc, &clen);
    if (*code < 0) return WL_WANT;
    if (*code == 0) return WL_NO_ANSWER;
    if (*code != 200) return WL_HTTP;
    size_t got = 0, want = clen >= 0 && (unsigned long long)clen < g->max ? (size_t)clen : g->max;
    int64_t until = esp_timer_get_time() + CONNECT_US;
    bool cut = false;
    while (got < want) {
        const size_t room = want - got < RX_BYTES ? want - got : RX_BYTES;
        const int r = conn_recv(c, g->buf, room, MSG_DONTWAIT);
        if (r > 0) {
            g->body(g->ctx, g->buf, (size_t)r);
            got += (size_t)r;
            until = esp_timer_get_time() + CONNECT_US;
            continue;
        }
        if (r == 0) break;                                  /* its end: the body's */
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            cut = true;
            break;
        }
        if (!asked(k)) return WL_WANT;
        const int64_t left = until - esp_timer_get_time();
        if (left <= 0 || conn_wait(c, left < SLICE_MS * 1000LL ? left : SLICE_MS * 1000LL) < 0) {
            cut = true;                                     /* silent, before its close */
            break;
        }
    }
    /* Closed before all its Content-Length came: short too. */
    if (clen >= 0 && got < want) cut = true;
    return cut ? WL_CUT : WL_OK;
}

/* Connected to the first of its addresses that takes it, TLS spoken, the
 * request answered: the end; `c` open on WL_OK. An http:// receiver's
 * redirect to https:// on its own host is followed at once, once. */
static wl_end_t attempt(conn_t *c, const wl_addr_t *a, const char *path, const wl_ask_t *k, wl_said_t *said,
                        ask_fn ask, void *x)
{
    const char *tag = k && k->tag ? k->tag : "wl";
    const char *host = a->host;
    uint16_t port = a->port;
    bool tls = a->tls;
    wl_said_t z = { 0 };
    wl_end_t why = WL_NO_ANSWER;
    for (int hop = 0; hop < 2; hop++) {
        addrs_t ad;
        why = resolve(host, port, &ad, tag);
        if (why != WL_OK) break;
        bool http = false;
        uint16_t to = 0;
        why = WL_NO_ANSWER;
        for (int i = 0; i < ad.n && !to; i++) {
            wl_end_t w;
            c->fd = tcp_connect(&ad.a[i], ad.n > 1 ? 3000 : 5000, &w, k);
            if (c->fd >= 0) {
                z.reached = true;
                w = tls ? tls_open(c, a, host, port, k) : WL_OK;
                if (w == WL_OK && hop) z.tls_port = port;
            }
            if (c->fd >= 0 && w == WL_OK) {
                char loc[128];
                int code = 0;
                w = ask(c, host, port, tls, path, k, &code, loc, sizeof loc, x);
                if (w == WL_OK) {
                    z.status = code;
                    if (said) *said = z;
                    return WL_OK;
                }
                conn_close(c);
                if (w == WL_WANT) { why = w; break; }
                if (kiwi_redirect_code(code) && !hop && kiwi_redirect(loc, host, tls, &to, z.to, sizeof z.to)) {
                    ESP_LOGI(tag, "%s:%u: HTTP %d to https://, port %u: followed", host, (unsigned)port, code,
                             (unsigned)to);
                    continue;
                }
                if (kiwi_redirect_code(code)) {
                    kiwi_redirect(loc, host, true, NULL, z.to, sizeof z.to);
                    ESP_LOGW(tag, "%s:%u: HTTP %d -- moved to %s", host, (unsigned)port, code, z.to[0] ? z.to : "?");
                    w = WL_MOVED;
                } else if (code > 0) {
                    ESP_LOGW(tag, "%s:%u%s: HTTP %d", host, (unsigned)port, path, code);
                } else {
                    ESP_LOGW(tag, "%s:%u: no answer to the request", host, (unsigned)port);
                }
                if (code > 0 || !http) {
                    why = w;
                    z.status = code;
                }
                http |= code > 0;
            } else {
                conn_close(c);
                if (w == WL_WANT || w == WL_CERT) { why = w; break; }
                if (!http) why = w;
            }
            if (!asked(k)) { why = WL_WANT; break; }
        }
        if (!to || why == WL_WANT || why == WL_CERT) break;
        port = to;
        tls = true;
    }
    if (said) *said = z;
    return why;
}

wl_end_t wl_open(wl_t *w, const wl_addr_t *a, const char *path, const wl_ask_t *k, wl_said_t *said)
{
    wl_close(w, false);
    w->tag = k && k->tag ? k->tag : "wl";
    w->have = w->at = 0;
    w->hn = 0;
    w->hl = 2;
    w->in_payload = false;
    w->msg_op = 0;
    w->bytes_in = w->bytes_out = 0;
    conn_t c = { .fd = -1, .tls = NULL };
    const wl_end_t e = attempt(&c, a, path, k, said, ask_ws, (void *)a);
    w->fd = c.fd;
    w->tls = c.tls;
    return e;
}

wl_end_t wl_get(const wl_addr_t *a, const char *path, void (*body)(void *ctx, const uint8_t *p, size_t n),
                void *bctx, size_t max, const wl_ask_t *k, wl_said_t *said)
{
    uint8_t *buf = heap_caps_malloc(RX_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) return WL_NO_SOCKET;
    get_t g = { .body = body, .ctx = bctx, .max = max, .buf = buf };
    conn_t c = { .fd = -1, .tls = NULL };
    const wl_end_t e = attempt(&c, a, path, k, said, ask_get, &g);
    conn_close(&c);
    free(buf);
    return e;
}

void wl_close(wl_t *w, bool bye)
{
    if (!w || w->fd < 0) return;
    if (bye) wl_send(w, 0x8, "\x03\xE8", 2);
    conn_t c = { .fd = w->fd, .tls = w->tls };
    conn_close(&c);
    w->fd = -1;
    w->tls = NULL;
}

void wl_counts(const wl_t *w, uint64_t *in, uint64_t *out)
{
    if (in) *in = w->bytes_in;
    if (out) *out = w->bytes_out;
}

/* ---------------------------------------------------------------- frames */

bool wl_send(wl_t *w, uint8_t op, const void *p, size_t n)
{
    if (w->fd < 0 || n > TX_BYTES) return false;
    uint8_t *f = w->tx;
    size_t h = 0;
    f[h++] = 0x80 | op;
    if (n < 126) {
        f[h++] = 0x80 | (uint8_t)n;
    } else {
        f[h++] = 0x80 | 126;
        f[h++] = (uint8_t)(n >> 8);
        f[h++] = (uint8_t)n;
    }
    uint8_t m[4];
    esp_fill_random(m, 4);
    memcpy(f + h, m, 4);
    h += 4;
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) f[h + i] = b[i] ^ m[i & 3];
    conn_t c = { .fd = w->fd, .tls = w->tls };
    const bool ok = conn_send(&c, f, h + n);
    if (ok) w->bytes_out += h + n;
    return ok;
}

bool wl_text(wl_t *w, const char *t) { return wl_send(w, 0x1, t, strlen(t)); }

/* More from the socket into rx, waiting up to `ms`: 1 had some, 0 none yet,
 * -2 the receiver closed its end, -1 failed. */
static int fill(wl_t *w, int ms)
{
    if (w->at) {
        memmove(w->rx, w->rx + w->at, w->have - w->at);
        w->have -= w->at;
        w->at = 0;
    }
    conn_t c = { .fd = w->fd, .tls = w->tls };
    const int sr = conn_wait(&c, (int64_t)ms * 1000);
    if (sr < 0) return -1;
    if (sr == 0) return 0;
    const int k = conn_recv(&c, w->rx + w->have, RX_BYTES - w->have, 0);
    if (k > 0) {
        w->have += (size_t)k;
        w->bytes_in += (uint64_t)k;
        return 1;
    }
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return k == 0 || errno == ECONNRESET ? -2 : -1;
}

bool wl_more(wl_t *w, int ms)
{
    if (w->at < w->have) return true;
    conn_t c = { .fd = w->fd, .tls = w->tls };
    return w->fd >= 0 && conn_wait(&c, (int64_t)ms * 1000) > 0;
}

int wl_next(wl_t *w, int ms, wl_piece_t *pc)
{
    if (w->fd < 0) return -1;
    const int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    for (;;) {
        while (w->at < w->have) {
            if (!w->in_payload) {
                /* The header, as it comes: two bytes, then its length's and
                 * its mask's. */
                w->hdr[w->hn++] = w->rx[w->at++];
                if (w->hn == 2) {
                    const uint8_t l7 = w->hdr[1] & 0x7F;
                    w->hl = (uint8_t)(2 + (l7 == 126 ? 2 : l7 == 127 ? 8 : 0) + (w->hdr[1] & 0x80 ? 4 : 0));
                }
                if (w->hn < 2 || w->hn < w->hl) continue;
                const uint8_t l7 = w->hdr[1] & 0x7F;
                uint64_t len = l7;
                size_t at = 2;
                if (l7 == 126) {
                    len = (uint64_t)w->hdr[2] << 8 | w->hdr[3];
                    at = 4;
                } else if (l7 == 127) {
                    len = 0;
                    for (int i = 2; i < 10; i++) len = len << 8 | w->hdr[i];
                    at = 10;
                }
                w->masked = w->hdr[1] & 0x80;                  /* never from a server, but undone if so */
                if (w->masked) memcpy(w->mask, w->hdr + at, 4);
                w->fin = w->hdr[0] & 0x80;
                w->op = w->hdr[0] & 0x0F;
                w->len = w->left = len;
                w->hn = 0;
                w->hl = 2;
                if (w->op >= 0x8) {
                    if (len > sizeof w->ctl) return -1;     /* no control frame is longer */
                    /* A control frame, whole, then handled: a ping answered,
                     * a pong let go, a close the end. */
                    size_t got = 0;
                    while (got < len) {
                        if (w->at == w->have) {
                            const int r = fill(w, 1000);
                            if (r < 0) return r;
                            if (r == 0) return -1;
                            continue;
                        }
                        const size_t n = w->have - w->at < len - got ? w->have - w->at : (size_t)(len - got);
                        memcpy(w->ctl + got, w->rx + w->at, n);
                        w->at += n;
                        got += n;
                    }
                    if (w->masked)
                        for (size_t i = 0; i < len; i++) w->ctl[i] ^= w->mask[i & 3];
                    if (w->op == 0x8) return -2;
                    if (w->op == 0x9) wl_send(w, 0xA, w->ctl, (size_t)len);
                    continue;
                }
                if (w->op != 0x0) w->msg_op = w->op;            /* a continuation: its first's */
                w->in_payload = true;
                if (!len) {
                    w->in_payload = false;
                    pc->op = w->msg_op;
                    pc->first = w->op != 0x0;
                    pc->last = w->fin;
                    pc->len = 0;
                    pc->p = w->rx + w->at;
                    pc->n = 0;
                    return 1;
                }
                continue;
            }
            /* As much of the payload as is here. */
            const size_t avail = w->have - w->at;
            const size_t n = (uint64_t)avail < w->left ? avail : (size_t)w->left;
            uint8_t *p = w->rx + w->at;
            const uint64_t off = w->len - w->left;
            if (w->masked)
                for (size_t i = 0; i < n; i++) p[i] ^= w->mask[(off + i) & 3];
            w->at += n;
            w->left -= n;
            pc->op = w->msg_op;
            pc->first = off == 0 && w->op != 0x0;
            pc->last = !w->left && w->fin;
            pc->len = w->len;
            pc->p = p;
            pc->n = n;
            if (!w->left) w->in_payload = false;
            return 1;
        }
        /* Nothing whole yet -- a header may be half here: more, within
         * what is left of `ms`. */
        const int64_t left = until - esp_timer_get_time();
        const int r = fill(w, left > 0 ? (int)((left + 999) / 1000) : 0);
        if (r <= 0) return r;
    }
}
