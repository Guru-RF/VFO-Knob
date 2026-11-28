/* A WebSocket client for AISStream.io's stream (components/ais_client): a
 * connection over TLS (components/kiwi_proto kiwi_tls) or in the clear, its
 * upgrade offering permessage-deflate, and its messages read whole --
 * fragments joined, a compressed one inflated (the S3 ROM's tinfl; zlib on
 * the PC) -- pings answered, its own pings sent to keep it open.
 *
 * Only what the stream needs: a client that sends text (the subscription,
 * never compressed) and reads text or binary. Nothing here blocks for long:
 * every wait is a slice at a time, the caller's go_on asked in between. Its
 * buffers are in PSRAM. Plain C on lwip's sockets, built on the PC too
 * (test/host ais_host). */
#ifndef AIS_WS_H
#define AIS_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ais_ws ais_ws_t;

/* What came of a step: a message, none yet, or the end and why. */
enum { AIS_WS_MSG = 1, AIS_WS_NONE = 0, AIS_WS_CLOSED = -2, AIS_WS_FAILED = -1 };

/* Connected and upgraded to `url` (ws:// or wss://, host, port, path):
 * NULL and why in a few words ("no answer", "HTTP 401", "certificate")
 * otherwise. `deflate`: permessage-deflate offered; ais_ws_deflated() says
 * whether the server took it. `ua` the User-Agent. */
ais_ws_t *ais_ws_open(const char *url, bool deflate, const char *ua, bool (*go_on)(void *), void *ctx,
                      char *why, size_t wn);

/* A text message, masked as a client's must be. False when the connection
 * would not take it. */
bool ais_ws_send_text(ais_ws_t *w, const char *t, size_t n);

/* Waits up to `ms` for a message: AIS_WS_MSG and it in *msg, *len (the
 * connection's buffer, NUL-ended, until the next call); AIS_WS_NONE for
 * none, a ping answered or a pong taken meanwhile; AIS_WS_CLOSED when the
 * far end closed it, or AIS_WS_FAILED, and why. A message too big for the
 * buffer, or that would not inflate, is let go by and counted. */
int ais_ws_recv(ais_ws_t *w, int ms, const char **msg, size_t *len, char *why, size_t wn);

/* A ping, its payload empty: the stream says nothing for a while where
 * ships are few, and a pong says the connection is still there. */
bool ais_ws_ping(ais_ws_t *w);

bool     ais_ws_deflated(const ais_ws_t *w);
/* When anything last came from the far end, µs (esp_timer). */
int64_t  ais_ws_heard_us(const ais_ws_t *w);
/* Bytes read off the wire, and in the messages after inflating; messages
 * let go by. */
void     ais_ws_counts(const ais_ws_t *w, uint64_t *wire, uint64_t *plain, uint32_t *dropped);

/* A close frame if the socket takes it, and all of it freed. */
void ais_ws_close(ais_ws_t *w);

#endif /* AIS_WS_H */
