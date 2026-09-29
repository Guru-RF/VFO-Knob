/* The reflector's control connection: SRV lookup, TCP, the plain-text
 * greeting and the TLS that follows it on the same socket, and framing.
 *
 * SvxLink's TLS is STARTTLS-like: the connection opens in the clear, the
 * reflector hands over its CA bundle, and only its MsgStartEncryption turns
 * the socket into TLS. Until then reads must take exactly one frame at a
 * time, so that not a byte of the TLS handshake is swallowed as plain text.
 *
 * TCP frame: [u32 BE length][u16 BE type][payload], the length counting the
 * type and the payload.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#define SVX_MAX_FRAME  (64 * 1024)

typedef struct {
    int       fd;
    uint32_t  addr;                 /* the reflector, network order */
    uint16_t  port;
    bool      tls;
    bool      tls_ready;            /* contexts initialised: free them */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    mbedtls_x509_crt    own;
    mbedtls_pk_context  key;
    uint8_t  *in;                   /* PSRAM: bytes read, not yet framed */
    size_t    in_len;
    size_t    consume;              /* the frame last handed out, dropped next */
    int       alert;                /* the fatal alert that ended the handshake */
} svx_link_t;

/* _svxreflector._tcp.<domain>: the target and port of the best record.
 * Returns 1 if there is one, 0 if there is none (use the name and the
 * configured port), -1 if DNS could not be asked. */
int svx_srv_lookup(const char *domain, char *host, size_t cap, uint16_t *port);

/* Resolve and connect, `timeout_ms` per address. 0 on success. */
int  svx_link_open(svx_link_t *l, const char *host, uint16_t port, int timeout_ms);
void svx_link_close(svx_link_t *l);

/* One whole frame; blocks up to `timeout_ms`. Plain text before TLS (read
 * exactly), decrypted after. Returns 1 with the type, and the frame from its
 * type field on with that length -- the form proto.c's parsers take -- valid
 * until the next call; 0 on timeout; -1 when the connection closed or failed. */
int svx_link_frame(svx_link_t *l, uint16_t *type, uint8_t **payload, size_t *len,
                   int timeout_ms);

/* After TLS: reads what has arrived without waiting. Returns -1 if the
 * connection closed, else 0. Frames are then taken with svx_link_next(). */
int svx_link_pump(svx_link_t *l);
/* A frame already read, if there is a whole one: 1, else 0; -1 if the peer
 * sent one too large to be real. */
int svx_link_next(svx_link_t *l, uint16_t *type, uint8_t **payload, size_t *len);

/* Send a built frame, whole, within `timeout_ms`. 0 on success. */
int svx_link_send(svx_link_t *l, const uint8_t *buf, size_t len, int timeout_ms);

/* Start TLS on the socket. Presents `crt_pem`/`key_pem` when both are given,
 * with the CAs above the certificate from `ca_pem`, the reflector's bundle.
 * 0 on success; -2 when the reflector refused the certificate we offered
 * (l->alert says how); -1 on any other failure. */
int svx_link_start_tls(svx_link_t *l, const char *crt_pem, const char *key_pem,
                       const char *ca_pem, int timeout_ms);

/* Something to read on the socket, or buffered inside TLS. */
bool svx_link_readable(svx_link_t *l);
