/* The UberSDR's connections: HTTP requests and WebSockets, over TLS to a
 * receiver behind its tunnel (https://<name>.tunnel.ubersdr.org, port 443) or
 * in the clear to one on the LAN (http://host:8080) -- the rest of the client
 * never asks which. TLS is esp-tls's; in the clear the connection is a socket
 * of its own, which spares the 2 kB of internal RAM an esp-tls connection
 * takes even without TLS.
 *
 * Everything here blocks, for at most the timeout given; it is meant for the
 * client's own tasks, whose stacks are in PSRAM (nothing here touches flash).
 * Every request says who it is with the same User-Agent: UberSDR refuses a
 * WebSocket to a session registered without one. */
#ifndef UBER_NET_H
#define UBER_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_tls.h"

typedef struct {
    char     host[64];
    uint16_t port;
    bool     tls;          /* https and wss; else http and ws, in the clear */
} uhost_t;

/* "VFO-Knob/1.14.0 (+https://github.com/Guru-RF/VFO-Knob)" */
const char *unet_agent(void);

/* A connection: esp-tls's over TLS, a plain socket in the clear. Zeroed, it
 * is closed. */
typedef struct {
    esp_tls_t *tls;        /* over TLS */
    int        fd;         /* its socket, either way */
    bool       up;
} unet_t;

/* Connected, or false with `why` in a word or two ("name not found",
 * "no answer", "TLS failed"). */
bool unet_connect(unet_t *c, const uhost_t *h, int timeout_ms, char *why, size_t wn);
bool unet_write(unet_t *c, const void *b, size_t n);
void unet_drop(unet_t *c);

/* One request, Connection: close. The body -- up to `cap` bytes, the rest
 * dropped -- into `out`, NUL-terminated when it fits; its length into *len.
 * Returns the HTTP status, or -1 with `why` when there was none. `body` is
 * JSON for a POST, NULL for a GET. */
int unet_http(const uhost_t *h, const char *method, const char *path, const char *body,
              char *out, size_t cap, size_t *len, int timeout_ms, char *why, size_t wn);

/* The same, on a connection kept open between requests, as a browser keeps
 * one: TLS is done in software here, over a second a handshake, and a
 * gallery's pictures or the band's voices come one request after another. A
 * connection the server has closed meanwhile is made again, once. */
typedef struct {
    unet_t     n;
    void      *rb;          /* its read buffer */
    int64_t    last;        /* when it was last used, us */
} uconn_t;
int  unet_http_keep(uconn_t *c, const uhost_t *h, const char *method, const char *path,
                    const char *body, char *out, size_t cap, size_t *len, int timeout_ms,
                    char *why, size_t wn);
void unet_close(uconn_t *c);

/* ------------------------------------------------------------ WebSocket */

typedef struct {
    unet_t     n;
    uint8_t   *rx;         /* frames as they arrive: PSRAM */
    size_t     cap, have;
    size_t     used;       /* the frame handed out last, dropped on the next call */
    size_t     skip;       /* the rest of a frame too big for rx, to let go by */
    uint16_t   close_code; /* the server's, when it closed */
} uws_t;

/* The upgrade to `path` (with its query). False, with `why`, unless the server
 * said 101: then its status and the start of its answer ("429 Too Many
 * Requests"). `rx_cap` bytes of PSRAM are taken for the frames. */
bool uws_open(uws_t *w, const uhost_t *h, const char *path, size_t rx_cap,
              int timeout_ms, char *why, size_t wn);

/* The next frame: 1 with it in *op, *p and *n (valid until the next call), 0
 * after `timeout_ms` with none, -1 once the connection is gone. Pings are
 * answered here, and never handed out. */
int  uws_recv(uws_t *w, int timeout_ms, uint8_t *op, const uint8_t **p, size_t *n);
bool uws_text(uws_t *w, const char *s);
void uws_close(uws_t *w);

#endif /* UBER_NET_H */
