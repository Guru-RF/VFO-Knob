/* The reflector's audio channel. See svx_udp.h; the crypto is
 * SVXConnect-CLI's src/common/crypto.c on mbedTLS. */
#include "svx_udp.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#include "common/proto.h"

static const char *TAG = "svx-udp";

#define MTU 2048

static uint8_t s_pt[MTU], s_wire[MTU + 32];   /* the reflector task's own */

static int64_t ms_now(void) { return esp_timer_get_time() / 1000; }

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int svx_udp_open(svx_udp_t *u, uint32_t addr, uint16_t port)
{
    memset(u, 0, sizeof *u);
    mbedtls_gcm_init(&u->tx);
    mbedtls_gcm_init(&u->rx);
    u->addr = addr;
    u->port = port;
    /* Not connect()ed: the tag is the authentication, from wherever it came. */
    u->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u->fd < 0) {
        ESP_LOGE(TAG, "no socket: errno %d", errno);
        return -1;
    }
    fcntl(u->fd, F_SETFL, fcntl(u->fd, F_GETFL, 0) | O_NONBLOCK);
    return 0;
}

void svx_udp_close(svx_udp_t *u)
{
    if (u->fd >= 0) close(u->fd);
    u->fd = -1;
    mbedtls_gcm_free(&u->tx);
    mbedtls_gcm_free(&u->rx);
    memset(u->tx_key, 0, sizeof u->tx_key);
    u->keyed_tx = u->keyed_rx = false;
}

void svx_udp_make_tx_key(svx_udp_t *u, uint16_t client_id)
{
    esp_fill_random(u->tx_key, sizeof u->tx_key);
    esp_fill_random(u->tx_iv, sizeof u->tx_iv);
    u->client_id    = client_id;
    u->tx_ctr       = 0;
    u->sent_initial = false;
    u->keyed_tx = mbedtls_gcm_setkey(&u->tx, MBEDTLS_CIPHER_ID_AES, u->tx_key, 128) == 0;
}

int svx_udp_rx_shared(svx_udp_t *u)
{
    /* Our key both ways. With client id 0 the two directions' nonces would be
     * identical under one key, which GCM does not survive. */
    if (!u->keyed_tx || u->client_id == 0) return -1;
    memcpy(u->rx_iv, u->tx_iv, 6);
    u->keyed_rx = mbedtls_gcm_setkey(&u->rx, MBEDTLS_CIPHER_ID_AES, u->tx_key, 128) == 0;
    u->have_high = false;
    return u->keyed_rx ? 0 : -1;
}

int svx_udp_rx_key(svx_udp_t *u, const uint8_t iv4[4], const uint8_t key[16])
{
    memcpy(u->rx_iv, iv4, 4);
    u->rx_iv[4] = u->rx_iv[5] = 0;
    u->keyed_rx = mbedtls_gcm_setkey(&u->rx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0;
    u->have_high = false;
    return u->keyed_rx ? 0 : -1;
}

int svx_udp_send(svx_udp_t *u, uint16_t type, const uint8_t *body, size_t len)
{
    if (u->fd < 0 || !u->keyed_tx) return -1;
    size_t n = 0;
    if (proto_build_udp_plaintext(s_pt, sizeof s_pt, &n, type, body, len) != 0) return -1;
    /* A counter that has been all the way round would repeat a nonce: stop,
     * and let a new login bring a new key. */
    if (u->sent_initial && u->tx_ctr == 0) return -1;

    const bool     initial = !u->sent_initial;
    const uint32_t ctr     = initial ? 0 : u->tx_ctr;
    uint8_t iv[12];
    memcpy(iv, u->tx_iv, 6);
    put16(iv + 6, u->client_id);
    put32(iv + 8, ctr);
    const size_t aad = initial ? 6 : 4;
    put32(s_wire, ctr);
    if (initial) put16(s_wire + 4, u->client_id);

    uint8_t tag[16];
    if (mbedtls_gcm_crypt_and_tag(&u->tx, MBEDTLS_GCM_ENCRYPT, n, iv, sizeof iv,
                                  s_wire, aad, s_pt, s_wire + aad + 8, 16, tag) != 0)
        return -1;
    memcpy(s_wire + aad, tag, 8);

    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(u->port),
                              .sin_addr.s_addr = u->addr };
    if (sendto(u->fd, s_wire, aad + 8 + n, 0, (struct sockaddr *)&to, sizeof to) < 0) {
        u->n_tx_fail++;
        return -1;
    }
    if (initial) { u->sent_initial = true; u->tx_ctr = 1; }
    else         u->tx_ctr++;
    u->n_tx++;
    u->t_tx = ms_now();
    return 0;
}

int svx_udp_recv(svx_udp_t *u, uint16_t *type, const uint8_t **body, size_t *len,
                 int *gap)
{
    while (u->fd >= 0) {
        int k = recv(u->fd, s_wire, sizeof s_wire, 0);
        if (k < 0) return 0;                 /* nothing more waiting */
        if (k < 12 || !u->keyed_rx) continue;

        const uint32_t ctr = get32(s_wire);
        uint8_t iv[12];
        memcpy(iv, u->rx_iv, 6);
        iv[6] = iv[7] = 0;
        put32(iv + 8, ctr);
        const size_t n = (size_t)k - 12;
        if (n > sizeof s_pt ||
            mbedtls_gcm_auth_decrypt(&u->rx, n, iv, sizeof iv, s_wire, 4,
                                     s_wire + 4, 8, s_wire + 12, s_pt) != 0) {
            u->n_auth_fail++;
            continue;
        }
        /* Replay protection after authentication, never before: a forged
         * packet must not be able to move the high-water mark. */
        int g = 0;
        if (u->have_high) {
            if (ctr <= u->rx_high) { u->n_replayed++; continue; }
            uint32_t d = ctr - u->rx_high - 1;
            g = d > 16 ? 16 : (int)d;
        }
        u->have_high = true;
        u->rx_high   = ctr;
        u->n_lost   += (uint32_t)g;
        u->n_rx++;
        u->t_rx = ms_now();
        if (proto_parse_udp_plaintext(s_pt, n, type, body, len) != 0) continue;
        *gap = g;
        return 1;
    }
    return 0;
}
