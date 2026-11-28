/* A WebSocket client for AISStream.io's stream. See ais_ws.h.
 *
 * permessage-deflate (RFC 7692) is offered with neither side's context taken
 * over, so that each compressed message inflates alone; a server that keeps
 * its context anyway is followed too, its last 32 kB kept as the window. The
 * client never compresses what it sends: a message with RSV1 clear is plain
 * under the extension as well. */
#include "ais_ws.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "kiwi_tls.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#ifdef ESP_PLATFORM
#include "miniz.h"            /* the ROM's tinfl */
#else
#include <zlib.h>
#endif

static const char *TAG = "ais_ws";

#define RAW_CAP    (8 * 1024)      /* off the wire, ahead of the frames read */
#define MSG_CAP    (32 * 1024)     /* a message as it came, its fragments joined */
#define OUT_CAP    (64 * 1024)     /* ...and inflated */
#define DICT       32768           /* deflate's window, a power of two (tinfl's ring) */
#define SLICE_MS   100
#define CONNECT_MS 8000            /* the connection, the handshake, the upgrade's answer, each */
#define FRAME_MS   10000           /* a frame begun, the rest of it */
#define CTRL_MAX   125
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL    /* a closed far end an error, not a signal (the PC's) */
#else
#define SEND_FLAGS 0
#endif

struct ais_ws {
    int         fd;
    kiwi_tls_t *tls;
    bool        up;                /* upgraded: frames from here */
    bool        deflate, keeps;    /* the server compresses; ...and keeps its context */
    uint8_t    *raw;
    size_t      raw_at, raw_have;
    uint8_t    *msg;
    size_t      msg_len;
    bool        in_msg, msg_z, msg_over;
    uint8_t    *out;
#ifdef ESP_PLATFORM
    tinfl_decompressor *inf;
    uint8_t    *dict;
    size_t      dict_ofs;
#else
    z_stream    z;
    bool        z_ok;
#endif
    int64_t     heard_us;
    uint64_t    wire, plain;
    uint32_t    dropped;
    bool      (*go_on)(void *);
    void       *ctx;
};

static int64_t now_us(void) { return esp_timer_get_time(); }

static bool going(ais_ws_t *w) { return !w->go_on || w->go_on(w->ctx); }

/* The socket readable (or writable) within ms: 1, 0 not, -1 failed. */
static int wait_fd(int fd, bool wr, int ms)
{
    fd_set fs;
    FD_ZERO(&fs);
    FD_SET(fd, &fs);
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    const int r = select(fd + 1, wr ? NULL : &fs, wr ? &fs : NULL, NULL, &tv);
    return r < 0 ? (errno == EINTR ? 0 : -1) : r > 0;
}

/* ------------------------------------------------------------- the bytes */

static bool put(ais_ws_t *w, const void *b, size_t n)
{
    const uint8_t *p = b;
    const int64_t until = now_us() + CONNECT_MS * 1000LL;
    while (n) {
        int k;
        bool wr = true;
        if (w->tls) {
            k = kiwi_tls_write(w->tls, p, n, &wr);
            if (k < 0) return false;
        } else {
            k = (int)send(w->fd, p, n, SEND_FLAGS);
            if (k < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
                k = 0;
            }
        }
        if (k > 0) {
            p += k;
            n -= (size_t)k;
            continue;
        }
        if (now_us() > until || wait_fd(w->fd, wr, SLICE_MS) < 0) return false;
    }
    return true;
}

/* More off the wire, waiting up to `ms`: 1 some came, 0 none, -2 the far
 * end closed, -1 failed. */
static int fill(ais_ws_t *w, int ms)
{
    if (w->raw_at && w->raw_at == w->raw_have) w->raw_at = w->raw_have = 0;
    if (w->raw_have == RAW_CAP) {
        memmove(w->raw, w->raw + w->raw_at, w->raw_have - w->raw_at);
        w->raw_have -= w->raw_at;
        w->raw_at = 0;
        if (w->raw_have == RAW_CAP) return 1;
    }
    const int64_t until = now_us() + ms * 1000LL;
    for (;;) {
        if (!(w->tls && kiwi_tls_pending(w->tls))) {
            const int64_t left = (until - now_us()) / 1000;
            const int r = wait_fd(w->fd, false, left <= 0 ? 0 : left < SLICE_MS ? (int)left : SLICE_MS);
            if (r < 0) return -1;
            if (!r) {
                if (left <= 0 || !going(w)) return 0;
                continue;
            }
        }
        int k;
        uint8_t *at = w->raw + w->raw_have;
        const size_t room = RAW_CAP - w->raw_have;
        if (w->tls) {
            k = kiwi_tls_read(w->tls, at, room);
            if (k == -2) return -2;
            if (k < 0) return -1;
        } else {
            k = (int)recv(w->fd, at, room, 0);
            if (k == 0) return -2;
            if (k < 0) {
                if (errno == ECONNRESET) return -2;
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
                k = 0;
            }
        }
        if (k > 0) {
            w->raw_have += (size_t)k;
            w->wire += (uint64_t)k;
            w->heard_us = now_us();
            return 1;
        }
        if (now_us() >= until) return 0;
    }
}

/* n bytes into dst (NULL: let go by), by `until`: 1, 0 not in time, -2, -1. */
static int take(ais_ws_t *w, uint8_t *dst, size_t n, int64_t until)
{
    while (n) {
        if (w->raw_at == w->raw_have) {
            const int64_t left = (until - now_us()) / 1000;
            if (left <= 0) return 0;
            const int r = fill(w, (int)left);
            if (r <= 0) return r;
            continue;
        }
        size_t k = w->raw_have - w->raw_at;
        if (k > n) k = n;
        if (dst) {
            memcpy(dst, w->raw + w->raw_at, k);
            dst += k;
        }
        w->raw_at += k;
        n -= k;
    }
    return 1;
}

/* ------------------------------------------------------------- connecting */

static bool parse_url(const char *url, bool *tls, char *host, size_t hn, uint16_t *port, char *path, size_t pn)
{
    const char *p;
    if (!strncmp(url, "wss://", 6))      { *tls = true;  p = url + 6; }
    else if (!strncmp(url, "ws://", 5))  { *tls = false; p = url + 5; }
    else return false;
    const char *slash = strchr(p, '/'), *end = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(end - p));
    const size_t hl = (size_t)((colon ? colon : end) - p);
    if (!hl || hl >= hn) return false;
    memcpy(host, p, hl);
    host[hl] = 0;
    *port = *tls ? 443 : 80;
    if (colon) {
        const long v = strtol(colon + 1, NULL, 10);
        if (v <= 0 || v > 65535) return false;
        *port = (uint16_t)v;
    }
    snprintf(path, pn, "%s", slash ? slash : "/");
    return true;
}

static int tcp_open(const char *host, uint16_t port, ais_ws_t *w, char *why, size_t wn)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    char ps[8];
    snprintf(ps, sizeof ps, "%u", (unsigned)port);
    if (getaddrinfo(host, ps, &hints, &ai) != 0 || !ai) {
        snprintf(why, wn, "name not found");
        return -1;
    }
    struct sockaddr_in sa;
    memcpy(&sa, ai->ai_addr, sizeof sa);
    freeaddrinfo(ai);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(why, wn, "no socket");
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int r = connect(fd, (const struct sockaddr *)&sa, sizeof sa);
    if (r != 0 && errno != EINPROGRESS) {
        close(fd);
        snprintf(why, wn, "no route");
        return -1;
    }
    for (int left = CONNECT_MS; r != 0; left -= SLICE_MS) {
        if (left <= 0 || !going(w)) {
            close(fd);
            snprintf(why, wn, left <= 0 ? "no answer" : "stopped");
            return -1;
        }
        const int k = wait_fd(fd, true, SLICE_MS);
        if (k < 0) break;
        if (k) {
            int e = 0;
            socklen_t el = sizeof e;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
            if (e) {
                close(fd);
                snprintf(why, wn, "no answer");
                return -1;
            }
            r = 0;
        }
    }
    if (r != 0) {
        close(fd);
        snprintf(why, wn, "no answer");
        return -1;
    }
    return fd;
}

static bool tls_open(ais_ws_t *w, const char *host, uint16_t port, char *why, size_t wn)
{
    uint32_t key = 2166136261u;                /* the kept session's: FNV-1a of host and port */
    for (const char *p = host; *p; p++) key = (key ^ (uint8_t)*p) * 16777619u;
    key = (key ^ port) * 16777619u;
    w->tls = kiwi_tls_new(w->fd, host, key | 1);
    if (!w->tls) {
        snprintf(why, wn, "no memory");
        return false;
    }
    const int64_t until = now_us() + CONNECT_MS * 1000LL * 2;
    for (;;) {
        bool wr = false;
        const int k = kiwi_tls_shake(w->tls, &wr);
        if (k == 1) return true;
        if (k < 0) {
            char t[64];
            kiwi_tls_why(w->tls, t, sizeof t);
            ESP_LOGW(TAG, "%s: TLS: %s", host, t);
            snprintf(why, wn, kiwi_tls_cert_refused(w->tls) ? "certificate" : "TLS failed");
            return false;
        }
        if (!going(w)) {
            snprintf(why, wn, "stopped");
            return false;
        }
        if (now_us() > until) {
            snprintf(why, wn, "TLS timed out");
            return false;
        }
        if (k == 0 && wait_fd(w->fd, wr, SLICE_MS) < 0) {
            snprintf(why, wn, "TLS failed");
            return false;
        }
    }
}

static void b64(const uint8_t *in, size_t n, char *out)
{
    static const char B[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = B[v >> 18 & 63];
        out[o++] = B[v >> 12 & 63];
        out[o++] = i + 1 < n ? B[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B[v & 63] : '=';
    }
    out[o] = 0;
}

static bool has_ci(const char *s, const char *word)
{
    const size_t n = strlen(word);
    for (; *s; s++)
        if (!strncasecmp(s, word, n)) return true;
    return false;
}

/* The upgrade asked, and its answer's headers read to the blank line. */
static bool upgrade(ais_ws_t *w, const char *host, uint16_t port, bool tls, const char *path, bool deflate,
                    const char *ua, char *why, size_t wn)
{
    uint8_t k[16];
    char k64[25], hh[96], req[640];
    esp_fill_random(k, sizeof k);
    b64(k, sizeof k, k64);
    if ((tls && port == 443) || (!tls && port == 80)) snprintf(hh, sizeof hh, "%s", host);
    else                                              snprintf(hh, sizeof hh, "%s:%u", host, (unsigned)port);
    const int n = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s"
        "User-Agent: %s\r\n\r\n", path, hh, k64,
        deflate ? "Sec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover; "
                  "server_no_context_takeover\r\n" : "",
        ua && *ua ? ua : "VFO-Knob");
    if (n <= 0 || n >= (int)sizeof req || !put(w, req, (size_t)n)) {
        snprintf(why, wn, "cut off");
        return false;
    }
    const int64_t until = now_us() + CONNECT_MS * 1000LL;
    char line[256];
    size_t got = 0;
    int code = -1;
    for (int i = 0; i < 8192; i++) {
        uint8_t c;
        const int r = take(w, &c, 1, until);
        if (r <= 0) {
            snprintf(why, wn, r == 0 ? "no upgrade" : "cut off");
            return false;
        }
        if (c != '\n') {
            if (got < sizeof line - 1) line[got++] = (char)c;
            continue;
        }
        while (got && line[got - 1] == '\r') got--;
        line[got] = 0;
        got = 0;
        if (code < 0) {
            code = strlen(line) >= 12 && !strncmp(line, "HTTP/1.", 7) ? atoi(line + 9) : 0;
            continue;
        }
        if (!line[0]) {
            if (code != 101) {
                snprintf(why, wn, "HTTP %d", code);
                return false;
            }
            w->up = true;
            return true;
        }
        if (!strncasecmp(line, "sec-websocket-extensions:", 25) && has_ci(line + 25, "permessage-deflate")) {
            w->deflate = true;
            w->keeps = !has_ci(line + 25, "server_no_context_takeover");
        }
    }
    snprintf(why, wn, "no upgrade");
    return false;
}

static bool inflater(ais_ws_t *w)
{
#ifdef ESP_PLATFORM
    w->inf = heap_caps_malloc(sizeof *w->inf, MALLOC_CAP_SPIRAM);
    w->dict = heap_caps_malloc(DICT, MALLOC_CAP_SPIRAM);
    if (!w->inf || !w->dict) return false;
    tinfl_init(w->inf);
    w->dict_ofs = 0;
    return true;
#else
    memset(&w->z, 0, sizeof w->z);
    w->z_ok = inflateInit2(&w->z, -15) == Z_OK;
    return w->z_ok;
#endif
}

ais_ws_t *ais_ws_open(const char *url, bool deflate, const char *ua, bool (*go_on)(void *), void *ctx,
                      char *why, size_t wn)
{
    bool tls;
    char host[64], path[128];
    uint16_t port;
    if (!parse_url(url, &tls, host, sizeof host, &port, path, sizeof path)) {
        snprintf(why, wn, "no address");
        return NULL;
    }
    ais_ws_t *w = heap_caps_calloc(1, sizeof *w, MALLOC_CAP_SPIRAM);
    if (!w) {
        snprintf(why, wn, "no memory");
        return NULL;
    }
    w->fd = -1;
    w->go_on = go_on;
    w->ctx = ctx;
    w->raw = heap_caps_malloc(RAW_CAP, MALLOC_CAP_SPIRAM);
    w->msg = heap_caps_malloc(MSG_CAP + 8, MALLOC_CAP_SPIRAM);
    w->out = heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM);
    if (!w->raw || !w->msg || !w->out) {
        snprintf(why, wn, "no memory");
        ais_ws_close(w);
        return NULL;
    }
    w->fd = tcp_open(host, port, w, why, wn);
    if (w->fd < 0 || (tls && !tls_open(w, host, port, why, wn)) ||
        !upgrade(w, host, port, tls, path, deflate, ua, why, wn)) {
        ais_ws_close(w);
        return NULL;
    }
    if (w->deflate && !inflater(w)) {
        snprintf(why, wn, "no memory");
        ais_ws_close(w);
        return NULL;
    }
    w->heard_us = now_us();
    ESP_LOGI(TAG, "%s: open%s%s%s", host, tls ? ", TLS " : "", tls ? kiwi_tls_suite(w->tls) : "",
             w->deflate ? (w->keeps ? ", deflate (context kept)" : ", deflate") : ", uncompressed");
    return w;
}

/* ------------------------------------------------------------- frames */

static bool send_frame(ais_ws_t *w, uint8_t op, const uint8_t *t, size_t n)
{
    uint8_t h[14];
    size_t hl = 0;
    h[hl++] = 0x80 | op;
    if (n < 126) {
        h[hl++] = 0x80 | (uint8_t)n;
    } else if (n < 65536) {
        h[hl++] = 0x80 | 126;
        h[hl++] = (uint8_t)(n >> 8);
        h[hl++] = (uint8_t)n;
    } else {
        return false;
    }
    uint8_t m[4];
    esp_fill_random(m, 4);
    memcpy(h + hl, m, 4);
    hl += 4;
    if (!put(w, h, hl)) return false;
    uint8_t b[256];
    for (size_t i = 0; i < n;) {
        size_t k = n - i < sizeof b ? n - i : sizeof b;
        for (size_t j = 0; j < k; j++) b[j] = t[i + j] ^ m[(i + j) & 3];
        if (!put(w, b, k)) return false;
        i += k;
    }
    return true;
}

bool ais_ws_send_text(ais_ws_t *w, const char *t, size_t n) { return w && send_frame(w, 0x1, (const uint8_t *)t, n); }

bool ais_ws_ping(ais_ws_t *w) { return w && send_frame(w, 0x9, NULL, 0); }

/* A compressed message inflated into w->out: its length, or -1. */
static long inflate_msg(ais_ws_t *w)
{
    static const uint8_t TAIL[4] = { 0, 0, 0xFF, 0xFF };
    memcpy(w->msg + w->msg_len, TAIL, 4);
    const size_t in_n = w->msg_len + 4;
#ifdef ESP_PLATFORM
    if (!w->keeps) {
        tinfl_init(w->inf);
        w->dict_ofs = 0;
    }
    const uint8_t *in = w->msg;
    size_t left = in_n, o = 0;
    for (;;) {
        size_t in_sz = left, out_sz = DICT - w->dict_ofs;
        const tinfl_status st = tinfl_decompress(w->inf, in, &in_sz, w->dict, w->dict + w->dict_ofs, &out_sz,
                                                 TINFL_FLAG_HAS_MORE_INPUT);
        if (o + out_sz >= OUT_CAP) {
            tinfl_init(w->inf);                /* the window lost: a server keeping it must start over */
            w->dict_ofs = 0;
            return -1;
        }
        memcpy(w->out + o, w->dict + w->dict_ofs, out_sz);
        o += out_sz;
        w->dict_ofs = (w->dict_ofs + out_sz) & (DICT - 1);
        in += in_sz;
        left -= in_sz;
        if (st < 0) {
            tinfl_init(w->inf);
            w->dict_ofs = 0;
            return -1;
        }
        if (st == TINFL_STATUS_DONE) {         /* a final block: the next message a stream of its own */
            tinfl_init(w->inf);
            break;
        }
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && !left) break;
    }
    w->out[o] = 0;
    return (long)o;
#else
    if (!w->keeps) inflateReset(&w->z);
    w->z.next_in = w->msg;
    w->z.avail_in = (uInt)in_n;
    w->z.next_out = w->out;
    w->z.avail_out = OUT_CAP - 1;
    const int r = inflate(&w->z, Z_SYNC_FLUSH);
    if ((r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) || (w->z.avail_in && !w->z.avail_out)) {
        inflateReset(&w->z);
        return -1;
    }
    const size_t o = OUT_CAP - 1 - w->z.avail_out;
    if (r == Z_STREAM_END) inflateReset(&w->z);
    w->out[o] = 0;
    return (long)o;
#endif
}

int ais_ws_recv(ais_ws_t *w, int ms, const char **msg, size_t *len, char *why, size_t wn)
{
    const int64_t until = now_us() + (int64_t)ms * 1000;
    for (;;) {
        if (w->raw_at == w->raw_have) {
            const int64_t left = (until - now_us()) / 1000;
            if (left <= 0) return AIS_WS_NONE;
            const int r = fill(w, (int)left);
            if (r == 0) return AIS_WS_NONE;
            if (r < 0) {
                snprintf(why, wn, r == -2 ? "closed" : "cut off");
                return r == -2 ? AIS_WS_CLOSED : AIS_WS_FAILED;
            }
            continue;
        }
        /* A frame begun: the whole of it, whatever the caller's wait. */
        const int64_t fu = now_us() + FRAME_MS * 1000LL;
        uint8_t h[2];
        int r = take(w, h, 2, fu);
        uint64_t n = h[1] & 0x7F;
        if (r > 0 && n >= 126) {
            uint8_t e[8];
            const size_t el = n == 126 ? 2 : 8;
            r = take(w, e, el, fu);
            n = 0;
            for (size_t i = 0; i < el; i++) n = n << 8 | e[i];
        }
        uint8_t mk[4] = { 0 };
        const bool masked = h[1] & 0x80;
        if (r > 0 && masked) r = take(w, mk, 4, fu);
        if (r <= 0) {
            snprintf(why, wn, r == -2 ? "closed" : "cut off");
            return r == -2 ? AIS_WS_CLOSED : AIS_WS_FAILED;
        }
        const uint8_t op = h[0] & 0x0F;
        const bool fin = h[0] & 0x80, rsv1 = h[0] & 0x40;
        if (op >= 0x8) {                       /* a control frame: whole, short */
            uint8_t c[CTRL_MAX];
            if (!fin || n > CTRL_MAX || (r = take(w, c, (size_t)n, fu)) <= 0) {
                snprintf(why, wn, "bad frame");
                return AIS_WS_FAILED;
            }
            if (masked) for (size_t i = 0; i < n; i++) c[i] ^= mk[i & 3];
            if (op == 0x9) {
                if (!send_frame(w, 0xA, c, (size_t)n)) {
                    snprintf(why, wn, "cut off");
                    return AIS_WS_FAILED;
                }
            } else if (op == 0x8) {
                const int code = n >= 2 ? c[0] << 8 | c[1] : 1005;
                snprintf(why, wn, "closed %d%s%.*s", code, n > 2 ? " " : "", n > 2 ? (int)(n - 2) : 0,
                         (const char *)c + 2);
                send_frame(w, 0x8, c, n >= 2 ? 2 : 0);
                return AIS_WS_CLOSED;
            }
            continue;                          /* a pong: heard, and nothing else */
        }
        if (op == 0x1 || op == 0x2) {
            if (w->in_msg) {
                snprintf(why, wn, "bad frame");
                return AIS_WS_FAILED;
            }
            w->in_msg = true;
            w->msg_z = rsv1 && w->deflate;
            w->msg_len = 0;
            w->msg_over = false;
        } else if (op != 0x0 || !w->in_msg) {
            snprintf(why, wn, "bad frame");
            return AIS_WS_FAILED;
        }
        if (!w->msg_over && w->msg_len + n <= MSG_CAP) {
            r = take(w, w->msg + w->msg_len, (size_t)n, fu);
            if (masked) for (size_t i = 0; i < n; i++) w->msg[w->msg_len + i] ^= mk[i & 3];
            w->msg_len += (size_t)n;
        } else {
            w->msg_over = true;
            r = take(w, NULL, (size_t)n, fu);
        }
        if (r <= 0) {
            snprintf(why, wn, r == -2 ? "closed" : "cut off");
            return r == -2 ? AIS_WS_CLOSED : AIS_WS_FAILED;
        }
        if (!fin) continue;
        w->in_msg = false;
        if (w->msg_over) {
            w->dropped++;
            continue;
        }
        if (w->msg_z) {
            const long o = inflate_msg(w);
            if (o < 0) {
                w->dropped++;
                continue;
            }
            *msg = (const char *)w->out;
            *len = (size_t)o;
        } else {
            w->msg[w->msg_len] = 0;
            *msg = (const char *)w->msg;
            *len = w->msg_len;
        }
        w->plain += *len;
        return AIS_WS_MSG;
    }
}

bool    ais_ws_deflated(const ais_ws_t *w) { return w && w->deflate; }
int64_t ais_ws_heard_us(const ais_ws_t *w) { return w ? w->heard_us : 0; }

void ais_ws_counts(const ais_ws_t *w, uint64_t *wire, uint64_t *plain, uint32_t *dropped)
{
    *wire = w ? w->wire : 0;
    *plain = w ? w->plain : 0;
    *dropped = w ? w->dropped : 0;
}

void ais_ws_close(ais_ws_t *w)
{
    if (!w) return;
    if (w->fd >= 0) {
        static const uint8_t BYE[2] = { 0x03, 0xE8 };     /* 1000, a normal close */
        if (w->up) send_frame(w, 0x8, BYE, 2);
        if (w->tls) kiwi_tls_free(w->tls);
        close(w->fd);
    }
#ifdef ESP_PLATFORM
    heap_caps_free(w->inf);
    heap_caps_free(w->dict);
#else
    if (w->z_ok) inflateEnd(&w->z);
#endif
    heap_caps_free(w->raw);
    heap_caps_free(w->msg);
    heap_caps_free(w->out);
    heap_caps_free(w);
}
