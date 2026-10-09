/* PA3FWM's WebSDR on the wire. See wsdr_sess.h.
 *
 *   GET <stream path> HTTP/1.1 (Upgrade), Origin as its page's
 *                                      101, or a status classed
 *   <- S-meter, rate, step, conversion, then audio items, at once: the last
 *      listener's tuning, or the site's own, until the knob's arrives
 *   -> GET /~~param?f=...&band=...&lo=...&hi=...&mode=...&name=...
 *   -> GET /~~param?mute=0, ...squelch=0|1, ...autonotch=0
 *   <- audio items, a message at a time: blocks of 128 samples, the S-meter
 *      every few; the rate again where it changes (AM, FM past 3.5 kHz)
 *   -> the tuning again as the dial turns, no oftener than every 250 ms
 *   <- rate 0 alone: too busy, and it closes
 *
 * Its messages are decoded whole, as its page decodes them -- a coded block
 * has no length of its own -- brought to 24 kHz and gathered into blocks for
 * the ring's flow (websdr_link.h). Silence comes as items of its own (muted,
 * its squelch shut), at the stream's pace: played as such. */
#include "wsdr_sess.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#define LOOP_MS     50
#define FIRST_US    (5 * 1000000LL)     /* no item at all: no answer */
#define QUIET_US    (10 * 1000000LL)    /* nothing for this long: gone */
#define REPORT_US   (30 * 1000000LL)
#define TURN_US     (20 * 1000LL)       /* messages read on end, at most, before the rest gets its turn */
#define INFO_MAX    (256 * 1024)        /* a bandinfo.js, at most (Twente's: 64 kB) */
#define JS_MAX      (160 * 1024)
#define PAGE_MAX    (96 * 1024)
#define DEFAULT_HZ  8000                /* the page's own, until the rate comes */
#define OUT_MAX     (WSDR_BLOCK * 24000 / 4000 + 64)

struct wsdr_sess {
    wl_t      *wl;
    kiwi_dsp_t dsp;
    wsdr_dec_t dec;
    uint8_t    msg[WSDR_MSG_MAX];
    size_t     msg_n;
    bool       msg_over;
    int16_t    out[OUT_MAX];
    int16_t    blk[WL_BLOCK];
    size_t     blk_n;
    char       cmd[320];
};

wsdr_sess_t *wsdr_sess_new(void)
{
    wsdr_sess_t *s = heap_caps_calloc(1, sizeof *s, MALLOC_CAP_SPIRAM);
    if (!s) return NULL;
    s->wl = wl_new();
    if (!s->wl) {
        free(s);
        return NULL;
    }
    return s;
}

static wsdr_end_t end_of(wl_end_t e, const wl_said_t *ws)
{
    switch (e) {
    case WL_OK:        return WSDR_END_NONE;
    case WL_WANT:      return WSDR_END_WANT;
    case WL_NOT_FOUND: return WSDR_END_NOT_FOUND;
    case WL_NO_ROUTE:  return WSDR_END_NO_ROUTE;
    case WL_NO_SOCKET: return WSDR_END_NO_SOCKET;
    case WL_CERT:      return WSDR_END_CERT;
    case WL_MOVED:     return WSDR_END_MOVED;
    case WL_HTTP:      return wsdr_http(ws->status);
    default:           return WSDR_END_NO_ANSWER;
    }
}

/* <prefix><leaf>: "/~~stream", "/websdr/tmp/bandinfo.js". -1 past `cap`. */
static int path_of(const wsdr_where_t *w, const char *leaf, char *out, size_t cap)
{
    const char *p = w->prefix && w->prefix[0] ? w->prefix : "/";
    const size_t pl = strlen(p);
    const int n = snprintf(out, cap, "%.*s/%s", (int)(p[pl - 1] == '/' ? pl - 1 : pl), p, leaf[0] == '/' ? leaf + 1 : leaf);
    return n > 0 && (size_t)n < cap ? n : -1;
}

/* ------------------------------------------------------------- the audio */

typedef struct {
    wsdr_sess_t       *s;
    const wsdr_link_t *l;
    wl_flow_t          w;
    uint32_t           seq;
    bool               play;            /* tuned: before that, the audio is another tuning's */
    unsigned           rate;
    bool               rate_known, busy, bad_rate;
    int64_t            now;
    bool               streaming;
} run_t;

static bool more(void *wl) { return wl_more((wl_t *)wl, 50); }

static void block_out(run_t *a)
{
    wsdr_sess_t *s = a->s;
    const bool play = wl_flow(&a->w, &a->l->ring, &s->dsp, ++a->seq, WL_BLOCK, WL_BLOCK * 1e6 / WL_OUT_HZ, a->now,
                              a->play, more, s->wl);
    if (play && a->l->audio) a->l->audio(a->l->ctx, s->blk, WL_BLOCK);
    s->blk_n = 0;
}

static void on_audio(void *ctx, const int16_t *pcm, bool silent)
{
    run_t *a = ctx;
    wsdr_sess_t *s = a->s;
    if (a->l->counts) {
        a->l->counts->blocks++;
        a->l->counts->silent += silent;
    }
    const int m = kiwi_resample(&s->dsp, pcm, WSDR_BLOCK, s->out, OUT_MAX);
    for (int j = 0; j < m;) {
        const size_t room = WL_BLOCK - s->blk_n;
        const size_t c = (size_t)(m - j) < room ? (size_t)(m - j) : room;
        memcpy(s->blk + s->blk_n, s->out + j, c * sizeof(int16_t));
        s->blk_n += c;
        j += (int)c;
        if (s->blk_n == WL_BLOCK) block_out(a);
    }
}

static void on_rate(void *ctx, unsigned hz)
{
    run_t *a = ctx;
    if (!hz) {
        a->busy = true;
        return;
    }
    if (a->rate_known && hz == a->rate) return;
    if (!kiwi_dsp_reset(&a->s->dsp, hz)) {
        a->bad_rate = true;
        return;
    }
    if (a->rate_known) ESP_LOGI(a->l->tag ? a->l->tag : "wsdr", "its audio at %u Hz now", hz);
    a->rate = hz;
    a->rate_known = true;
}

static void on_meter(void *ctx, int v)
{
    run_t *a = ctx;
    if (a->l->meter && a->play) a->l->meter(a->l->ctx, wsdr_dbm10(v));
}

/* ------------------------------------------------------------ the tuning */

typedef struct {
    wsdr_tune_t t;
    char        name[32];
    bool        sq, mute;
    bool        tuned;
    int64_t     t_tune;
    uint32_t    touch;
    int64_t     t_touch;
} sent_t;

static bool say(wsdr_sess_t *s, const wsdr_link_t *l, int n)
{
    if (n < 0) return true;                             /* nothing it can say: not the link's fault */
    if (l->counts) l->counts->sent++;
    return wl_text(s->wl, s->cmd);
}

static bool tune_same(const wsdr_tune_t *a, const wsdr_tune_t *b)
{
    return a->dial_hz == b->dial_hz && a->band == b->band && a->mode == b->mode && a->cw == b->cw &&
           a->lo == b->lo && a->hi == b->hi;
}

/* The caller's settings followed: everything, as the page opens, the first
 * time; then the tuning no oftener than every WSDR_TUNE_GAP_MS, the last
 * always, and mute and squelch as they change. False: the socket failed. */
static bool follow(wsdr_sess_t *s, const wsdr_link_t *l, sent_t *z, run_t *a, int64_t now, const char *tag)
{
    wsdr_ctl_t c;
    l->ctl(l->ctx, &c);
    if (c.b.maxbw_hz > 0) wsdr_clamp_pass(&c.b, &c.t);   /* a band not known yet: as the caller has it */
    if (c.touch != z->touch) {
        z->touch = c.touch;
        z->t_touch = now;
    }
    char *cmd = s->cmd;
    const size_t cap = sizeof s->cmd;
    const char *name = c.name;
    if (!z->tuned) {
        if (!say(s, l, wsdr_param_cmd(cmd, cap, &c.b, &c.t, name)) ||
            !say(s, l, wsdr_flag_cmd(cmd, cap, "mute", c.mute)) ||
            !say(s, l, wsdr_flag_cmd(cmd, cap, "squelch", c.squelch)) ||
            !say(s, l, wsdr_flag_cmd(cmd, cap, "autonotch", false)))
            return false;
        ESP_LOGI(tag, "tuned: %lld Hz, band %d, mode %u, %ld..%ld%s%s", (long long)c.t.dial_hz, c.t.band,
                 (unsigned)c.t.mode, (long)c.t.lo, (long)c.t.hi, c.squelch ? ", squelch" : "", c.mute ? ", muted" : "");
        z->t = c.t;
        snprintf(z->name, sizeof z->name, "%s", name);
        z->sq = c.squelch;
        z->mute = c.mute;
        z->tuned = true;
        z->t_tune = now;
        a->play = true;
        return true;
    }
    const bool renamed = strcmp(name, z->name) != 0;
    if ((renamed || !tune_same(&c.t, &z->t)) && now - z->t_tune >= WSDR_TUNE_GAP_MS * 1000LL) {
        if (!say(s, l, wsdr_param_cmd(cmd, cap, &c.b, &c.t, name))) return false;
        if (c.t.band != z->t.band) ESP_LOGI(tag, "band %d (%s)", c.t.band, c.b.name);
        if (renamed) ESP_LOGI(tag, "listed as \"%s\" from now on", name);
        z->t = c.t;
        snprintf(z->name, sizeof z->name, "%s", name);
        z->t_tune = now;
    }
    if (c.mute != z->mute) {
        if (!say(s, l, wsdr_flag_cmd(cmd, cap, "mute", c.mute))) return false;
        z->mute = c.mute;
    }
    if (c.squelch != z->sq) {
        if (!say(s, l, wsdr_flag_cmd(cmd, cap, "squelch", c.squelch))) return false;
        z->sq = c.squelch;
    }
    return true;
}

/* ------------------------------------------------------------ the session */

wsdr_end_t wsdr_sess_run(wsdr_sess_t *s, const wsdr_where_t *w, bool v11, uint32_t idle_ms, const wsdr_link_t *l)
{
    const char *tag = l->tag ? l->tag : "wsdr";
    l->state(l->ctx, WSDR_ST_CONNECTING, 0);
    char path[96];
    if (path_of(w, v11 ? WSDR_STREAM_V11 : WSDR_STREAM, path, sizeof path) < 0) return WSDR_END_PROTOCOL;
    const wl_addr_t ad = { .host = w->host, .port = w->port, .tls = w->tls, .key = w->key, .origin = true };
    const wl_ask_t ask = { .go_on = l->go_on, .ctx = l->ctx, .tag = tag };
    wl_said_t ws;
    wsdr_end_t end = end_of(wl_open(s->wl, &ad, path, &ask, &ws), &ws);
    if (ws.tls_port && l->moved) l->moved(l->ctx, ws.tls_port);
    if (end != WSDR_END_NONE) return end;

    kiwi_dsp_init(&s->dsp);
    kiwi_dsp_reset(&s->dsp, DEFAULT_HZ);
    wsdr_dec_reset(&s->dec);
    s->blk_n = 0;
    s->msg_n = 0;
    s->msg_over = false;
    run_t a = { .s = s, .l = l, .rate = DEFAULT_HZ };
    wl_flow_init(&a.w, &l->ring);
    const wsdr_cb_t cb = { .audio = on_audio, .rate = on_rate, .meter = on_meter, .ctx = &a };
    const int64_t t_open = esp_timer_get_time();
    sent_t z = { .t_touch = t_open };
    wsdr_ctl_t c0;
    l->ctl(l->ctx, &c0);
    z.touch = c0.touch;
    int64_t t_rx = t_open, t_rep = t_open, t_turn = t_open;
    bool had = false;                                   /* any item at all */
    uint64_t bytes = 0;
    uint8_t op = 0;

    while (end == WSDR_END_NONE) {
        wl_piece_t pc;
        const int r = wl_next(s->wl, LOOP_MS, &pc);
        int64_t now = esp_timer_get_time();
        a.now = now;
        if (r < 0) {
            end = a.streaming ? WSDR_END_CLOSED : had ? WSDR_END_CLOSED : WSDR_END_NO_ANSWER;
            break;
        }
        if (r == 1) {
            t_rx = now;
            if (pc.first) {
                op = pc.op;
                s->msg_n = 0;
                s->msg_over = pc.len > WSDR_MSG_MAX;
            }
            if (op == WL_BIN && !s->msg_over) {
                if (s->msg_n + pc.n > WSDR_MSG_MAX) {
                    s->msg_over = true;
                } else {
                    memcpy(s->msg + s->msg_n, pc.p, pc.n);
                    s->msg_n += pc.n;
                }
            }
            bytes += pc.n;
            if (pc.last && op == WL_BIN) {
                had = true;
                if (l->counts) {
                    l->counts->msgs++;
                    if (pc.len > l->counts->biggest) l->counts->biggest = (uint32_t)pc.len;
                    l->counts->dropped += s->msg_over;
                }
                if (!s->msg_over) wsdr_items(&s->dec, s->msg, s->msg_n, &cb);
                if (a.busy) {
                    ESP_LOGW(tag, "%s:%u: too busy right now", w->host, (unsigned)w->port);
                    end = WSDR_END_BUSY;
                    break;
                }
                if (a.bad_rate) {
                    ESP_LOGW(tag, "%s:%u: a rate it cannot play", w->host, (unsigned)w->port);
                    end = WSDR_END_PROTOCOL;
                    break;
                }
                if (a.play && !a.streaming && a.seq) {
                    a.streaming = true;
                    t_rep = a.w.t_rep = now;
                    memset(&a.w.rep, 0, sizeof a.w.rep);
                    l->state(l->ctx, WSDR_ST_STREAMING, a.rate);
                    ESP_LOGI(tag, "streaming from %s:%u%s, %u Hz", w->host, (unsigned)w->port, path, a.rate);
                }
            }
            if (now - t_turn < TURN_US && wl_more(s->wl, 0)) continue;
            now = esp_timer_get_time();
            a.now = now;
        }
        t_turn = now;
        if (!l->go_on(l->ctx)) {
            end = WSDR_END_WANT;
            break;
        }
        if (!had && now - t_open > FIRST_US) {
            ESP_LOGW(tag, "%s:%u: nothing came on its stream", w->host, (unsigned)w->port);
            end = WSDR_END_NO_ANSWER;
            break;
        }
        if (now - t_rx > QUIET_US) {
            ESP_LOGW(tag, "%s:%u: nothing for %lld s", w->host, (unsigned)w->port, (long long)(QUIET_US / 1000000));
            end = WSDR_END_QUIET;
            break;
        }
        if (!follow(s, l, &z, &a, now, tag)) {
            end = WSDR_END_CLOSED;
            break;
        }
        /* Its idle timeout, as its page keeps it: nothing touched that long. */
        if (idle_ms && now - z.t_touch >= (int64_t)idle_ms * 1000) {
            ESP_LOGI(tag, "%s:%u: nothing touched for %lu min: let go, as its page does", w->host,
                     (unsigned)w->port, (unsigned long)(idle_ms / 60000));
            end = WSDR_END_IDLE;
            break;
        }
        if (a.streaming && now - t_rep >= REPORT_US) {
            const double sec = (double)(now - t_rep) / 1e6;
            ESP_LOGI(tag, "these %.0f s: %.1f kB/s at %u Hz", sec, (double)bytes / 1024 / sec, a.rate);
            bytes = 0;
            wl_report_t rep;
            wl_flow_report(&a.w, &rep, s->dsp.trim, now);
            if (l->report) l->report(l->ctx, &rep);
            t_rep = now;
        }
    }
    if (a.streaming && l->report) {                     /* the last of it */
        wl_report_t rep;
        wl_flow_report(&a.w, &rep, s->dsp.trim, esp_timer_get_time());
        l->report(l->ctx, &rep);
    }
    wl_close(s->wl, end == WSDR_END_WANT || end == WSDR_END_IDLE);
    uint64_t in = 0, out = 0;
    wl_counts(s->wl, &in, &out);
    ESP_LOGI(tag, "%s:%u: session over: %s (%llu bytes in, %llu out)", w->host, (unsigned)w->port,
             end == WSDR_END_WANT ? "left" : wsdr_end_word(end), (unsigned long long)in, (unsigned long long)out);
    return end;
}

/* ------------------------------------------------------- what a page reads */

static void info_body(void *ctx, const uint8_t *p, size_t n) { wsdr_info_feed(ctx, p, n); }
static void v11_body(void *ctx, const uint8_t *p, size_t n) { wsdr_v11_feed(ctx, p, n); }
static void title_body(void *ctx, const uint8_t *p, size_t n) { wsdr_title_feed(ctx, p, n); }

static wsdr_end_t get(const wsdr_where_t *w, const char *leaf, void (*body)(void *, const uint8_t *, size_t),
                      void *bctx, size_t max, uint16_t *tls_port, bool (*go_on)(void *), void *ctx, const char *tag)
{
    char path[96];
    if (path_of(w, leaf, path, sizeof path) < 0) return WSDR_END_PROTOCOL;
    const wl_addr_t ad = { .host = w->host, .port = w->port, .tls = w->tls, .key = w->key };
    const wl_ask_t ask = { .go_on = go_on, .ctx = ctx, .tag = tag ? tag : "wsdr" };
    wl_said_t ws;
    const wl_end_t e = wl_get(&ad, path, body, bctx, max, &ask, &ws);
    if (tls_port) *tls_port = ws.tls_port;
    return e == WL_OK ? WSDR_END_NONE : end_of(e, &ws);
}

wsdr_end_t wsdr_info_read(const wsdr_where_t *w, wsdr_info_t *out, uint16_t *tls_port, bool (*go_on)(void *),
                          void *ctx, const char *tag)
{
    wsdr_info_rd_t rd;                  /* a few hundred bytes: the caller's own, never shared */
    wsdr_info_begin(&rd, out);
    const wsdr_end_t e = get(w, "tmp/bandinfo.js", info_body, &rd, INFO_MAX, tls_port, go_on, ctx, tag);
    if (e != WSDR_END_NONE) return e;
    return wsdr_info_end(&rd) ? WSDR_END_NONE : WSDR_END_NOT_WSDR;
}

wsdr_end_t wsdr_path_read(const wsdr_where_t *w, bool *v11, bool (*go_on)(void *), void *ctx, const char *tag)
{
    wsdr_v11_t v = { 0 };
    const wsdr_end_t e = get(w, "websdr-sound.js", v11_body, &v, JS_MAX, NULL, go_on, ctx, tag);
    if (v11) *v11 = v.v11;
    return e;
}

wsdr_end_t wsdr_title_read(const wsdr_where_t *w, char *out, size_t cap, bool (*go_on)(void *), void *ctx,
                           const char *tag)
{
    wsdr_title_t t;
    memset(&t, 0, sizeof t);
    const wsdr_end_t e = get(w, "", title_body, &t, PAGE_MAX, NULL, go_on, ctx, tag);
    if (out && cap) snprintf(out, cap, "%s", e == WSDR_END_NONE ? wsdr_title_end(&t) : "");
    return e;
}
