/* OpenWebRX's protocol on the wire. See owrx_sess.h.
 *
 *   GET <path>ws/ HTTP/1.1 (Upgrade)   101, or a status classed
 *   -> SERVER DE CLIENT client=VFO-Knob type=receiver
 *   -> {"type":"connectionproperties","params":{"output_rate":12000,"hd_output_rate":48000}}
 *   <- CLIENT DE SERVER server=openwebrx version=v...
 *   <- receiver_details, clients, config (its globals), config (its band),
 *      bookmarks, modes, profiles ...; the waterfall (0x01) from here on
 *   -> dspcontrol params (all of them), then {"action":"start"}
 *   <- audio (0x02, 0x04 for WFM), smeter four times a second
 *   -> dspcontrol params as the dial turns: offset_freq alone; mod,
 *      low_cut, high_cut and offset_freq together for a mode
 *   <- config again whenever its band changes, anyone's doing: tuned again,
 *      and start again, or nothing plays
 *
 * Its frames come in all sizes, a few hundred bytes of audio at a time, its
 * ADPCM's SYNC wherever it falls: the audio is decoded as it comes, brought
 * to 24 kHz and gathered into blocks for the ring's flow (websdr_link.h),
 * so that a stall is told from the stream's own unevenness as for a Kiwi's
 * frames. A receiver whose squelch closes stops its audio (csdr's Squelch,
 * after a few blocks of zeros) while its S-meter comes on: with its meter
 * under the squelch -- the same power its squelch weighs -- a pause in the
 * audio is the squelch's, and the session plays silence at the stream's
 * pace from where it stopped, the ring kept at its target: a squelch is no
 * stall, and the next voice is not cut. A pause with the squelch open is
 * the network's, for the ring to ride out as ever. */
#include "owrx_sess.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#define LOOP_MS       50
#define HELLO_US      (5 * 1000000LL)   /* no "CLIENT DE SERVER": no answer */
#define QUIET_US      (10 * 1000000LL)  /* nothing at all: gone */
#define OFFSET_GAP_US (50 * 1000LL)     /* the dial's newest, no oftener */
#define PASS_GAP_US   (100 * 1000LL)
#define SQ_GAP_US     (200 * 1000LL)
#define PAUSE_US      (60 * 1000LL)     /* its audio paused this long, its squelch closed: silence */
#define REPORT_US     (30 * 1000000LL)
#define IN_MAX        2048              /* decoded samples resampled at a time */
#define STATUS_MAX    (64 * 1024)       /* a status.json, at most */
#define TURN_US       (20 * 1000LL)     /* frames read on end, at most, before the rest gets its turn */
#define FK_NONE       (-1)
#define FK_TEXT       0x100             /* the frame being read: text, or its binary kind (its first byte) */

struct owrx_sess {
    wl_t        *wl;
    kiwi_dsp_t   dsp;
    owrx_adpcm_t ad;
    int16_t      pcm[2 * 4096 + 8];     /* a piece's audio, decoded */
    int16_t      out[2 * IN_MAX + 64];  /* ...at 24 kHz */
    int16_t      blk[WL_BLOCK];
    size_t       blk_n;
    char         cmd[512];
};

owrx_sess_t *owrx_sess_new(void)
{
    owrx_sess_t *s = heap_caps_calloc(1, sizeof *s, MALLOC_CAP_SPIRAM);
    if (!s) return NULL;
    s->wl = wl_new();
    if (!s->wl) {
        free(s);
        return NULL;
    }
    return s;
}

static owrx_end_t end_of(wl_end_t e, const wl_said_t *ws)
{
    switch (e) {
    case WL_OK:        return OWRX_END_NONE;
    case WL_WANT:      return OWRX_END_WANT;
    case WL_NOT_FOUND: return OWRX_END_NOT_FOUND;
    case WL_NO_ROUTE:  return OWRX_END_NO_ROUTE;
    case WL_NO_SOCKET: return OWRX_END_NO_SOCKET;
    case WL_CERT:      return OWRX_END_CERT;
    case WL_MOVED:     return OWRX_END_MOVED;
    case WL_HTTP:      return owrx_http(ws->status);
    default:           return OWRX_END_NO_ANSWER;
    }
}

/* ------------------------------------------------------------- the audio */

/* Where its audio is: the blocks, and the silence played while it pauses. */
typedef struct {
    wl_flow_t w;
    uint32_t  seq;
    bool      play;                     /* tuned: before that, its audio is the last listener's */
    int64_t   t_audio;                  /* the last audio */
    int64_t   t_due;                    /* the stream's pace: when the next block is due */
    bool      shut;                     /* its meter under its squelch: the squelch closed */
    bool      filling;
    uint8_t   kind;                     /* the audio's frame kind now: its rate */
} aud_t;

static bool more(void *wl) { return wl_more((wl_t *)wl, 50); }

static void block_out(owrx_sess_t *s, const owrx_link_t *l, aud_t *a, int64_t now)
{
    const int64_t period = (int64_t)(WL_BLOCK * 1e6 / WL_OUT_HZ);
    /* The stream's pace, from its first block on -- taken afresh after a
     * stall had it fall far behind. */
    a->t_due = !a->seq || a->t_due < now - 500000 ? now + period : a->t_due + period;
    const bool play = wl_flow(&a->w, &l->ring, &s->dsp, ++a->seq, WL_BLOCK, WL_BLOCK * 1e6 / WL_OUT_HZ, now,
                              a->play, more, s->wl);
    if (play && l->audio) l->audio(l->ctx, s->blk, WL_BLOCK);
    s->blk_n = 0;
}

/* Decoded audio at its own rate, to 24 kHz, into blocks. */
static void audio_in(owrx_sess_t *s, const owrx_link_t *l, aud_t *a, const int16_t *x, size_t n, int64_t now)
{
    for (size_t i = 0; i < n;) {
        const size_t k = n - i < IN_MAX ? n - i : IN_MAX;
        const int m = kiwi_resample(&s->dsp, x + i, (int)k, s->out, (int)(sizeof s->out / sizeof s->out[0]));
        for (int j = 0; j < m;) {
            const size_t room = WL_BLOCK - s->blk_n;
            const size_t c = (size_t)(m - j) < room ? (size_t)(m - j) : room;
            memcpy(s->blk + s->blk_n, s->out + j, c * sizeof(int16_t));
            s->blk_n += c;
            j += (int)c;
            if (s->blk_n == WL_BLOCK) block_out(s, l, a, now);
        }
        i += k;
    }
}

/* Its audio paused with its squelch closed -- its meter under it: silence,
 * block by block at the stream's pace from where the audio stopped, so the
 * ring keeps its time. */
static void pause_fill(owrx_sess_t *s, const owrx_link_t *l, aud_t *a, int64_t now)
{
    const bool paused = a->play && a->seq && a->shut && now - a->t_audio > PAUSE_US;
    if (!paused) {
        a->filling = false;
        return;
    }
    if (!a->filling) {
        a->filling = true;
        if (s->blk_n) {                                 /* the block it stopped in, finished */
            memset(s->blk + s->blk_n, 0, (WL_BLOCK - s->blk_n) * sizeof(int16_t));
            s->blk_n = WL_BLOCK;
            block_out(s, l, a, now);
        }
    }
    while (a->t_due <= now) {
        memset(s->blk, 0, sizeof s->blk);
        s->blk_n = WL_BLOCK;
        block_out(s, l, a, now);
    }
}

/* ------------------------------------------------------------ the tuning */

/* What was last sent, and when. */
typedef struct {
    owrx_tune_t t;
    bool        tuned;                  /* false: everything, then start, again */
    int64_t     t_off, t_pass, t_sq;
    uint32_t    band_seq;
    bool        band_due;
    char        band[OWRX_ID_MAX];
    int64_t     t_band;                 /* the last band asked for -- or the session's start */
} sent_t;

static bool say(owrx_sess_t *s, const owrx_link_t *l, int n)
{
    if (n < 0) return true;                             /* nothing it can say: not the link's fault */
    if (l->counts) l->counts->sent++;
    return wl_text(s->wl, s->cmd);
}

/* The caller's settings followed: everything and start after a change of
 * band; else the dial, the mode and its passband, the squelch, each as it
 * moves, no oftener than the receiver needs them. A band asked for goes
 * OWRX_SWITCH_GAP_MS after the last at the soonest. False: the socket
 * failed. */
static bool follow(owrx_sess_t *s, const owrx_link_t *l, sent_t *z, const owrx_said_t *said, aud_t *a,
                   int64_t now, const char *tag)
{
    owrx_ctl_t c;
    l->ctl(l->ctx, &c);
    owrx_retune(said, &c.t);
    c.t.sq = owrx_squelch_db(said, c.sq_pct);
    char *cmd = s->cmd;
    const size_t cap = sizeof s->cmd;
    if (c.band_seq != z->band_seq) {
        z->band_seq = c.band_seq;
        if (c.band[0]) {
            snprintf(z->band, sizeof z->band, "%s", c.band);
            z->band_due = true;
        }
    }
    if (z->band_due && now - z->t_band >= OWRX_SWITCH_GAP_MS * 1000LL) {
        z->band_due = false;
        z->t_band = now;
        ESP_LOGI(tag, "selectprofile %s", z->band);
        if (!say(s, l, owrx_select_cmd(cmd, cap, z->band))) return false;
    }
    if (!z->tuned) {
        const int n = owrx_tune_cmd(cmd, cap, &c.t, said->center, said->rate, said->plus, OWRX_P_ALL);
        if (n < 0) {
            /* Said once: the next change of mode tries again. */
            ESP_LOGW(tag, "%s: no mode it plays", c.t.mod);
            z->t = c.t;
            z->tuned = true;
            return true;
        }
        if (!say(s, l, n) || !say(s, l, owrx_start_cmd(cmd, cap))) return false;
        ESP_LOGI(tag, "tuned: %lld Hz %s %ld..%ld, squelch %d dB, on %lld +- %ld", (long long)c.t.hz, c.t.mod,
                 (long)c.t.lo, (long)c.t.hi, c.t.sq, (long long)said->center, (long)(said->rate / 2));
        z->t = c.t;
        z->tuned = true;
        z->t_off = z->t_pass = z->t_sq = now;
        a->play = true;
        return true;
    }
    if (strcmp(c.t.mod, z->t.mod) || c.t.lo != z->t.lo || c.t.hi != z->t.hi) {
        if (now - z->t_pass < PASS_GAP_US) return true;
        const int n = owrx_tune_cmd(cmd, cap, &c.t, said->center, said->rate, said->plus,
                                    OWRX_P_PASS | OWRX_P_MOD | OWRX_P_OFFSET);
        if (n >= 0) {
            if (!say(s, l, n)) return false;
            z->t = c.t;
            z->t_pass = z->t_off = now;
        }
    } else if (c.t.hz != z->t.hz && now - z->t_off >= OFFSET_GAP_US) {
        if (!say(s, l, owrx_tune_cmd(cmd, cap, &c.t, said->center, said->rate, said->plus, OWRX_P_OFFSET)))
            return false;
        z->t.hz = c.t.hz;
        z->t_off = now;
    }
    if (c.t.sq != z->t.sq && now - z->t_sq >= SQ_GAP_US) {
        if (!say(s, l, owrx_tune_cmd(cmd, cap, &c.t, said->center, said->rate, said->plus, OWRX_P_SQ)))
            return false;
        z->t.sq = c.t.sq;
        z->t_sq = now;
    }
    return true;
}

/* ------------------------------------------------------------ the session */

owrx_end_t owrx_sess_run(owrx_sess_t *s, const owrx_url_t *u, uint32_t key, const owrx_link_t *l,
                         owrx_said_t *said)
{
    const char *tag = l->tag ? l->tag : "owrx";
    owrx_said_init(said);
    l->state(l->ctx, OWRX_ST_CONNECTING, said);
    char path[sizeof u->path + 8];
    if (owrx_path(u, "ws/", path, sizeof path) < 0) return OWRX_END_PROTOCOL;
    const wl_addr_t ad = { .host = u->host, .port = u->port, .tls = u->tls, .key = key };
    const wl_ask_t ask = { .go_on = l->go_on, .ctx = l->ctx, .tag = tag };
    wl_said_t ws;
    owrx_end_t end = end_of(wl_open(s->wl, &ad, path, &ask, &ws), &ws);
    if (ws.tls_port && l->moved) l->moved(l->ctx, ws.tls_port);
    if (end != OWRX_END_NONE) return end;

    if (!say(s, l, owrx_hello_cmd(s->cmd, sizeof s->cmd)) || !say(s, l, owrx_props_cmd(s->cmd, sizeof s->cmd))) {
        wl_close(s->wl, false);
        return OWRX_END_NO_ANSWER;
    }
    kiwi_dsp_init(&s->dsp);
    kiwi_dsp_reset(&s->dsp, OWRX_RATE);
    owrx_adpcm_reset(&s->ad);
    s->blk_n = 0;
    aud_t a = { .kind = OWRX_BIN_AUDIO };
    wl_flow_init(&a.w, &l->ring);
    const int64_t t_open = esp_timer_get_time();
    owrx_ctl_t c0;
    l->ctl(l->ctx, &c0);
    sent_t z = { .band_seq = c0.band_seq, .t_band = t_open };
    bool streaming = false, spanned = false;
    int64_t t_rx = t_open, t_rep = t_open, t_turn = t_open;
    uint64_t by_kind[3] = { 0 };                        /* audio, waterfall, text: bytes these 30 s */
    int fk = FK_NONE;
    bool need_type = false;
    uint32_t lost_seen = 0;

    while (end == OWRX_END_NONE) {
        wl_piece_t pc;
        const int r = wl_read(s->wl, LOOP_MS, &pc);
        int64_t now = esp_timer_get_time();
        if (r < 0) {
            end = streaming ? OWRX_END_CLOSED : OWRX_END_NO_ANSWER;
            break;
        }
        if (r == 1) {
            t_rx = now;
            if (pc.first) {
                fk = pc.op == WL_TEXT ? FK_TEXT : FK_NONE;
                need_type = pc.op == WL_BIN;
                if (fk == FK_TEXT) {
                    owrx_text_begin(said);
                    if (l->counts) {
                        l->counts->texts++;
                        if (pc.len > l->counts->biggest) l->counts->biggest = (uint32_t)pc.len;
                    }
                }
            }
            const uint8_t *p = pc.p;
            size_t n = pc.n;
            if (need_type && n) {
                fk = p[0];
                need_type = false;
                p++;
                n--;
                if (l->counts) {
                    if (fk == OWRX_BIN_AUDIO || fk == OWRX_BIN_HD) l->counts->audio++;
                    else if (fk == OWRX_BIN_FFT || fk == OWRX_BIN_FFT2) l->counts->fft++;
                    else l->counts->other++;
                }
            }
            if (fk == FK_TEXT) {
                by_kind[2] += n;
                owrx_text_feed(said, p, n);
                if (pc.last) {
                    const uint32_t ev = owrx_text_end(said);
                    if (pc.len > 16384)
                        ESP_LOGI(tag, "a %s of %lu bytes, let go by as it came", said->type[0] ? said->type : "frame",
                                 (unsigned long)pc.len);
                    if (ev & OWRX_EV_HELLO) {
                        ESP_LOGI(tag, "%s:%u%s: OpenWebRX %s", u->host, (unsigned)u->port, u->path, said->version);
                        l->state(l->ctx, OWRX_ST_CONNECTED, said);
                    }
                    if (ev & OWRX_EV_END) {
                        ESP_LOGW(tag, "%s:%u: %s%s%s", u->host, (unsigned)u->port, owrx_end_word(said->end),
                                 said->reason[0] ? ": " : "", said->reason);
                        end = said->end;
                        break;
                    }
                    if (ev & OWRX_EV_SDR) owrx_adpcm_reset(&s->ad);
                    if (ev & OWRX_EV_SPAN) {
                        if (!spanned)
                            ESP_LOGI(tag, "%s, audio %s, waterfall %s%s", said->plus ? "OpenWebRX+" : "OpenWebRX",
                                     said->audio_raw ? "int16" : "ADPCM", said->fft_raw ? "uncompressed" : "ADPCM",
                                     said->fft_raw ? " -- 147 kB/s of it" : "");
                        spanned = true;
                        l->span(l->ctx, said);
                        z.tuned = false;
                    }
                    if ((ev & OWRX_EV_LOG) && strstr(said->log, "locked")) {
                        /* A band refused: its DSP made anew, which plays
                         * nothing until start comes again. */
                        z.tuned = false;
                        owrx_adpcm_reset(&s->ad);
                    }
                    if ((ev & OWRX_EV_DEMOD)) ESP_LOGW(tag, "its demodulator: %s", said->log);
                    if ((ev & OWRX_EV_METER) && z.tuned) {
                        const float db = owrx_db(said->meter);
                        a.shut = z.t.sq > -150 && db < z.t.sq;
                        if (l->meter) l->meter(l->ctx, db);
                    }
                    if (l->told && (ev & ~(OWRX_EV_METER | OWRX_EV_SPAN | OWRX_EV_SDR)))
                        l->told(l->ctx, ev, said);
                }
            } else if (fk == OWRX_BIN_AUDIO || fk == OWRX_BIN_HD) {
                by_kind[0] += n;
                if (fk != a.kind) {
                    a.kind = (uint8_t)fk;
                    kiwi_dsp_reset(&s->dsp, fk == OWRX_BIN_HD ? OWRX_HD_RATE : OWRX_RATE);
                }
                const size_t m = said->audio_raw ? owrx_pcm_feed(&s->ad, p, n, s->pcm)
                                                 : owrx_adpcm_feed(&s->ad, p, n, s->pcm);
                if (m) {
                    a.t_audio = now;
                    if (a.filling) a.filling = false;
                    audio_in(s, l, &a, s->pcm, m, now);
                }
                if (s->ad.lost != lost_seen && l->counts) {
                    l->counts->lost += s->ad.lost - lost_seen;
                    lost_seen = s->ad.lost;
                }
                if (a.play && !streaming && a.seq) {
                    streaming = true;
                    t_rep = a.w.t_rep = now;
                    memset(&a.w.rep, 0, sizeof a.w.rep);
                    l->state(l->ctx, OWRX_ST_STREAMING, said);
                    ESP_LOGI(tag, "streaming from %s:%u%s", u->host, (unsigned)u->port, u->path);
                }
            } else {
                by_kind[1] += n;                        /* the waterfall: read, and let go */
            }
            if (pc.last) fk = FK_NONE;
            /* What has come, read on -- for TURN_US at most: the dial
             * waits no longer than that for its turn. */
            if (now - t_turn < TURN_US && wl_more(s->wl, 0)) continue;
            now = esp_timer_get_time();
        }
        t_turn = now;
        if (!l->go_on(l->ctx)) {
            end = OWRX_END_WANT;
            break;
        }
        if (!said->hello && now - t_open > HELLO_US) {
            ESP_LOGW(tag, "%s:%u: no answer to the hello", u->host, (unsigned)u->port);
            end = OWRX_END_NO_ANSWER;
            break;
        }
        if (now - t_rx > QUIET_US) {
            ESP_LOGW(tag, "%s:%u: nothing for %lld s", u->host, (unsigned)u->port, (long long)(QUIET_US / 1000000));
            end = OWRX_END_QUIET;
            break;
        }
        if (said->hello && spanned && !follow(s, l, &z, said, &a, now, tag)) {
            end = OWRX_END_CLOSED;
            break;
        }
        pause_fill(s, l, &a, now);
        if (streaming && now - t_rep >= REPORT_US) {
            const double sec = (double)(now - t_rep) / 1e6;
            ESP_LOGI(tag, "these %.0f s: audio %.1f kB/s, waterfall %.1f kB/s, text %.1f kB/s", sec,
                     (double)by_kind[0] / 1024 / sec, (double)by_kind[1] / 1024 / sec, (double)by_kind[2] / 1024 / sec);
            memset(by_kind, 0, sizeof by_kind);
            wl_report_t rep;
            wl_flow_report(&a.w, &rep, s->dsp.trim, now);
            if (l->report) l->report(l->ctx, &rep);
            t_rep = now;
        }
    }
    if (streaming && l->report) {                       /* the last of it */
        wl_report_t rep;
        wl_flow_report(&a.w, &rep, s->dsp.trim, esp_timer_get_time());
        l->report(l->ctx, &rep);
    }
    wl_close(s->wl, end == OWRX_END_WANT);
    uint64_t in = 0, out = 0;
    wl_counts(s->wl, &in, &out);
    ESP_LOGI(tag, "%s:%u: session over: %s (%llu bytes in, %llu out)", u->host, (unsigned)u->port,
             end == OWRX_END_WANT ? "left" : owrx_end_word(end), (unsigned long long)in, (unsigned long long)out);
    return end;
}

/* ------------------------------------------------------------ status.json */

static void status_body(void *ctx, const uint8_t *p, size_t n) { owrx_status_feed(ctx, p, n); }

owrx_end_t owrx_status_read(const owrx_url_t *u, uint32_t key, owrx_status_t *out, uint16_t *tls_port,
                            bool (*go_on)(void *), void *ctx, const char *tag)
{
    char path[sizeof u->path + 16];
    owrx_status_init(out);
    if (owrx_path(u, "status.json", path, sizeof path) < 0) return OWRX_END_PROTOCOL;
    const wl_addr_t ad = { .host = u->host, .port = u->port, .tls = u->tls, .key = key };
    const wl_ask_t ask = { .go_on = go_on, .ctx = ctx, .tag = tag ? tag : "owrx" };
    wl_said_t ws;
    const wl_end_t e = wl_get(&ad, path, status_body, out, STATUS_MAX, &ask, &ws);
    if (tls_port) *tls_port = ws.tls_port;
    if (e != WL_OK) return end_of(e, &ws);
    return owrx_status_end(out) ? OWRX_END_NONE : OWRX_END_NOT_OWRX;
}
