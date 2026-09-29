/* The reflector's audio channel: UDP to the same host and port as the control
 * connection, every datagram AES-128-GCM with the tag cut to 8 bytes.
 *
 *   wire  = AAD || tag[0..7] || ciphertext
 *   TX IV = tx_iv_rand[6] || client_id BE16 || counter BE32
 *   RX IV = rx_iv_rand[6] || 00 00          || counter BE32
 *   AAD   = counter BE32, plus client_id BE16 on the very first datagram
 *
 * The first datagram of a login -- counter 0, carrying the client id -- is what
 * binds this flow to the login on the reflector's side, and opens the NAT on
 * ours; it goes out once, as soon as the reflector says StartUdpEncryption.
 * Received counters must rise strictly, checked after authentication.
 *
 * Plaintext: [u16 type][u16 length][opus] for audio, [u16 type][body] for
 * everything else.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbedtls/gcm.h"

typedef struct {
    int       fd;
    uint32_t  addr;            /* network order */
    uint16_t  port;
    mbedtls_gcm_context tx, rx;
    bool      keyed_tx, keyed_rx;
    uint8_t   tx_key[16];
    uint8_t   tx_iv[6], rx_iv[6];
    uint16_t  client_id;
    uint32_t  tx_ctr;
    bool      sent_initial;
    bool      have_high;
    uint32_t  rx_high;
    /* counters, for the log */
    uint32_t  n_rx, n_auth_fail, n_replayed, n_lost, n_tx, n_tx_fail;
    int64_t   t_tx, t_rx;      /* last send, last authenticated receive (ms) */
} svx_udp_t;

int  svx_udp_open(svx_udp_t *u, uint32_t addr, uint16_t port);
void svx_udp_close(svx_udp_t *u);

/* Our transmit key, made fresh for each login: sent to the reflector in
 * NodeInfo. */
void svx_udp_make_tx_key(svx_udp_t *u, uint16_t client_id);

/* StartUdpEncryption: with an empty body, the reflector uses our key both
 * ways; otherwise it names its own 4-byte IV and key. 0, or -1 if refused
 * (shared-key mode with client id 0 would reuse nonces). */
int svx_udp_rx_shared(svx_udp_t *u);
int svx_udp_rx_key(svx_udp_t *u, const uint8_t iv4[4], const uint8_t key[16]);

/* Encrypt and send one message. 0, or -1 if it could not go. */
int svx_udp_send(svx_udp_t *u, uint16_t type, const uint8_t *body, size_t len);

/* One authenticated message, if one is waiting: 1 with its type and body
 * (valid until the next call) and how many were lost before it; 0 if none. */
int svx_udp_recv(svx_udp_t *u, uint16_t *type, const uint8_t **body, size_t *len,
                 int *gap);
