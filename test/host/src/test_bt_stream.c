/* The radio's byte stream between the knob's two chips
 * (components/bt_link/include/bt_link_stream.h): every byte once and in
 * order, through a wire that drops frames, doubles them and holds them back;
 * never past the receiver's room; old acknowledgements and another link's
 * frames changing nothing. */
#include "tiny.h"
#include "bt_link_stream.h"

/* A wire one way: frames queued, each one delivered at its time -- or lost,
 * or delivered twice. */
#define WIRE_MAX 4096
typedef struct {
    uint8_t  type;
    uint16_t n;
    uint8_t  p[BTL_MAX_PAYLOAD];
    int64_t  at;
} frame_t;
typedef struct {
    frame_t  q[WIRE_MAX];
    int      n;
    int      loss_pct, dup_pct;     /* of every 100 frames */
    int64_t  delay;
    uint32_t sent, lost;
} wire_t;

static uint32_t s_rand = 12345;
static int roll(void)
{
    s_rand = s_rand * 1103515245u + 12345u;
    return (int)((s_rand >> 16) % 100);
}

static int64_t s_now;

static bool wire_send(void *ctx, uint8_t type, const void *p, uint16_t n)
{
    wire_t *w = ctx;
    w->sent++;
    if (roll() < w->loss_pct) {
        w->lost++;
        return true;                /* gone on the wire: the sender cannot tell */
    }
    const int copies = roll() < w->dup_pct ? 2 : 1;
    for (int c = 0; c < copies && w->n < WIRE_MAX; c++) {
        frame_t *f = &w->q[w->n++];
        f->type = type;
        f->n    = n;
        memcpy(f->p, p, n);
        f->at   = s_now + w->delay;
    }
    return true;
}

/* One side: what it sends, what it receives. */
typedef struct {
    btl_rs_tx_t tx;
    btl_rs_rx_t rx;
    uint8_t     conn;
} end_t;

static void deliver(wire_t *w, end_t *to)
{
    int keep = 0;
    for (int i = 0; i < w->n; i++) {
        frame_t *f = &w->q[i];
        if (f->at > s_now) {
            w->q[keep++] = *f;
            continue;
        }
        if (f->type == BTL_RS_DATA) {
            uint8_t conn;
            uint32_t off;
            if (btl_rs_parse_data(f->p, f->n, &conn, &off) && conn == to->conn)
                btl_rs_rx_data(&to->rx, off, f->p + 5, f->n - 5u);
        } else if (f->type == BTL_RS_ACK) {
            uint8_t  conn;
            uint32_t next;
            uint16_t room;
            if (btl_rs_parse_ack(f->p, f->n, &conn, &next, &room) && conn == to->conn)
                btl_rs_tx_ack(&to->tx, next, room, s_now);
        }
    }
    w->n = keep;
}

static uint8_t pattern(uint32_t i) { return (uint8_t)(i * 7u + (i >> 8)); }

/* Sends `total` bytes from a to b through the wires, reading at b `rate`
 * bytes per ms at most; true when every byte came, in order. */
static bool run(end_t *a, end_t *b, wire_t *ab, wire_t *ba, uint32_t total, uint32_t rate, int64_t limit_ms,
                uint32_t *max_unread)
{
    uint32_t put = 0, got = 0;
    bool ok = true;
    for (s_now = 0; s_now < limit_ms * 1000 && got < total; s_now += 1000) {
        uint8_t chunk[700];
        while (put < total) {
            uint32_t n = total - put < sizeof chunk ? total - put : (uint32_t)sizeof chunk;
            for (uint32_t i = 0; i < n; i++) chunk[i] = pattern(put + i);
            const uint32_t took = btl_rs_tx_put(&a->tx, chunk, n);
            put += took;
            if (took < n) break;
        }
        btl_rs_tx_pump(&a->tx, a->conn, s_now, wire_send, ab);
        deliver(ab, b);
        if (btl_rs_rx_avail(&b->rx) > *max_unread) *max_unread = btl_rs_rx_avail(&b->rx);
        uint8_t out[4096];
        const uint32_t n = btl_rs_rx_read(&b->rx, out, rate < sizeof out ? rate : (uint32_t)sizeof out);
        for (uint32_t i = 0; i < n; i++)
            if (out[i] != pattern(got + i)) ok = false;
        got += n;
        btl_rs_rx_pump(&b->rx, b->conn, s_now, wire_send, ba);
        deliver(ba, a);
    }
    return ok && got == total;
}

static uint8_t s_buf[4][1 << 14];

static void ends(end_t *a, end_t *b, uint32_t a_cap, uint32_t b_cap)
{
    memset(a, 0, sizeof *a);
    memset(b, 0, sizeof *b);
    a->tx.buf = s_buf[0]; a->tx.cap = a_cap;
    a->rx.buf = s_buf[1]; a->rx.cap = a_cap;
    b->tx.buf = s_buf[2]; b->tx.cap = b_cap;
    b->rx.buf = s_buf[3]; b->rx.cap = b_cap;
    a->conn = b->conn = 3;
    btl_rs_tx_reset(&a->tx);
    btl_rs_rx_reset(&a->rx);
    btl_rs_tx_reset(&b->tx);
    btl_rs_rx_reset(&b->rx);
}

static wire_t s_ab, s_ba;

static void wires(int loss, int dup, int64_t delay_us)
{
    memset(&s_ab, 0, sizeof s_ab);
    memset(&s_ba, 0, sizeof s_ba);
    s_ab.loss_pct = s_ba.loss_pct = loss;
    s_ab.dup_pct  = s_ba.dup_pct  = dup;
    s_ab.delay    = s_ba.delay    = delay_us;
}

static void test_clean(void)
{
    CASE("a clean wire: every byte, in order, nothing sent twice");
    end_t a, b;
    ends(&a, &b, 1 << 13, 1 << 13);
    wires(0, 0, 2000);
    uint32_t most = 0;
    CHECK(run(&a, &b, &s_ab, &s_ba, 200000, 100000, 2000, &most));
    CHECK_EQ(a.tx.resends, 0);
    CHECK_EQ(b.rx.dups, 0);
    CHECK(btl_rs_tx_idle(&a.tx) || a.tx.head - a.tx.acked <= a.tx.cap);
}

static void test_lossy(void)
{
    CASE("a wire that loses 10% and doubles 5%: every byte once, in order");
    end_t a, b;
    ends(&a, &b, 1 << 13, 1 << 13);
    wires(10, 5, 3000);
    uint32_t most = 0;
    CHECK(run(&a, &b, &s_ab, &s_ba, 300000, 100000, 20000, &most));
    CHECK(a.tx.resends > 0);
    CHECK(s_ab.lost > 0);
}

static void test_room(void)
{
    CASE("a slow reader: the sender never runs past its room");
    end_t a, b;
    ends(&a, &b, 1 << 13, 1 << 11);
    wires(0, 0, 1000);
    uint32_t most = 0;
    CHECK(run(&a, &b, &s_ab, &s_ba, 50000, 300, 20000, &most));
    CHECK(most <= (1u << 11));
    CASE("...and a slow reader with losses, the room's news lost too");
    ends(&a, &b, 1 << 13, 1 << 11);
    wires(20, 0, 1000);
    most = 0;
    CHECK(run(&a, &b, &s_ab, &s_ba, 50000, 300, 60000, &most));
    CHECK(most <= (1u << 11));
}

static void test_old_news(void)
{
    CASE("an old ACK, and one past what was put, change nothing");
    end_t a, b;
    ends(&a, &b, 1 << 12, 1 << 12);
    uint8_t x[100];
    memset(x, 0x55, sizeof x);
    CHECK_EQ(btl_rs_tx_put(&a.tx, x, sizeof x), 100);
    btl_rs_tx_ack(&a.tx, 60, 4000, 0);
    CHECK_EQ(a.tx.acked, 60);
    btl_rs_tx_ack(&a.tx, 40, 4000, 0);
    CHECK_EQ(a.tx.acked, 60);
    btl_rs_tx_ack(&a.tx, 101, 4000, 0);
    CHECK_EQ(a.tx.acked, 60);
    CHECK_EQ(btl_rs_tx_free(&a.tx), (1 << 12) - 40);

    CASE("a full sender takes only what fits");
    ends(&a, &b, 1 << 10, 1 << 10);
    uint8_t big[2000];
    memset(big, 1, sizeof big);
    CHECK_EQ(btl_rs_tx_put(&a.tx, big, sizeof big), 1 << 10);
    CHECK_EQ(btl_rs_tx_put(&a.tx, big, 1), 0);

    CASE("data past a gap is not taken; data already had is not taken twice");
    uint8_t d[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    CHECK_EQ(btl_rs_rx_data(&b.rx, 5, d, 10), 0);
    CHECK_EQ(b.rx.gaps, 1);
    CHECK_EQ(btl_rs_rx_data(&b.rx, 0, d, 10), 10);
    CHECK_EQ(btl_rs_rx_data(&b.rx, 0, d, 10), 0);
    CHECK_EQ(b.rx.dups, 1);
    CASE("...and a frame that overlaps the end takes only its new part");
    CHECK_EQ(btl_rs_rx_data(&b.rx, 5, d, 10), 5);
    CHECK_EQ(b.rx.next, 15);
    uint8_t out[20];
    CHECK_EQ(btl_rs_rx_read(&b.rx, out, sizeof out), 15);
    CHECK_EQ(out[10], 6);
    CHECK_EQ(out[14], 10);
}

static void test_other_link(void)
{
    CASE("another link's frames are dropped by the caller's conn check");
    end_t a, b;
    ends(&a, &b, 1 << 12, 1 << 12);
    wires(0, 0, 0);
    uint8_t x[50];
    memset(x, 9, sizeof x);
    btl_rs_tx_put(&a.tx, x, sizeof x);
    btl_rs_tx_pump(&a.tx, 4, 0, wire_send, &s_ab);   /* a stale link's number */
    s_now = 0;
    deliver(&s_ab, &b);
    CHECK_EQ(btl_rs_rx_avail(&b.rx), 0);
}

static void test_wrap(void)
{
    CASE("offsets past 2^32 wrap on: the stream goes on");
    end_t a, b;
    ends(&a, &b, 1 << 12, 1 << 12);
    a.tx.head = a.tx.acked = a.tx.sent = 0xFFFFF000u;
    b.rx.next = b.rx.read = 0xFFFFF000u;
    wires(5, 0, 1000);
    uint32_t most = 0;
    CHECK(run(&a, &b, &s_ab, &s_ba, 40000, 100000, 5000, &most));
}

T_MAIN(test_clean(); test_lossy(); test_room(); test_old_news(); test_other_link(); test_wrap())
