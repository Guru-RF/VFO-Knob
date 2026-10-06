/* TLS for a receiver behind a front. See kiwi_tls.h.
 *
 * mbedTLS as esp-tls drives it -- a client, the certificate required, the
 * bundle's verify callback -- but on the caller's own socket and without
 * esp-tls's 2 kB context in internal RAM: the contexts are one PSRAM block
 * here. TLS 1.2, as the firmware's mbedTLS is built.
 *
 * The bundle's callback refuses a chain no authority in it signed with
 * MBEDTLS_ERR_X509_CERT_VERIFY_FAILED, which mbedTLS makes a fatal error, its
 * flags all set; a certificate for another name fails as CERT_VERIFY_FAILED
 * itself. Either way the verify result says it was the certificate. */
#include "kiwi_tls.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

/* A handshake step this long has kept its core: the idle task's turn after
 * it, before any connection's next. Another connection's step under way:
 * looked at again this often. */
#define LONG_STEP_US  (20 * 1000LL)
#define STEP_WAIT_MS  20

struct kiwi_tls {
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context  ent;
    int                      fd;
    uint32_t                 key;       /* the receiver's, for its kept session */
    int                      err;       /* mbedTLS's, the last that failed it */
    bool                     done;      /* the handshake over */
    bool                     stepped;   /* ...begun: err may be its */
    int64_t                  work_us;   /* in its steps */
    int64_t                  turn_us;   /* ...and waiting for another connection's */
};

/* The newest receivers' sessions, for resumption: each one's own place, kept
 * whole -- its ticket or its id, the certificate it was verified with. A
 * place is taken under the lock and copied outside it, as mbedTLS allocates
 * while it copies: one another task has taken is let be. */
#define KEPT_N 4
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static struct {
    uint32_t            key;            /* 0: an empty place */
    uint32_t            seq;            /* how recent */
    bool                ok, busy;
    mbedtls_ssl_session s;
} s_kept[KEPT_N];
static uint32_t s_seq;
/* The connection whose handshake step runs now, of all of them (under
 * s_mux): one at a time. */
EXT_RAM_BSS_ATTR static const kiwi_tls_t *s_stepping;

/* ------------------------------------------------------------- the socket */

static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    const int k = send(((kiwi_tls_t *)ctx)->fd, b, n, 0);
    if (k >= 0) return k;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (errno == EPIPE || errno == ECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    const int k = recv(((kiwi_tls_t *)ctx)->fd, b, n, 0);
    if (k >= 0) return k;                       /* 0: the far end closed its side */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
    if (errno == EPIPE || errno == ECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* ------------------------------------------------------- kept sessions */

/* A place in s_kept, taken: the key's own -- with a session in it, `own` --
 * or, to keep one where it has none, an empty place, else the oldest. Never
 * one another task has: -1, and nothing done. */
static int take(uint32_t key, bool own)
{
    int at = -1;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < KEPT_N && at < 0; i++)
        if (s_kept[i].key == key) at = i;
    for (int i = 0; i < KEPT_N && at < 0 && !own; i++)
        if (!s_kept[i].key && !s_kept[i].busy) at = i;
    for (int i = 0; i < KEPT_N && !own && (at < 0 || s_kept[at].key != key); i++)
        if (!s_kept[i].busy && (at < 0 || (s_kept[at].key && s_kept[i].seq < s_kept[at].seq))) at = i;
    if (at >= 0 && (s_kept[at].busy || (own && !s_kept[at].ok))) at = -1;
    if (at >= 0) s_kept[at].busy = true;
    portEXIT_CRITICAL(&s_mux);
    return at;
}

static void give(int at, uint32_t key, bool ok)
{
    portENTER_CRITICAL(&s_mux);
    s_kept[at].key = ok ? key : 0;
    s_kept[at].ok = ok;
    s_kept[at].seq = ++s_seq;
    s_kept[at].busy = false;
    portEXIT_CRITICAL(&s_mux);
}

/* Its session offered, where one is kept: the front may take it back. */
static void offer(kiwi_tls_t *t)
{
    const int at = take(t->key, true);
    if (at < 0) return;
    const bool ok = mbedtls_ssl_set_session(&t->ssl, &s_kept[at].s) == 0;
    portENTER_CRITICAL(&s_mux);
    s_kept[at].busy = false;
    portEXIT_CRITICAL(&s_mux);
    (void)ok;                                   /* refused: a whole handshake, as without */
}

/* The session just made, kept for the next connection to it. */
static void keep(kiwi_tls_t *t)
{
    const int at = take(t->key, false);
    if (at < 0) return;
    mbedtls_ssl_session_free(&s_kept[at].s);
    mbedtls_ssl_session_init(&s_kept[at].s);
    give(at, t->key, mbedtls_ssl_get_session(&t->ssl, &s_kept[at].s) == 0);
}

/* One that failed is not offered again. */
static void forget(uint32_t key)
{
    const int at = take(key, true);
    if (at < 0) return;
    mbedtls_ssl_session_free(&s_kept[at].s);
    mbedtls_ssl_session_init(&s_kept[at].s);
    give(at, key, false);
}

/* ------------------------------------------------------- one step at a time */

/* The turn for a handshake step: true, this connection's -- false, another's
 * step runs now. */
static bool step_take(const kiwi_tls_t *t)
{
    portENTER_CRITICAL(&s_mux);
    const bool mine = !s_stepping || s_stepping == t;
    if (mine) s_stepping = t;
    portEXIT_CRITICAL(&s_mux);
    return mine;
}

static void step_give(const kiwi_tls_t *t)
{
    portENTER_CRITICAL(&s_mux);
    if (s_stepping == t) s_stepping = NULL;
    portEXIT_CRITICAL(&s_mux);
}

/* ------------------------------------------------------------- kiwi_tls.h */

kiwi_tls_t *kiwi_tls_new(int fd, const char *host, uint32_t key)
{
    kiwi_tls_t *t = heap_caps_calloc(1, sizeof *t, MALLOC_CAP_SPIRAM);
    if (!t) return NULL;
    t->fd = fd;
    t->key = key;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_entropy_init(&t->ent);
    int e = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->ent, NULL, 0);
    if (!e) e = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                            MBEDTLS_SSL_PRESET_DEFAULT);
    if (!e) {
        mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
        /* No bundle: the knob's own trouble, not the receiver's certificate. */
        if (esp_crt_bundle_attach(&t->conf) != ESP_OK) e = MBEDTLS_ERR_SSL_BAD_CONFIG;
    }
    if (!e) e = mbedtls_ssl_setup(&t->ssl, &t->conf);
    if (!e) e = mbedtls_ssl_set_hostname(&t->ssl, host);
    if (!e) {
        mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
        offer(t);
    }
    t->err = e;
    return t;
}

int kiwi_tls_shake(kiwi_tls_t *t, bool *wr)
{
    if (t->err) return -1;
    if (t->done) return 1;
    /* Another connection's step: its turn first. */
    if (!step_take(t)) {
        const int64_t w0 = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(STEP_WAIT_MS));
        t->turn_us += esp_timer_get_time() - w0;
        return 2;
    }
    t->stepped = true;
    const int64_t t0 = esp_timer_get_time();
    const int r = mbedtls_ssl_handshake_step(&t->ssl);
    const int64_t dt = esp_timer_get_time() - t0;
    t->work_us += dt;
    /* A long one -- the server's certificates checked, the key reckoned: the
     * idle task's turn, with no other connection's step begun meanwhile. */
    if (dt >= LONG_STEP_US) vTaskDelay(1);
    step_give(t);
    if (r == 0) {
        if (!mbedtls_ssl_is_handshake_over(&t->ssl)) return 2;
        t->done = true;
        keep(t);
        return 1;
    }
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        if (wr) *wr = r == MBEDTLS_ERR_SSL_WANT_WRITE;
        return 0;
    }
    t->err = r;
    forget(t->key);
    return -1;
}

int kiwi_tls_read(kiwi_tls_t *t, void *b, size_t n)
{
    const int r = mbedtls_ssl_read(&t->ssl, b, n);
    if (r > 0) return r;
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_NET_CONN_RESET ||
        r == MBEDTLS_ERR_SSL_CONN_EOF)
        return -2;
    t->err = r;
    return -1;
}

int kiwi_tls_write(kiwi_tls_t *t, const void *b, size_t n, bool *wr)
{
    const int r = mbedtls_ssl_write(&t->ssl, b, n);
    if (r > 0) return r;
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        if (wr) *wr = r == MBEDTLS_ERR_SSL_WANT_WRITE;
        return 0;
    }
    t->err = r;
    return -1;
}

size_t kiwi_tls_pending(kiwi_tls_t *t) { return t ? mbedtls_ssl_get_bytes_avail(&t->ssl) : 0; }

bool kiwi_tls_cert_refused(const kiwi_tls_t *t)
{
    /* Its handshake failed with the certificate's check failed: flags for
     * what it failed on -- or all of them, the bundle's callback refusing a
     * chain none of its authorities signed. */
    return t && t->stepped && t->err && !t->done && mbedtls_ssl_get_verify_result(&t->ssl) != 0;
}

void kiwi_tls_why(const kiwi_tls_t *t, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = 0;
    if (!t) return;
    if (kiwi_tls_cert_refused(t)) {
        const uint32_t f = mbedtls_ssl_get_verify_result(&t->ssl);
        if (f == (uint32_t)-1) {
            snprintf(out, cap, "signed by no authority in the knob's bundle");
            return;
        }
#if !defined(MBEDTLS_X509_REMOVE_INFO)
        /* What the certificate failed on: its first reason, its own words. */
        char v[160];
        if (mbedtls_x509_crt_verify_info(v, sizeof v, "", f) > 0) {
            v[strcspn(v, "\n")] = 0;
            snprintf(out, cap, "%s", v);
            return;
        }
#endif
    }
    char e[96] = "";
#if defined(MBEDTLS_ERROR_C)
    mbedtls_strerror(t->err, e, sizeof e);
#endif
    snprintf(out, cap, "-0x%04x%s%s", (unsigned)-t->err, e[0] ? " " : "", e);
}

const char *kiwi_tls_suite(const kiwi_tls_t *t)
{
    const char *s = t && t->done ? mbedtls_ssl_get_ciphersuite(&t->ssl) : NULL;
    return s ? s : "";
}

int64_t kiwi_tls_work_us(const kiwi_tls_t *t) { return t ? t->work_us : 0; }

int64_t kiwi_tls_turn_us(const kiwi_tls_t *t) { return t ? t->turn_us : 0; }

void kiwi_tls_free(kiwi_tls_t *t)
{
    if (!t) return;
    if (t->done && !t->err) mbedtls_ssl_close_notify(&t->ssl);      /* as far as the socket takes it */
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->ent);
    free(t);
}
