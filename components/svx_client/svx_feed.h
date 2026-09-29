/* Where a talker is, from an enhanced reflector's live feed.
 *
 * Some reflectors run a portal beside the SvxLink protocol that publishes the
 * same traffic as JSON over a WebSocket, wss://reflector.<domain>/ -- with
 * each node's own description of itself. Its talk_start names the talker's
 * node and location:
 *
 *   {"type":"talk_start","session":{"callsign":"ON0BXL",...,
 *    "node":{"nodeLocation":"Brussel-Vorst",...,"qth":{"loc":"JO20ET",...}}}}
 *
 * That line goes under the callsign on the knob's face. Nothing else from the
 * feed is used: its opening snapshot -- every node and a day of history,
 * 290 kB on be.svx.link -- is skipped unread. A plain reflector has no feed,
 * and costs one refused connection a minute.
 *
 * The feed runs in its own task (esp_websocket_client); lookups are
 * thread-safe.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Start following the feed of this reflector; a no-op once running. */
void svx_feed_start(const char *reflector);
/* Stop following it, and give its task and buffers back. */
void svx_feed_stop(void);

/* The location of `callsign`'s node, as its last talk_start gave it. */
bool svx_feed_where(const char *callsign, char *out, size_t cap);
