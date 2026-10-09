/* A WebSDR session (components/wsdr_proto's wsdr_sess.c on components/
 * websdr_link) on the PC, against tools/mock_wsdr.py: what a page reads
 * first, then one session, scripted, its audio into the shim's ring played
 * by the PC's clock. test/host/wsdr_link.py drives it and asks the mock what
 * it saw.
 *
 *   wsdr_link_host URL SECONDS [what ...]
 *
 *   dial=HZ mode=M lo=HZ hi=HZ  where it listens at the start (14074000 usb)
 *   tune=HZ@S                   the dial moved to HZ, S s in (repeatable)
 *   turn=HZ/MS@S                the dial turned from S s in, HZ every MS ms,
 *                               ten times
 *   mute=0|1@S  sq=0|1@S        mute, the squelch, S s in
 *   touch=S                     an act of the operator's every S s
 *   path=v11|plain              the stream path, not read from the site
 *
 * Its lines, for the driver:
 *   @INFO bands=N nbands=N idle=MS ini=HZ mode= plan=N name0= ...
 *   @PATH v11=0|1               what its sound script opens
 *   @TITLE "..."
 *   @MOVED port                 a redirect to https:// followed
 *   @STATE connecting|streaming rate
 *   @REPORT blocks gap lost held jumps left breaks level target trim
 *   @END word                   how the session ended
 *   @SUMMARY ...                what it played, and how */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_out.h"
#include "esp_timer.h"
#include "owrx_proto.h"
#include "shim.h"
#include "wsdr_sess.h"

typedef struct { int64_t at_us; char what; int64_t v; } step_t;

static wsdr_info_t   s_info;
static wsdr_ctl_t    s_ctl;
static step_t        s_steps[64];
static int           s_n_steps, s_next;
static int64_t       s_t0, s_end_us, s_touch_every;
static wsdr_counts_t s_counts;
static bool          s_streaming;
static int           s_reports;
static uint32_t      s_jumps, s_breaks, s_lost_blocks;
static int           s_dbm10 = -1500;
static unsigned      s_rate;

/* The band the dial is in, as the knob keeps it: the one in use where it
 * still covers it, else the first that does. */
static void place(void)
{
    const int b = wsdr_band_of(&s_info, s_ctl.t.dial_hz, s_ctl.t.band);
    if (b >= 0) {
        s_ctl.t.band = b;
        s_ctl.b = s_info.band[b];
    }
}

static void cb_ctl(void *ctx, wsdr_ctl_t *out)
{
    (void)ctx;
    const int64_t now = esp_timer_get_time() - s_t0;
    while (s_next < s_n_steps && now >= s_steps[s_next].at_us) {
        const step_t *st = &s_steps[s_next++];
        switch (st->what) {
        case 't':
            s_ctl.t.dial_hz = st->v;
            place();
            s_ctl.touch++;
            printf("@TUNE %lld band=%d\n", (long long)st->v, s_ctl.t.band);
            break;
        case 'm':
            s_ctl.mute = st->v;
            s_ctl.touch++;
            printf("@MUTE %d\n", (int)st->v);
            break;
        case 's':
            s_ctl.squelch = st->v;
            s_ctl.touch++;
            printf("@SQUELCH %d\n", (int)st->v);
            break;
        }
        fflush(stdout);
    }
    if (s_touch_every && now / s_touch_every != (now - 50000) / s_touch_every) s_ctl.touch++;
    *out = s_ctl;
}

static void cb_audio(void *ctx, const int16_t *pcm, size_t n)
{
    (void)ctx;
    audio_out_feed_pcm16(pcm, n, 1);
}

static size_t cb_queued(void *ctx) { (void)ctx; return audio_out_queued(); }
static void cb_preroll(void *ctx, size_t n) { (void)ctx; audio_out_set_preroll(n); }
static uint32_t cb_underruns(void *ctx)
{
    (void)ctx;
    audio_stats_t a;
    audio_out_stats(&a);
    return a.underruns;
}

static void cb_meter(void *ctx, int dbm10) { (void)ctx; s_dbm10 = dbm10; }

static void cb_state(void *ctx, wsdr_state_t st, unsigned rate)
{
    (void)ctx;
    printf("@STATE %s %u\n", st == WSDR_ST_STREAMING ? "streaming" : "connecting", rate);
    fflush(stdout);
    if (st == WSDR_ST_STREAMING && !s_streaming) {
        s_streaming = true;
        s_rate = rate;
        shim_pitch_reset();
    }
}

static bool cb_go_on(void *ctx) { (void)ctx; return esp_timer_get_time() < s_end_us; }

static void cb_report(void *ctx, const wl_report_t *r)
{
    (void)ctx;
    s_reports++;
    s_jumps += r->jumps;
    s_breaks += r->breaks;
    s_lost_blocks += r->lost;
    printf("@REPORT blocks=%u gap=%u lost=%u held=%u jumps=%u left=%u breaks=%u level=%u target=%u trim=%.5f\n",
           r->blocks, r->gap_ms, r->lost, r->held, r->jumps, r->left_ms, r->breaks, r->level_ms, r->target_ms,
           (double)r->trim);
    fflush(stdout);
}

static void cb_moved(void *ctx, uint16_t port)
{
    (void)ctx;
    printf("@MOVED %u\n", port);
    fflush(stdout);
}

static void add(char what, int64_t v, double at_s)
{
    if (s_n_steps >= (int)(sizeof s_steps / sizeof s_steps[0])) return;
    s_steps[s_n_steps++] = (step_t){ .at_us = (int64_t)(at_s * 1e6), .what = what, .v = v };
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: wsdr_link_host URL SECONDS [dial= mode= lo= hi= tune=HZ@S turn=HZ/MS@S mute=V@S "
                        "sq=V@S touch=S path=v11|plain]\n");
        return 2;
    }
    owrx_url_t u;
    if (!owrx_url(argv[1], &u)) {
        fprintf(stderr, "not an address: %s\n", argv[1]);
        return 2;
    }
    const double secs = atof(argv[2]);
    const wsdr_mode_t *m0 = wsdr_mode_named("usb");
    s_ctl.t = (wsdr_tune_t){ .dial_hz = 14074000, .mode = m0->mode, .lo = m0->lo, .hi = m0->hi };
    snprintf(s_ctl.name, sizeof s_ctl.name, "MOCKTEST");
    int path = -1;
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        const char *at = strchr(a, '@');
        const double when = at ? atof(at + 1) : 0;
        if (!strncmp(a, "dial=", 5)) s_ctl.t.dial_hz = atoll(a + 5);
        else if (!strncmp(a, "mode=", 5)) {
            const wsdr_mode_t *m = wsdr_mode_named(a + 5);
            if (m) {
                s_ctl.t.mode = m->mode;
                s_ctl.t.lo = m->lo;
                s_ctl.t.hi = m->hi;
                s_ctl.t.cw = !strcmp(m->name, "cw");
            }
        } else if (!strncmp(a, "lo=", 3)) s_ctl.t.lo = atoi(a + 3);
        else if (!strncmp(a, "hi=", 3)) s_ctl.t.hi = atoi(a + 3);
        else if (!strncmp(a, "tune=", 5) && at) add('t', atoll(a + 5), when);
        else if (!strncmp(a, "turn=", 5) && at) {
            const int64_t step = atoll(a + 5);
            const char *sl = strchr(a, '/');
            const double ms = sl ? atof(sl + 1) : 50;
            int64_t hz = s_ctl.t.dial_hz;
            for (int k = 0; k < 10; k++) add('t', hz += step, when + k * ms / 1000.0);
        } else if (!strncmp(a, "mute=", 5) && at) add('m', atoi(a + 5), when);
        else if (!strncmp(a, "sq=", 3) && at) add('s', atoi(a + 3), when);
        else if (!strncmp(a, "touch=", 6)) s_touch_every = (int64_t)(atof(a + 6) * 1e6);
        else if (!strcmp(a, "path=v11")) path = 1;
        else if (!strcmp(a, "path=plain")) path = 0;
    }
    /* The steps in time order: the script's own, whatever order it came in. */
    for (int i = 1; i < s_n_steps; i++)
        for (int j = i; j > 0 && s_steps[j].at_us < s_steps[j - 1].at_us; j--) {
            const step_t t = s_steps[j];
            s_steps[j] = s_steps[j - 1];
            s_steps[j - 1] = t;
        }

    wsdr_where_t w = { .host = u.host, .prefix = u.path, .port = u.port, .tls = u.tls, .key = owrx_key(&u) };
    uint16_t tp = 0;
    const wsdr_end_t ie = wsdr_info_read(&w, &s_info, &tp, NULL, NULL, "host");
    if (tp) {
        printf("@MOVED %u\n", tp);
        w.tls = true;
        w.port = tp;
    }
    printf("@INFO end=%s bands=%d nbands=%d idle=%u ini=%lld mode=%s plan=%d", wsdr_end_word(ie), s_info.n_bands,
           s_info.nbands, (unsigned)s_info.idle_ms, (long long)s_info.ini_hz, s_info.ini_mode, s_info.n_plan);
    for (int i = 0; i < s_info.n_bands; i++) printf(" %s", s_info.band[i].name);
    printf("\n");
    bool v11 = false;
    const wsdr_end_t pe = wsdr_path_read(&w, &v11, NULL, NULL, "host");
    printf("@PATH end=%s v11=%d\n", wsdr_end_word(pe), v11);
    char title[64];
    wsdr_title_read(&w, title, sizeof title, NULL, NULL, "host");
    printf("@TITLE \"%s\"\n", title);
    fflush(stdout);
    if (path >= 0) v11 = path;
    place();

    static wsdr_sess_t *s;                  /* kept for the program's life, as on the knob */
    s = wsdr_sess_new();
    const wsdr_link_t l = {
        .ctl = cb_ctl, .audio = cb_audio,
        .ring = { .queued = cb_queued, .target = audio_out_preroll(), .room = audio_out_room(), .trim = true,
                  .target_max = audio_out_preroll_max(), .preroll = cb_preroll, .underruns = cb_underruns,
                  .tag = "host" },
        .meter = cb_meter, .state = cb_state, .go_on = cb_go_on, .report = cb_report, .moved = cb_moved,
        .counts = &s_counts, .tag = "host",
    };
    s_t0 = esp_timer_get_time();
    s_end_us = s_t0 + (int64_t)(secs * 1e6);
    const wsdr_end_t e = wsdr_sess_run(s, &w, v11, s_info.idle_ms, &l);
    const double ran = (double)(esp_timer_get_time() - s_t0) / 1e6;
    uint64_t played = 0;
    const double pitch = s_streaming ? shim_pitch(&played) : 0;
    audio_stats_t as;
    audio_out_stats(&as);
    printf("@END %s\n", e == WSDR_END_WANT ? "left" : wsdr_end_word(e));
    printf("@SUMMARY ran=%.1f played=%.1f pitch=%.1f underruns=%u dropped=%u sent=%u msgs=%u blocks=%u silent=%u "
           "biggest=%u toolong=%u reports=%d jumps=%u breaks=%u lost_blocks=%u dbm=%.1f rate=%u dial=%lld band=%d\n",
           ran, (double)played / 24000.0, pitch, as.underruns, as.dropped, s_counts.sent, s_counts.msgs,
           s_counts.blocks, s_counts.silent, s_counts.biggest, s_counts.dropped, s_reports, s_jumps, s_breaks,
           s_lost_blocks, s_dbm10 / 10.0, s_rate, (long long)s_ctl.t.dial_hz, s_ctl.t.band);
    fflush(stdout);
    return 0;
}
