/* The UberSDR's connections: see uber_net.h. */
#include "uber_net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "uber-net";

const char *unet_agent(void)
{
    static char ua[96];
    if (!ua[0])
        snprintf(ua, sizeof ua, "VFO-Knob/%s (+https://github.com/Guru-RF/VFO-Knob)",
                 esp_app_get_description()->version);
    return ua;
}

esp_tls_t *unet_connect(const uhost_t *h, int timeout_ms, char *why, size_t wn)
{
    esp_tls_t *t = esp_tls_init();
    if (!t) {
        snprintf(why, wn, "no memory");
        return NULL;
    }
    const esp_tls_cfg_t cfg = {
        .timeout_ms        = timeout_ms,
        .is_plain_tcp      = !h->tls,
        .crt_bundle_attach = h->tls ? esp_crt_bundle_attach : NULL,
    };
    if (esp_tls_conn_new_sync(h->host, (int)strlen(h->host), h->port, &cfg, t) == 1) return t;
    esp_tls_error_handle_t eh = NULL;
    esp_err_t last = ESP_FAIL;
    int flags = 0;
    if (esp_tls_get_error_handle(t, &eh) == ESP_OK && eh) {
        last  = eh->last_error;
        flags = eh->esp_tls_flags;
    }
    const char *w = last == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME ? "name not found"
                  : last == ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET     ? "no free socket"
                  : last == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST   ? "refused"
                  : last == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED     ? (flags ? "certificate refused"
                                                                              : "TLS failed")
                  : "no answer";
    ESP_LOGW(TAG, "%s:%u: %s (%s, flags 0x%x)", h->host, (unsigned)h->port, w,
             esp_err_to_name(last), flags);
    snprintf(why, wn, "%s", w);
    esp_tls_conn_destroy(t);
    return NULL;
}

/* A read: bytes, 0 once closed, -2 when there is nothing yet (a timeout on a
 * blocking socket, or nothing waiting on a non-blocking one), -1 an error.
 * esp-tls answers in its own codes for TLS, and as recv() does in the clear. */
static ssize_t rd(esp_tls_t *t, bool tls, void *b, size_t n)
{
    const ssize_t k = esp_tls_conn_read(t, b, n);
    if (tls) {
        if (k == ESP_TLS_ERR_SSL_WANT_READ || k == ESP_TLS_ERR_SSL_WANT_WRITE) return -2;
        return k < 0 ? -1 : k;
    }
    if (k < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? -2 : -1;
    return k;
}

bool unet_write(esp_tls_t *t, const void *b, size_t n)
{
    const uint8_t *p = b;
    const int64_t until = esp_timer_get_time() + 3000000;
    while (n) {
        const ssize_t k = esp_tls_conn_write(t, p, n);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
            continue;
        }
        const bool again = k == ESP_TLS_ERR_SSL_WANT_WRITE || k == ESP_TLS_ERR_SSL_WANT_READ ||
                           (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        if (!again || esp_timer_get_time() > until) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

/* ------------------------------------------------------------------ HTTP */

typedef struct {
    esp_tls_t *t;
    bool       tls;
    uint8_t    b[512];
    size_t     pos, len;
} rbuf_t;

/* A byte, or -1 once there are none (closed, an error, or the timeout). */
static int rb_getc(rbuf_t *r)
{
    if (r->pos == r->len) {
        const ssize_t k = rd(r->t, r->tls, r->b, sizeof r->b);
        if (k <= 0) return -1;
        r->pos = 0;
        r->len = (size_t)k;
    }
    return r->b[r->pos++];
}

/* A line without its CR LF; false once there is none. */
static bool rb_line(rbuf_t *r, char *line, size_t cap)
{
    size_t o = 0;
    for (;;) {
        const int c = rb_getc(r);
        if (c < 0) return false;
        if (c == '\n') break;
        if (c != '\r' && o + 1 < cap) line[o++] = (char)c;
    }
    line[o] = 0;
    return true;
}

/* Up to n bytes of the body, into dst (or dropped with dst NULL). */
static size_t rb_read(rbuf_t *r, uint8_t *dst, size_t n)
{
    size_t got = 0;
    while (got < n) {
        if (r->pos < r->len) {
            size_t k = r->len - r->pos;
            if (k > n - got) k = n - got;
            if (dst) memcpy(dst + got, r->b + r->pos, k);
            r->pos += k;
            got += k;
            continue;
        }
        const ssize_t k = rd(r->t, r->tls, r->b, sizeof r->b);
        if (k <= 0) break;
        r->pos = 0;
        r->len = (size_t)k;
    }
    return got;
}

static void host_hdr(const uhost_t *h, char *out, size_t cap)
{
    if (h->port == (h->tls ? 443 : 80)) snprintf(out, cap, "%s", h->host);
    else                                  snprintf(out, cap, "%s:%u", h->host, (unsigned)h->port);
}

/* One request on an open connection, and its answer: the status, or -1.
 * *reuse says whether the connection may carry another. */
static int http_on(esp_tls_t *t, rbuf_t *r, const uhost_t *h, bool keep, const char *method,
                   const char *path, const char *body, char *out, size_t cap, size_t *len,
                   bool *reuse, char *why, size_t wn)
{
    *reuse = false;
    if (len) *len = 0;
    char hh[80], req[640];
    host_hdr(h, hh, sizeof hh);
    const size_t bl = body ? strlen(body) : 0;
    int n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\n"
                     "Connection: %s\r\n", method, path, hh, unet_agent(), keep ? "keep-alive" : "close");
    if (body && n > 0 && n < (int)sizeof req)
        n += snprintf(req + n, sizeof req - n,
                      "Content-Type: application/json\r\nContent-Length: %u\r\n", (unsigned)bl);
    if (n > 0 && n < (int)sizeof req) n += snprintf(req + n, sizeof req - n, "\r\n");
    if (n <= 0 || n >= (int)sizeof req || !unet_write(t, req, (size_t)n) ||
        (bl && !unet_write(t, body, bl))) {
        snprintf(why, wn, "could not send");
        return -1;
    }
    char line[160];
    int status = -1;
    long clen = -1;
    bool chunked = false, closing = !keep;
    if (rb_line(r, line, sizeof line) && !strncmp(line, "HTTP/1.", 7)) status = atoi(line + 9);
    while (status > 0 && rb_line(r, line, sizeof line) && line[0]) {
        if (!strncasecmp(line, "content-length:", 15)) clen = atol(line + 15);
        else if (!strncasecmp(line, "transfer-encoding:", 18) && strcasestr(line + 18, "chunked"))
            chunked = true;
        else if (!strncasecmp(line, "connection:", 11) && strcasestr(line + 11, "close"))
            closing = true;
    }
    if (status < 0) {
        snprintf(why, wn, "no answer");
        return -1;
    }
    size_t o = 0;
    bool whole = true;
    if (chunked) {
        whole = false;
        for (;;) {
            if (!rb_line(r, line, sizeof line)) break;
            const size_t k = (size_t)strtoul(line, NULL, 16);
            if (!k) {
                rb_line(r, line, sizeof line);             /* the blank line at its end */
                whole = true;
                break;
            }
            const size_t keep_n = o < cap ? (cap - o < k ? cap - o : k) : 0;
            size_t got = rb_read(r, out ? (uint8_t *)out + o : NULL, keep_n);
            o += got;
            if (keep_n < k) got += rb_read(r, NULL, k - keep_n);
            if (got < k) break;
            rb_line(r, line, sizeof line);                 /* the chunk's CR LF */
        }
    } else if (clen >= 0) {
        const size_t want = (size_t)clen;
        o = rb_read(r, (uint8_t *)out, want < cap ? want : cap);
        if (want > cap) rb_read(r, NULL, want - cap);
        whole = o == (want < cap ? want : cap);
    } else {
        o = rb_read(r, (uint8_t *)out, cap);               /* to the close */
        closing = true;
    }
    if (out && cap) out[o < cap ? o : cap - 1] = 0;
    if (len) *len = o;
    *reuse = keep && whole && !closing;
    return status;
}

int unet_http(const uhost_t *h, const char *method, const char *path, const char *body,
              char *out, size_t cap, size_t *len, int timeout_ms, char *why, size_t wn)
{
    uconn_t c = { 0 };
    const int st = unet_http_keep(&c, h, method, path, body, out, cap, len, timeout_ms, why, wn);
    unet_close(&c);
    return st;
}

void unet_close(uconn_t *c)
{
    if (c->tls) esp_tls_conn_destroy(c->tls);
    free(c->rb);
    memset(c, 0, sizeof *c);
}

int unet_http_keep(uconn_t *c, const uhost_t *h, const char *method, const char *path,
                   const char *body, char *out, size_t cap, size_t *len, int timeout_ms,
                   char *why, size_t wn)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        const int64_t now = esp_timer_get_time();
        bool fresh = false;
        /* An idle one the server may have let go: made again rather than tried. */
        if (c->tls && now - c->last > 30 * 1000000LL) unet_close(c);
        if (!c->tls) {
            c->tls = unet_connect(h, timeout_ms, why, wn);
            if (!c->tls) return -1;
            c->rb = heap_caps_malloc(sizeof(rbuf_t), MALLOC_CAP_SPIRAM);
            if (!c->rb) {
                snprintf(why, wn, "no memory");
                unet_close(c);
                return -1;
            }
            rbuf_t *r = c->rb;
            r->t = c->tls;
            r->tls = h->tls;
            r->pos = r->len = 0;
            fresh = true;
        }
        bool reuse = false;
        const int st = http_on(c->tls, c->rb, h, true, method, path, body, out, cap, len,
                               &reuse, why, wn);
        c->last = esp_timer_get_time();
        if (st > 0) {
            if (!reuse) unet_close(c);
            return st;
        }
        unet_close(c);
        if (fresh) return -1;                  /* a new connection failing is real */
    }
    return -1;
}

/* ------------------------------------------------------------ WebSocket */

bool uws_open(uws_t *w, const uhost_t *h, const char *path, size_t rx_cap,
              int timeout_ms, char *why, size_t wn)
{
    memset(w, 0, sizeof *w);
    w->fd = -1;
    w->tls = unet_connect(h, timeout_ms, why, wn);
    if (!w->tls) return false;
    w->is_tls = h->tls;

    uint8_t key[16];
    esp_fill_random(key, sizeof key);
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char k64[25];
    int o = 0;
    for (int i = 0; i < 16; i += 3) {
        const uint32_t v = key[i] << 16 | (i + 1 < 16 ? key[i + 1] << 8 : 0) | (i + 2 < 16 ? key[i + 2] : 0);
        k64[o++] = B64[v >> 18 & 63];
        k64[o++] = B64[v >> 12 & 63];
        k64[o++] = i + 1 < 16 ? B64[v >> 6 & 63] : '=';
        k64[o++] = i + 2 < 16 ? B64[v & 63] : '=';
    }
    k64[o] = 0;
    char hh[80];
    host_hdr(h, hh, sizeof hh);
    const size_t rl = strlen(path) + strlen(hh) + strlen(unet_agent()) + 200;
    char *req = heap_caps_malloc(rl, MALLOC_CAP_SPIRAM);
    rbuf_t *r = heap_caps_malloc(sizeof *r, MALLOC_CAP_SPIRAM);
    w->rx = heap_caps_malloc(rx_cap, MALLOC_CAP_SPIRAM);
    w->cap = rx_cap;
    bool ok = false;
    if (!req || !r || !w->rx) {
        snprintf(why, wn, "no memory");
        goto done;
    }
    const int n = snprintf(req, rl,
                           "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nUpgrade: websocket\r\n"
                           "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                           "Sec-WebSocket-Version: 13\r\n\r\n", path, hh, unet_agent(), k64);
    if (n <= 0 || (size_t)n >= rl || !unet_write(w->tls, req, (size_t)n)) {
        snprintf(why, wn, "could not send");
        goto done;
    }
    r->t = w->tls;
    r->tls = h->tls;
    r->pos = r->len = 0;
    char line[160];
    if (!rb_line(r, line, sizeof line) || strncmp(line, "HTTP/1.", 7)) {
        snprintf(why, wn, "no answer");
        goto done;
    }
    const int status = atoi(line + 9);
    long clen = -1;
    while (rb_line(r, line, sizeof line) && line[0])
        if (!strncasecmp(line, "content-length:", 15)) clen = atol(line + 15);
    if (status != 101) {
        /* Refused before the upgrade: the status, and what the body says. */
        char body[96] = "";
        size_t bn = clen > 0 ? (size_t)clen : sizeof body - 1;
        if (bn > sizeof body - 1) bn = sizeof body - 1;
        bn = rb_read(r, (uint8_t *)body, bn);
        body[bn] = 0;
        for (char *c = body; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
        snprintf(why, wn, "%d %s", status, body);
        goto done;
    }
    /* Frames the server sent straight after its answer, already read. */
    if (r->len > r->pos) {
        const size_t k = r->len - r->pos < w->cap ? r->len - r->pos : w->cap;
        memcpy(w->rx, r->b + r->pos, k);
        w->have = k;
    }
    esp_tls_get_conn_sockfd(w->tls, &w->fd);
    fcntl(w->fd, F_SETFL, fcntl(w->fd, F_GETFL, 0) | O_NONBLOCK);
    ok = true;
done:
    free(req);
    free(r);
    if (!ok) {
        esp_tls_conn_destroy(w->tls);
        w->tls = NULL;
        free(w->rx);
        w->rx = NULL;
    }
    return ok;
}

static bool ws_send(uws_t *w, uint8_t op, const void *p, size_t n)
{
    if (!w->tls || n > 1024) return false;
    uint8_t f[1024 + 8];
    size_t hl = 0;
    f[hl++] = 0x80 | op;
    if (n < 126) {
        f[hl++] = 0x80 | (uint8_t)n;
    } else {
        f[hl++] = 0x80 | 126;
        f[hl++] = (uint8_t)(n >> 8);
        f[hl++] = (uint8_t)n;
    }
    uint8_t m[4];
    esp_fill_random(m, 4);
    memcpy(f + hl, m, 4);
    hl += 4;
    const uint8_t *s = p;
    for (size_t i = 0; i < n; i++) f[hl + i] = s[i] ^ m[i & 3];
    return unet_write(w->tls, f, hl + n);
}

bool uws_text(uws_t *w, const char *s)
{
    return ws_send(w, 0x1, s, strlen(s));
}

/* A complete frame at the front of rx: 1, with it handed out; 0 for more. */
static int frame(uws_t *w, uint8_t *op, const uint8_t **p, size_t *n, bool *fin)
{
    if (w->have < 2) return 0;
    const uint8_t *b = w->rx;
    size_t hl = 2;
    uint64_t len = b[1] & 0x7F;
    if (len == 126) {
        if (w->have < 4) return 0;
        len = (uint64_t)b[2] << 8 | b[3];
        hl = 4;
    } else if (len == 127) {
        if (w->have < 10) return 0;
        len = 0;
        for (int i = 2; i < 10; i++) len = len << 8 | b[i];
        hl = 10;
    }
    const bool masked = b[1] & 0x80;              /* never, from a server */
    if (masked) hl += 4;
    if (hl + len > w->cap) {
        /* Bigger than anything wanted here: let it go by. */
        ESP_LOGW(TAG, "a %llu-byte frame let go", (unsigned long long)len);
        w->skip = (size_t)(hl + len - w->have);
        w->have = 0;
        return 0;
    }
    if (w->have < hl + len) return 0;
    if (masked) {
        const uint8_t *m = b + hl - 4;
        for (size_t i = 0; i < len; i++) w->rx[hl + i] ^= m[i & 3];
    }
    *op  = b[0] & 0x0F;
    *fin = b[0] & 0x80;
    *p   = w->rx + hl;
    *n   = (size_t)len;
    w->used = hl + (size_t)len;
    return 1;
}

int uws_recv(uws_t *w, int timeout_ms, uint8_t *op, const uint8_t **p, size_t *n)
{
    if (!w->tls) return -1;
    const bool tls = w->is_tls;
    const int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        if (w->used) {
            memmove(w->rx, w->rx + w->used, w->have - w->used);
            w->have -= w->used;
            w->used = 0;
        }
        bool fin = true;
        if (!w->skip && frame(w, op, p, n, &fin)) {
            if (*op == 0x9) {                      /* ping: the pong, here */
                ws_send(w, 0xA, *p, *n > 125 ? 125 : *n);
                continue;
            }
            if (*op == 0xA) continue;              /* a pong */
            if (*op == 0x8) {
                w->close_code = *n >= 2 ? (uint16_t)((*p)[0] << 8 | (*p)[1]) : 1005;
                return -1;
            }
            if (!fin || *op == 0x0) {
                /* Fragments: gorilla splits nothing this client asks for. */
                ESP_LOGW(TAG, "a fragmented message let go (op %u)", *op);
                continue;
            }
            return 1;
        }
        /* More from the network, as long as the time allows. */
        const int64_t left = until - esp_timer_get_time();
        const bool held = tls && esp_tls_get_bytes_avail(w->tls) > 0;
        if (!held) {
            if (left <= 0) return 0;
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(w->fd, &rs);
            struct timeval tv = { .tv_sec = (time_t)(left / 1000000), .tv_usec = (suseconds_t)(left % 1000000) };
            const int s = select(w->fd + 1, &rs, NULL, NULL, &tv);
            if (s < 0) return -1;
            if (s == 0) return 0;
        }
        uint8_t *dst = w->skip ? w->rx : w->rx + w->have;
        const size_t room = w->skip ? (w->skip < w->cap ? w->skip : w->cap) : w->cap - w->have;
        if (!room) return -1;                       /* cannot happen: frame() let it go */
        const ssize_t k = rd(w->tls, tls, dst, room);
        if (k == -2) continue;
        if (k <= 0) return -1;
        if (w->skip) w->skip -= (size_t)k;
        else         w->have += (size_t)k;
    }
}

void uws_close(uws_t *w)
{
    if (w->tls) {
        const uint8_t bye[2] = { 0x03, 0xE8 };     /* 1000: normal */
        ws_send(w, 0x8, bye, 2);
        esp_tls_conn_destroy(w->tls);
    }
    free(w->rx);
    memset(w, 0, sizeof *w);
    w->fd = -1;
}
