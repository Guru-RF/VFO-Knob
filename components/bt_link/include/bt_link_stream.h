/* A byte stream over the link between the knob's two chips: a radio's serial
 * link (bt_link_proto.h's RS_DATA and RS_ACK), the same code on both ends.
 *
 * The UART drops a frame whose CRC fails, and has no flow control: fine for
 * audio, where a lost frame is a moment's silence, but a radio's stream is a
 * WebSocket, and one byte lost is the end of it. So each byte has its offset
 * in the stream, as TCP numbers them. The receiver takes only the byte it
 * expects next, and says in RS_ACK how far it has them all, and how many
 * more it has room for. The sender keeps what is not acknowledged, sends no
 * further than that room, and after BTL_RS_RESEND_US without an
 * acknowledgement of anything new goes back to the first byte not
 * acknowledged and sends from there again.
 *
 * No locks and no clock of its own: the caller holds its lock round every
 * call, and passes the time. Offsets start at 0 with each link (conn). */
#ifndef BT_LINK_STREAM_H
#define BT_LINK_STREAM_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bt_link_proto.h"

/* Nothing new acknowledged for this long: from the last acknowledged byte
 * again. The UART's round trip is a few ms; a frame is 5 ms on the wire. */
#define BTL_RS_RESEND_US 40000
/* An RS_ACK at least this often while a link is open, news or not: one lost
 * is soon made good, and a room that grew is soon known. */
#define BTL_RS_ACK_US    100000
/* What the sender may assume before the receiver's first RS_ACK. */
#define BTL_RS_ROOM0     BTL_RS_CHUNK

typedef bool (*btl_rs_send_t)(void *ctx, uint8_t type, const void *p, uint16_t n);

/* The sending end. */
typedef struct {
    uint8_t *buf;
    uint32_t cap;                /* a power of two */
    uint32_t head;               /* the offset past the last byte put */
    uint32_t acked;              /* the receiver has every byte before it */
    uint32_t sent;               /* sent up to here */
    uint32_t room;               /* ...and it takes this many past acked */
    int64_t  t_wait;             /* since when what is out has waited for news */
    uint32_t resends;
} btl_rs_tx_t;

/* The receiving end. */
typedef struct {
    uint8_t *buf;
    uint32_t cap;                /* a power of two */
    uint32_t next;               /* every byte before it taken */
    uint32_t read;               /* ...and read out before this */
    bool     ack_owed;
    int64_t  t_ack;              /* the last RS_ACK sent */
    uint32_t dups, gaps;         /* frames already had, and frames past a gap */
} btl_rs_rx_t;

static inline void btl_rs_tx_reset(btl_rs_tx_t *t)
{
    t->head = t->acked = t->sent = 0;
    t->room   = BTL_RS_ROOM0;
    t->t_wait = 0;
}

static inline void btl_rs_rx_reset(btl_rs_rx_t *r)
{
    r->next = r->read = 0;
    r->ack_owed = true;          /* its room, at once: the sender knows only BTL_RS_ROOM0 */
    r->t_ack = 0;
}

/* Room for more from the application. */
static inline uint32_t btl_rs_tx_free(const btl_rs_tx_t *t) { return t->cap - (t->head - t->acked); }

/* Nothing put is left unacknowledged. */
static inline bool btl_rs_tx_idle(const btl_rs_tx_t *t) { return t->head == t->acked; }

/* As much of p as fits: the count taken. */
static inline uint32_t btl_rs_tx_put(btl_rs_tx_t *t, const void *p, uint32_t n)
{
    const uint32_t f = btl_rs_tx_free(t);
    if (n > f) n = f;
    for (uint32_t i = 0; i < n; i++) t->buf[(t->head + i) & (t->cap - 1)] = ((const uint8_t *)p)[i];
    t->head += n;
    return n;
}

/* An RS_ACK for this link: next and room. */
static inline void btl_rs_tx_ack(btl_rs_tx_t *t, uint32_t next, uint16_t room, int64_t now)
{
    /* Only forward, and never past what was put: an old ACK, late, says
     * nothing new. */
    if ((int32_t)(next - t->acked) < 0 || (int32_t)(next - t->head) > 0) return;
    if (next != t->acked) {
        t->acked  = next;
        t->t_wait = now;
        if ((int32_t)(t->sent - next) < 0) t->sent = next;
    }
    t->room = room;
}

/* Sends what the room allows, as RS_DATA of link conn; and, with nothing
 * acknowledged for BTL_RS_RESEND_US, goes back first. Stops at the first
 * frame `send` refuses. */
static inline void btl_rs_tx_pump(btl_rs_tx_t *t, uint8_t conn, int64_t now, btl_rs_send_t send, void *ctx)
{
    static uint8_t f[BTL_MAX_PAYLOAD];
    if (t->sent != t->acked && now - t->t_wait > BTL_RS_RESEND_US) {
        t->sent   = t->acked;
        t->t_wait = now;
        t->resends++;
    }
    uint32_t end = t->acked + t->room;
    if ((int32_t)(end - t->head) > 0) end = t->head;
    while ((int32_t)(end - t->sent) > 0) {
        uint32_t n = end - t->sent;
        if (n > BTL_RS_CHUNK) n = BTL_RS_CHUNK;
        f[0] = conn;
        f[1] = (uint8_t)t->sent;
        f[2] = (uint8_t)(t->sent >> 8);
        f[3] = (uint8_t)(t->sent >> 16);
        f[4] = (uint8_t)(t->sent >> 24);
        for (uint32_t i = 0; i < n; i++) f[5 + i] = t->buf[(t->sent + i) & (t->cap - 1)];
        if (!send(ctx, BTL_RS_DATA, f, (uint16_t)(5 + n))) return;
        if (t->sent == t->acked) t->t_wait = now;    /* the first out since the last news */
        t->sent += n;
    }
}

/* Room for more, as RS_ACK says it. */
static inline uint16_t btl_rs_rx_room(const btl_rs_rx_t *r)
{
    const uint32_t f = r->cap - (r->next - r->read);
    return f > 0xFFFF ? 0xFFFF : (uint16_t)f;
}

/* Bytes taken and not read out yet. */
static inline uint32_t btl_rs_rx_avail(const btl_rs_rx_t *r) { return r->next - r->read; }

/* An RS_DATA of this link: what it carries from `off`. Takes what follows on
 * from the bytes before, as far as there is room; the count taken. */
static inline uint32_t btl_rs_rx_data(btl_rs_rx_t *r, uint32_t off, const uint8_t *p, uint32_t n)
{
    r->ack_owed = true;
    if ((int32_t)(off - r->next) > 0) {          /* one before it lost */
        r->gaps++;
        return 0;
    }
    const uint32_t skip = r->next - off;
    if (skip >= n) {                             /* had already */
        r->dups++;
        return 0;
    }
    uint32_t take = n - skip;
    const uint32_t room = r->cap - (r->next - r->read);
    if (take > room) take = room;
    for (uint32_t i = 0; i < take; i++) r->buf[(r->next + i) & (r->cap - 1)] = p[skip + i];
    r->next += take;
    return take;
}

/* Up to max bytes out, in order: the count. */
static inline uint32_t btl_rs_rx_read(btl_rs_rx_t *r, void *out, uint32_t max)
{
    uint32_t n = r->next - r->read;
    if (n > max) n = max;
    if (!n) return 0;
    /* A room that was short grows: the sender hears so now, not at the next
     * reminder. */
    if (btl_rs_rx_room(r) < 2 * BTL_RS_CHUNK) r->ack_owed = true;
    for (uint32_t i = 0; i < n; i++) ((uint8_t *)out)[i] = r->buf[(r->read + i) & (r->cap - 1)];
    r->read += n;
    return n;
}

/* An RS_ACK of link conn, when one is owed or the reminder is due. */
static inline void btl_rs_rx_pump(btl_rs_rx_t *r, uint8_t conn, int64_t now, btl_rs_send_t send, void *ctx)
{
    if (!r->ack_owed && now - r->t_ack < BTL_RS_ACK_US) return;
    const uint16_t room = btl_rs_rx_room(r);
    const uint8_t  a[7] = { conn, (uint8_t)r->next, (uint8_t)(r->next >> 8), (uint8_t)(r->next >> 16),
                            (uint8_t)(r->next >> 24), (uint8_t)room, (uint8_t)(room >> 8) };
    if (send(ctx, BTL_RS_ACK, a, sizeof a)) {
        r->ack_owed = false;
        r->t_ack    = now;
    }
}

/* RS_DATA's and RS_ACK's fields; false for a frame too short. */
static inline bool btl_rs_parse_data(const uint8_t *p, uint16_t n, uint8_t *conn, uint32_t *off)
{
    if (n < 6) return false;
    *conn = p[0];
    *off  = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
    return true;
}

static inline bool btl_rs_parse_ack(const uint8_t *p, uint16_t n, uint8_t *conn, uint32_t *next, uint16_t *room)
{
    if (n < 7) return false;
    *conn = p[0];
    *next = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
    *room = (uint16_t)(p[5] | p[6] << 8);
    return true;
}

#endif /* BT_LINK_STREAM_H */
