/* OpenWebRX's session (components/owrx_proto's owrx_sess.c on components/
 * websdr_link) on the PC, against tools/mock_owrx.py: one session, scripted,
 * its audio into the shim's ring played by the PC's clock. test/host/
 * owrx_link.py drives it and asks the mock what it saw.
 *
 *   owrx_link_host URL SECONDS [what ...]
 *
 *   dial=HZ mode=M lo=HZ hi=HZ  where it listens at the start (7074500 usb)
 *   sq=PCT                      the squelch, 0-100 %
 *   tune=HZ@S                   the dial moved to HZ, S s in (repeatable)
 *   band=ID@S                   a band chosen, S s in (repeatable)
 *   status                      its status.json first: @STATUS ...
 *
 * Its lines, for the driver:
 *   @MOVED port                 a redirect to https:// followed
 *   @STATE connecting|connected|streaming
 *   @SPAN center rate dial mode the band it is on, and the dial on it
 *   @TOLD ev name="" bands=N clients=N log=""
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
#include "owrx_sess.h"
#include "shim.h"

typedef struct { int64_t at_us; int64_t hz; char band[OWRX_ID_MAX]; } step_t;

static owrx_ctl_t   s_ctl;
static step_t       s_steps[16];
static int          s_n_steps, s_next;
static int64_t      s_t0, s_end_us;
static owrx_counts_t s_counts;
static bool         s_streaming;
static int          s_spans, s_reports;
static uint32_t     s_jumps, s_breaks, s_lost_blocks;

static void cb_ctl(void *ctx, owrx_ctl_t *out)
{
    (void)ctx;
    const int64_t now = esp_timer_get_time() - s_t0;
    while (s_next < s_n_steps && now >= s_steps[s_next].at_us) {
        const step_t *st = &s_steps[s_next++];
        if (st->band[0]) {
            snprintf(s_ctl.band, sizeof s_ctl.band, "%s", st->band);
            s_ctl.band_seq++;
            printf("@BAND %s\n", st->band);
        } else {
            s_ctl.t.hz = st->hz;
            printf("@TUNE %lld\n", (long long)st->hz);
        }
        fflush(stdout);
    }
    *out = s_ctl;
}

static void cb_span(void *ctx, const owrx_said_t *said)
{
    (void)ctx;
    owrx_retune(said, &s_ctl.t);
    s_spans++;
    printf("@SPAN %lld %ld %lld %s\n", (long long)said->center, (long)said->rate, (long long)s_ctl.t.hz, s_ctl.t.mod);
    fflush(stdout);
}

static void cb_told(void *ctx, uint32_t ev, const owrx_said_t *said)
{
    (void)ctx;
    printf("@TOLD %04x name=\"%s\" bands=%u seen=%u clients=%d log=\"%s\" plus=%d\n", (unsigned)ev, said->name,
           said->n_bands, said->bands_seen, said->clients, said->log, said->plus);
    fflush(stdout);
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

static float s_db = -150;
static void cb_meter(void *ctx, float db) { (void)ctx; s_db = db; }

static void cb_state(void *ctx, owrx_state_t st, const owrx_said_t *said)
{
    (void)ctx;
    (void)said;
    static const char *W[] = { "connecting", "connected", "streaming" };
    printf("@STATE %s\n", W[st]);
    fflush(stdout);
    if (st == OWRX_ST_STREAMING && !s_streaming) {
        s_streaming = true;
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

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: owrx_link_host URL SECONDS [dial=HZ mode=M lo=HZ hi=HZ sq=PCT tune=HZ@S band=ID@S status]\n");
        return 2;
    }
    owrx_url_t u;
    if (!owrx_url(argv[1], &u)) {
        fprintf(stderr, "not an address: %s\n", argv[1]);
        return 2;
    }
    const double secs = atof(argv[2]);
    s_ctl.t = (owrx_tune_t){ .hz = 7074500, .mod = "usb", .lo = 300, .hi = 3000, .sq = -150 };
    bool status = false;
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];
        const char *at = strchr(a, '@');
        if (!strncmp(a, "dial=", 5)) s_ctl.t.hz = atoll(a + 5);
        else if (!strncmp(a, "mode=", 5)) snprintf(s_ctl.t.mod, sizeof s_ctl.t.mod, "%s", a + 5);
        else if (!strncmp(a, "lo=", 3)) s_ctl.t.lo = atoi(a + 3);
        else if (!strncmp(a, "hi=", 3)) s_ctl.t.hi = atoi(a + 3);
        else if (!strncmp(a, "sq=", 3)) s_ctl.sq_pct = (uint8_t)atoi(a + 3);
        else if (!strcmp(a, "status")) status = true;
        else if ((!strncmp(a, "tune=", 5) || !strncmp(a, "band=", 5)) && at && s_n_steps < 16) {
            step_t *st = &s_steps[s_n_steps++];
            memset(st, 0, sizeof *st);
            st->at_us = (int64_t)(atof(at + 1) * 1e6);
            if (a[0] == 't') st->hz = atoll(a + 5);
            else snprintf(st->band, sizeof st->band, "%.*s", (int)(at - a - 5), a + 5);
        }
    }
    const uint32_t key = owrx_key(&u);
    if (status) {
        static owrx_status_t st;
        uint16_t tp = 0;
        const owrx_end_t e = owrx_status_read(&u, key, &st, &tp, NULL, NULL, "host");
        printf("@STATUS end=%s ok=%d name=\"%s\" version=%s plus=%d sdrs=%u profiles=%u tls_port=%u\n",
               owrx_end_word(e), st.ok, st.name, st.version, owrx_plus_version(st.version), st.n_sdrs, st.profiles, tp);
        fflush(stdout);
        if (tp) {
            u.tls = true;
            u.port = tp;
        }
    }
    static owrx_sess_t *s;                  /* kept for the program's life, as on the knob */
    s = owrx_sess_new();
    static owrx_said_t said;
    const owrx_link_t l = {
        .ctl = cb_ctl, .span = cb_span, .told = cb_told, .audio = cb_audio,
        .ring = { .queued = cb_queued, .target = audio_out_preroll(), .room = audio_out_room(), .trim = true,
                  .target_max = audio_out_preroll_max(), .preroll = cb_preroll, .underruns = cb_underruns,
                  .tag = "host" },
        .meter = cb_meter, .state = cb_state, .go_on = cb_go_on, .report = cb_report, .moved = cb_moved,
        .counts = &s_counts, .tag = "host",
    };
    s_t0 = esp_timer_get_time();
    s_end_us = s_t0 + (int64_t)(secs * 1e6);
    const owrx_end_t e = owrx_sess_run(s, &u, key, &l, &said);
    const double ran = (double)(esp_timer_get_time() - s_t0) / 1e6;
    uint64_t played = 0;
    const double pitch = s_streaming ? shim_pitch(&played) : 0;
    audio_stats_t as;
    audio_out_stats(&as);
    printf("@END %s\n", e == OWRX_END_WANT ? "left" : owrx_end_word(e));
    printf("@SUMMARY ran=%.1f played=%.1f pitch=%.1f underruns=%u dropped=%u texts=%u audio=%u fft=%u other=%u "
           "biggest=%u lost=%u sent=%u spans=%d reports=%d jumps=%u breaks=%u lost_blocks=%u db=%.1f "
           "center=%lld dial=%lld mode=%s bands=%u plus=%d version=%s\n",
           ran, (double)played / 24000.0, pitch, as.underruns, as.dropped, s_counts.texts, s_counts.audio,
           s_counts.fft, s_counts.other, s_counts.biggest, s_counts.lost, s_counts.sent, s_spans, s_reports,
           s_jumps, s_breaks, s_lost_blocks, (double)s_db, (long long)said.center, (long long)s_ctl.t.hz,
           s_ctl.t.mod, said.n_bands, said.plus, said.version);
    fflush(stdout);
    return 0;
}
