/* The reflector's control connection on lwIP and mbedTLS. See svx_net.h.
 * The TLS settings are SVXConnect-CLI's (src/common/tls.c): TLS 1.2 only, the
 * reflector's certificate not verified -- the trust runs the other way, the
 * reflector checking ours -- no SNI, and never a close_notify. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS    /* the alert that ended a handshake */
#include "svx_net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"

static const char *TAG = "svx-net";

/* What went each way during the TLS handshake: which side stalls, if one does. */
static size_t s_hs_out, s_hs_in;

#define IN_CAP (SVX_MAX_FRAME + 8)

static int64_t ms_now(void) { return esp_timer_get_time() / 1000; }

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int rng(void *p, unsigned char *out, size_t len)
{
    (void)p;
    esp_fill_random(out, len);
    return 0;
}

/* 1 ready, 0 timed out, -1 failed. */
static int wait_fd(int fd, bool for_write, int timeout_ms)
{
    if (timeout_ms < 0) timeout_ms = 0;
    fd_set s;
    FD_ZERO(&s);
    FD_SET(fd, &s);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    int r = select(fd + 1, for_write ? NULL : &s, for_write ? &s : NULL, NULL, &tv);
    return r > 0 ? 1 : r;
}

/* ------------------------------------------------------------------ SRV */

/* A name at `off`, following compression pointers. `*next` is where the
 * record continues after it. */
static int dns_name(const uint8_t *m, size_t len, size_t off, char *out, size_t cap,
                    size_t *next)
{
    size_t o = 0;
    bool jumped = false;
    for (int hops = 0; off < len; ) {
        const uint8_t c = m[off];
        if (c == 0) {
            if (!jumped) *next = off + 1;
            out[o] = 0;
            return 0;
        }
        if ((c & 0xC0) == 0xC0) {
            if (off + 1 >= len || ++hops > 16) return -1;
            if (!jumped) *next = off + 2;
            jumped = true;
            off = (size_t)(c & 0x3F) << 8 | m[off + 1];
            continue;
        }
        if ((c & 0xC0) || off + 1 + c > len || o + c + 2 > cap) return -1;
        if (o) out[o++] = '.';
        memcpy(out + o, m + off + 1, c);
        o += c;
        off += 1 + c;
    }
    return -1;
}

static size_t dns_query(uint8_t *q, size_t cap, uint16_t id, const char *name)
{
    memset(q, 0, 12);
    q[0] = id >> 8; q[1] = (uint8_t)id;
    q[2] = 0x01;                            /* recursion desired */
    q[5] = 1;                               /* one question */
    size_t o = 12;
    for (const char *p = name; *p; ) {
        const char *dot = strchr(p, '.');
        const size_t n = dot ? (size_t)(dot - p) : strlen(p);
        if (n == 0) break;                  /* a trailing dot */
        if (n > 63 || o + n + 6 > cap) return 0;
        q[o++] = (uint8_t)n;
        memcpy(q + o, p, n);
        o += n;
        p += n;
        if (*p == '.') p++;
    }
    q[o++] = 0;
    q[o++] = 0; q[o++] = 33;                /* SRV */
    q[o++] = 0; q[o++] = 1;                 /* IN  */
    return o;
}

/* 1 found, 0 no such record, -1 not an answer to this question. */
static int srv_parse(const uint8_t *m, size_t len, uint16_t id,
                     char *host, size_t cap, uint16_t *port)
{
    if (len < 12 || be16(m) != id || !(m[2] & 0x80)) return -1;
    const int rcode = m[3] & 0x0F;
    if (rcode == 3) return 0;               /* no such name */
    if (rcode != 0) return -1;
    const unsigned qd = be16(m + 4), an = be16(m + 6);
    size_t off = 12, next = 0;
    char name[256];
    for (unsigned i = 0; i < qd; i++) {
        if (dns_name(m, len, off, name, sizeof name, &next)) return -1;
        off = next + 4;
    }
    /* Priority ascending, then weight descending: deterministic, as the CLI
     * does it, rather than RFC 2782's weighted draw. */
    int best_prio = 0x10000, best_weight = -1, found = 0;
    for (unsigned i = 0; i < an && off < len; i++) {
        if (dns_name(m, len, off, name, sizeof name, &next)) return found;
        off = next;
        if (off + 10 > len) return found;
        const uint16_t type = be16(m + off), rdlen = be16(m + off + 8);
        off += 10;
        if (off + rdlen > len) return found;
        if (type == 33 && rdlen >= 7) {
            const int prio = be16(m + off), weight = be16(m + off + 2);
            const uint16_t p = be16(m + off + 4);
            /* An empty target, ".", means "no service here". */
            if (dns_name(m, len, off + 6, name, sizeof name, &next) == 0 && name[0] &&
                (prio < best_prio || (prio == best_prio && weight > best_weight))) {
                snprintf(host, cap, "%s", name);
                *port = p;
                best_prio = prio;
                best_weight = weight;
                found = 1;
            }
        }
        off += rdlen;
    }
    return found;
}

int svx_srv_lookup(const char *domain, char *host, size_t cap, uint16_t *port)
{
    struct in_addr lit;
    if (inet_aton(domain, &lit)) return 0;  /* an address: nothing to look up */

    char qname[160];
    snprintf(qname, sizeof qname, "_svxreflector._tcp.%s", domain);
    uint8_t *m = malloc(768);
    if (!m) return -1;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { free(m); return -1; }

    int res = -1;
    for (int s = 0; s < DNS_MAX_SERVERS && res < 0; s++) {
        const ip_addr_t *dns = dns_getserver((u8_t)s);
        if (!dns || !IP_IS_V4(dns) || ip_addr_isany(dns)) continue;
        struct sockaddr_in to = {
            .sin_family = AF_INET,
            .sin_port   = htons(53),
            .sin_addr.s_addr = ip4_addr_get_u32(ip_2_ip4(dns)),
        };
        for (int attempt = 0; attempt < 2 && res < 0; attempt++) {
            const uint16_t id = (uint16_t)esp_random();
            const size_t n = dns_query(m, 768, id, qname);
            if (!n || sendto(fd, m, n, 0, (struct sockaddr *)&to, sizeof to) < 0) break;
            const int64_t deadline = ms_now() + 1500;
            while (res < 0) {
                if (wait_fd(fd, false, (int)(deadline - ms_now())) != 1) break;
                int k = recv(fd, m, 768, 0);
                if (k <= 0) break;
                res = srv_parse(m, (size_t)k, id, host, cap, port);
            }
        }
    }
    close(fd);
    free(m);
    return res;
}

/* ------------------------------------------------------------------ TCP */

int svx_link_open(svx_link_t *l, const char *host, uint16_t port, int timeout_ms)
{
    memset(l, 0, sizeof *l);
    l->fd = -1;
    l->in = heap_caps_malloc(IN_CAP, MALLOC_CAP_SPIRAM);
    if (!l->in) return -1;

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    char ps[8];
    snprintf(ps, sizeof ps, "%u", (unsigned)port);
    int e = getaddrinfo(host, ps, &hints, &res);
    if (e != 0 || !res) {
        ESP_LOGW(TAG, "%s does not resolve (%d)", host, e);
        return -1;
    }
    for (struct addrinfo *ai = res; ai && l->fd < 0; ai = ai->ai_next) {
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) { ESP_LOGW(TAG, "no socket: errno %d", errno); break; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r != 0 && errno == EINPROGRESS && wait_fd(fd, true, timeout_ms) == 1) {
            int err = 0;
            socklen_t el = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            r = err ? -1 : 0;
        }
        if (r != 0) { close(fd); continue; }
        l->fd   = fd;
        l->addr = ((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr;
        l->port = port;
    }
    freeaddrinfo(res);
    return l->fd >= 0 ? 0 : -1;
}

void svx_link_close(svx_link_t *l)
{
    /* No close_notify: the reflector then keeps the node registered until it
     * times out. Closing the socket is the clean goodbye -- and an abortive
     * one: a plain close() of a connection with data still queued left lwIP
     * trying to deliver it for minutes, holding its buffers in internal RAM,
     * and after a few stalled logins nothing could be sent at all. */
    if (l->fd >= 0) {
        const struct linger lg = { .l_onoff = 1, .l_linger = 0 };
        setsockopt(l->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        close(l->fd);
        l->fd = -1;
    }
    if (l->tls_ready) {
        mbedtls_ssl_free(&l->ssl);
        mbedtls_ssl_config_free(&l->conf);
        mbedtls_x509_crt_free(&l->own);
        mbedtls_pk_free(&l->key);
        l->tls_ready = false;
    }
    l->tls = false;
    free(l->in);
    l->in = NULL;
    l->in_len = l->consume = 0;
}

/* ---------------------------------------------------------------- frames */

static int read_exact(int fd, uint8_t *b, size_t n, int64_t deadline)
{
    size_t got = 0;
    while (got < n) {
        int w = wait_fd(fd, false, (int)(deadline - ms_now()));
        if (w <= 0) return w;
        int k = recv(fd, b + got, n - got, 0);
        if (k == 0) return -1;
        if (k < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        got += (size_t)k;
    }
    return 1;
}

int svx_link_next(svx_link_t *l, uint16_t *type, uint8_t **payload, size_t *len)
{
    for (;;) {
        if (l->consume) {
            memmove(l->in, l->in + l->consume, l->in_len - l->consume);
            l->in_len -= l->consume;
            l->consume = 0;
        }
        if (l->in_len < 4) return 0;
        const uint32_t L = be32(l->in);
        if (L > SVX_MAX_FRAME) return -1;
        if (l->in_len < 4 + (size_t)L) return 0;
        l->consume = 4 + L;
        if (L < 2) continue;                 /* no type: skip it */
        *type    = be16(l->in + 4);
        *payload = l->in + 4;
        *len     = L;
        return 1;
    }
}

int svx_link_pump(svx_link_t *l)
{
    if (!l->tls) return 0;
    if (l->consume) {
        memmove(l->in, l->in + l->consume, l->in_len - l->consume);
        l->in_len -= l->consume;
        l->consume = 0;
    }
    while (l->in_len < IN_CAP) {
        int r = mbedtls_ssl_read(&l->ssl, l->in + l->in_len, IN_CAP - l->in_len);
        if (r > 0) { l->in_len += (size_t)r; continue; }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        if (r != 0 && r != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY && r != MBEDTLS_ERR_SSL_CONN_EOF) {
            char e[80];
            mbedtls_strerror(r, e, sizeof e);
            ESP_LOGW(TAG, "TLS read: %s (-0x%04x)", e, (unsigned)-r);
        }
        return -1;
    }
    return 0;
}

bool svx_link_readable(svx_link_t *l)
{
    return l->tls && (mbedtls_ssl_get_bytes_avail(&l->ssl) > 0 ||
                      mbedtls_ssl_check_pending(&l->ssl));
}

int svx_link_frame(svx_link_t *l, uint16_t *type, uint8_t **payload, size_t *len,
                   int timeout_ms)
{
    const int64_t deadline = ms_now() + timeout_ms;
    if (!l->tls) {
        /* Exactly one frame: the next bytes may already be TLS. */
        uint8_t h[4];
        int r = read_exact(l->fd, h, 4, deadline);
        if (r <= 0) return r;
        const uint32_t L = be32(h);
        if (L < 2 || L > SVX_MAX_FRAME) return -1;
        if (read_exact(l->fd, l->in, L, deadline) != 1) return -1;
        *type    = be16(l->in);
        *payload = l->in;
        *len     = L;
        return 1;
    }
    for (;;) {
        int k = svx_link_next(l, type, payload, len);
        if (k != 0) return k;
        const bool closed = svx_link_pump(l) < 0;
        k = svx_link_next(l, type, payload, len);
        if (k != 0) return k;
        if (closed) return -1;
        const int left = (int)(deadline - ms_now());
        if (left <= 0) return 0;
        if (!svx_link_readable(l) && wait_fd(l->fd, false, left) < 0) return -1;
    }
}

int svx_link_send(svx_link_t *l, const uint8_t *buf, size_t len, int timeout_ms)
{
    const int64_t deadline = ms_now() + timeout_ms;
    size_t off = 0;
    while (off < len) {
        int r;
        if (l->tls) {
            r = mbedtls_ssl_write(&l->ssl, buf + off, len - off);
        } else {
            r = send(l->fd, buf + off, len - off, 0);
            if (r < 0) r = (errno == EAGAIN || errno == EWOULDBLOCK)
                         ? MBEDTLS_ERR_SSL_WANT_WRITE : -1;
        }
        if (r > 0) { off += (size_t)r; continue; }
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) {
            const int left = (int)(deadline - ms_now());
            if (left <= 0 || wait_fd(l->fd, r == MBEDTLS_ERR_SSL_WANT_WRITE, left) < 0) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ TLS */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int n = send(*(int *)ctx, buf, len, 0);
    if (n > 0) s_hs_out += (size_t)n;
    if (n >= 0) return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    ESP_LOGW(TAG, "send: errno %d", errno);
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int n = recv(*(int *)ctx, buf, len, 0);
    if (n > 0) s_hs_in += (size_t)n;
    if (n >= 0) return n;                    /* 0 is the end of the stream */
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    ESP_LOGW(TAG, "recv: errno %d", errno);
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static bool self_signed(const mbedtls_x509_crt *c)
{
    return c->issuer_raw.len == c->subject_raw.len &&
           memcmp(c->issuer_raw.p, c->subject_raw.p, c->subject_raw.len) == 0;
}

static bool in_chain(const mbedtls_x509_crt *chain, const mbedtls_x509_crt *c)
{
    for (; chain && chain->raw.p; chain = chain->next)
        if (chain->raw.len == c->raw.len && memcmp(chain->raw.p, c->raw.p, c->raw.len) == 0)
            return true;
    return false;
}

/* Our certificate, and after it the CAs between it and the reflector's root:
 * the reflector trusts its root, and signs with an issuing CA under it, so it
 * needs to be shown the way up. OpenSSL, which the CLI uses, builds this from
 * the fetched bundle without being asked; mbedTLS sends exactly what it is
 * given. The leaf is the first block of `crt_pem`; the intermediates come
 * from the rest of it and from the bundle, the self-signed roots left out. */
static int own_chain(mbedtls_x509_crt *own, const char *crt_pem, const char *ca_pem)
{
    mbedtls_x509_crt all;
    mbedtls_x509_crt_init(&all);
    int r = mbedtls_x509_crt_parse(&all, (const unsigned char *)crt_pem, strlen(crt_pem) + 1);
    if (r < 0 || !all.raw.p) { mbedtls_x509_crt_free(&all); return r < 0 ? r : -1; }
    r = mbedtls_x509_crt_parse_der(own, all.raw.p, all.raw.len);
    if (r == 0 && ca_pem)
        mbedtls_x509_crt_parse(&all, (const unsigned char *)ca_pem, strlen(ca_pem) + 1);
    int n = 1;
    for (const mbedtls_x509_crt *c = all.next; r == 0 && c && c->raw.p; c = c->next) {
        if (self_signed(c) || in_chain(own, c)) continue;
        if (mbedtls_x509_crt_parse_der(own, c->raw.p, c->raw.len) == 0) n++;
    }
    mbedtls_x509_crt_free(&all);
    if (r == 0) ESP_LOGI(TAG, "presenting our certificate and %d CA%s above it", n - 1,
                         n == 2 ? "" : "s");
    return r;
}

/* The alerts that mean "not with that certificate" rather than a network
 * fault: bad, unsupported, revoked, expired or unknown certificate, unknown
 * CA, access denied, certificate required. */
static bool cert_alert(int a)
{
    return (a >= 42 && a <= 46) || a == 48 || a == 49 || a == 116;
}

int svx_link_start_tls(svx_link_t *l, const char *crt_pem, const char *key_pem,
                       const char *ca_pem, int timeout_ms)
{
    mbedtls_ssl_init(&l->ssl);
    mbedtls_ssl_config_init(&l->conf);
    mbedtls_x509_crt_init(&l->own);
    mbedtls_pk_init(&l->key);
    l->tls_ready = true;
    l->alert = 0;

    const bool offer = crt_pem && key_pem;
    int r = mbedtls_ssl_config_defaults(&l->conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
    if (r == 0) {
        mbedtls_ssl_conf_authmode(&l->conf, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&l->conf, rng, NULL);
        /* Offering 1.3 makes some reflectors drop the connection. */
        mbedtls_ssl_conf_min_tls_version(&l->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&l->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        /* No session ticket: every login is a full handshake anyway, so it
         * would only cost the reflector a kilobyte per login. */
        mbedtls_ssl_conf_session_tickets(&l->conf, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
    }
    if (r == 0 && offer) {
        r = own_chain(&l->own, crt_pem, ca_pem);
        if (r == 0) r = mbedtls_pk_parse_key(&l->key, (const unsigned char *)key_pem,
                                             strlen(key_pem) + 1, NULL, 0, rng, NULL);
        if (r == 0) r = mbedtls_ssl_conf_own_cert(&l->conf, &l->own, &l->key);
    }
    if (r == 0) r = mbedtls_ssl_setup(&l->ssl, &l->conf);
    if (r != 0) {
        char e[80];
        mbedtls_strerror(r, e, sizeof e);
        ESP_LOGE(TAG, "TLS setup: %s (-0x%04x)", e, (unsigned)-r);
        return -1;
    }
    mbedtls_ssl_set_bio(&l->ssl, &l->fd, bio_send, bio_recv, NULL);

    s_hs_out = s_hs_in = 0;
    const int64_t t0 = ms_now();
    const int64_t deadline = t0 + timeout_ms;
    int64_t flight_ms = 0;
    for (;;) {
        const int64_t ts = ms_now();
        r = mbedtls_ssl_handshake(&l->ssl);
        /* The longest single call is the one that makes our second flight --
         * certificate, key exchange, signature, Finished. */
        if (ms_now() - ts > flight_ms) flight_ms = ms_now() - ts;
        if (r == 0) break;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            const int left = (int)(deadline - ms_now());
            if (left <= 0) {
                int soerr = 0;
                socklen_t el = sizeof soerr;
                getsockopt(l->fd, SOL_SOCKET, SO_ERROR, &soerr, &el);
                ESP_LOGW(TAG, "TLS handshake timed out in state %d, waiting to %s; %u bytes "
                         "out, %u in, socket error %d, our flight made in %lld ms; internal "
                         "RAM %u free, %u in one piece, %u of it DMA-capable; stack %u unused",
                         l->ssl.state, r == MBEDTLS_ERR_SSL_WANT_WRITE ? "write" : "read",
                         (unsigned)s_hs_out, (unsigned)s_hs_in, soerr, (long long)flight_ms,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA |
                                                                    MALLOC_CAP_INTERNAL),
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
                return -1;
            }
            if (wait_fd(l->fd, r == MBEDTLS_ERR_SSL_WANT_WRITE, left) < 0) return -1;
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE && l->ssl.in_msg)
            l->alert = l->ssl.in_msg[1];
        /* A reflector that turns a certificate down may say why in an alert
         * that is still unread when our next write fails: have a look. */
        uint8_t a[7];
        if (!l->alert && recv(l->fd, a, sizeof a, MSG_DONTWAIT) == (int)sizeof a &&
            a[0] == 0x15 && a[5] == 2)
            l->alert = a[6];
        char e[80];
        mbedtls_strerror(r, e, sizeof e);
        ESP_LOGW(TAG, "TLS handshake: %s (-0x%04x)%s", e, (unsigned)-r,
                 l->alert ? ", alert from the reflector" : "");
        if (l->alert) ESP_LOGW(TAG, "the reflector's alert: %d", l->alert);
        return offer && cert_alert(l->alert) ? -2 : -1;
    }
    l->tls = true;
    ESP_LOGI(TAG, "TLS up: %s, %s%s, %lld ms (our flight %lld), %u bytes out, %u in; "
             "internal RAM %u free, %u DMA-capable in one piece; stack %u unused",
             mbedtls_ssl_get_version(&l->ssl), mbedtls_ssl_get_ciphersuite(&l->ssl),
             offer ? ", with our certificate" : ", no certificate", (long long)(ms_now() - t0),
             (long long)flight_ms, (unsigned)s_hs_out, (unsigned)s_hs_in,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return 0;
}
