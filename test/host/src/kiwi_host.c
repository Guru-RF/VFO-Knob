/* The kiwi firmware's receiver -- components/kiwi_client on components/
 * kiwi_proto, with sdr_rx's list, as the firmware builds them -- run on the
 * PC against tools/mock_kiwi.py. One run of this program is one boot of the
 * knob; its NVS is a file that outlives the run, so a run after a run is a
 * restart. test/host/kiwi_client_limits.py drives it and asks the mock what it
 * saw.
 *
 *   kiwi_host NVSFILE CMD...
 *
 *   save [NAME=][PW@]ADDR[/IPL] ...  the page's Save: the list replaced, each
 *                              one's passwords kept unless given, the right
 *                              ear's receiver kept through the reshuffle. ADDR
 *                              as the page takes it: host:port, http://host[:port],
 *                              https://host[:port] -- TLS, the CA the run trusts
 *                              in KIWI_TEST_CA; a receiver's address before a
 *                              redirect moved it is that receiver (sdr_same)
 *   list                       the list as it is now: @LIST N host port tls kport
 *   use N                      the operator's act: the dial's, the page's In use,
 *                              the API's receiver= (kiwi_rx_use(N, true)): @USE
 *                              N ok|refused
 *   right N                    the right ear (components/sdr_rx, as app_main
 *                              brings it up on the kiwi firmware, following
 *                              the dial): the dial's commit, the API's sdr=
 *                              (sdr_rx_choose; -1 off): @RIGHT N ok|refused
 *   balance B                  the balance, -100..100
 *   rstate                     @RSTATE want=.. sel=.. streaming=.. note=".." state=".." ...
 *                              range=LO..HI: where its receiver tunes (0..0 not said)
 *   raudio                     the right ear's ring: @RAUDIO level=.. dropped=.. underruns=..
 *   rpitch S                   the right ear's pitch over S s: @RPITCH hz=.. samples=..
 *   runtil WORD S              until the right ear's state says WORD, S seconds at most
 *   relisten                   LISTEN AGAIN tapped (radio_choose(0))
 *   test N                     the page's Test of entry N: @TEST <json>
 *   tune HZ | mode M           the dial: radio_goto_freq, radio_set_mode
 *   filter LO HI | agc A       the filter editor's passband, the AGC's
 *   nr N | squelch P           the noise filter (0 off .. 3), the squelch (0-100 %):
 *                              the face's editors, the API's gain= and squelch=
 *   ident WHO                  the page's "your name, for their owners" (sdr_ident_save)
 *   label N                    entry N's name on the dial: @LABEL N "..."
 *   touches N GAP              a finger on the glass N times, GAP s apart -- as
 *                              the face's task hands it on (radio_user_activity)
 *   pitch S                    the audio's pitch over S s: @PITCH hz=.. samples=..
 *   audio                      the ring: @AUDIO level=.. dropped=.. underruns=.. feeds=..
 *   nvs                        flash written so far: @NVS writes=..
 *   wait S                     seconds
 *   until WORD S               until the state says WORD, S seconds at most
 *   state                      @STATE link=.. rx=.. why=".." state=".." ... nr=.. sq=.. ovl=..
 *   marks                      @MARK per entry a mark holds back, @REST per one
 *                              whose mark rests, then @MARKS n (those held)
 *   off                        power off now: what is not in flash is lost
 *   quit                       flash what waits, then end
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "audio_out.h"
#include "kiwi.h"
#include "kiwi_mark.h"
#include "kiwi_proto.h"
#include "kiwi_sess.h"
#include "radio.h"
#include "sdr_rx.h"
#include "shim.h"

static const char *LINK[] = { "DOWN", "CONNECTING", "GREETING", "READY", "DEGRADED" };

/* "[name=][password@]ADDR[/ipl]", ADDR as the page takes it: https://host[:port]
 * (TLS, 443), http://host[:port] (80) or host[:port] (8073). */
static bool parse_rx(const char *arg, sdr_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    const char *eq = strchr(arg, '=');
    if (eq) {
        snprintf(c->name, sizeof c->name, "%.*s", (int)(eq - arg), arg);
        arg = eq + 1;
    }
    const char *at = strchr(arg, '@');
    if (at) {
        snprintf(c->pass, sizeof c->pass, "%.*s", (int)(at - arg), arg);
        arg = at + 1;
    }
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s", arg);
    char *s = strstr(tmp, "://");
    char *ipl = strchr(s ? s + 3 : tmp, '/');
    if (ipl) { *ipl++ = 0; snprintf(c->ipl, sizeof c->ipl, "%s", ipl); }
    return sdr_parse_addr(tmp, c);
}

/* What a save's argument gives besides its address, as parse_rx reads it: a
 * password (PW@), a time-limit password (/IPL, past the address's "://"). */
static void given(const char *arg, bool *pw, bool *ipl)
{
    const char *eq = strchr(arg, '=');
    if (eq) arg = eq + 1;
    const char *at = strchr(arg, '@');
    *pw = at != NULL;
    if (at) arg = at + 1;
    const char *s = strstr(arg, "://");
    *ipl = strchr(s ? s + 3 : arg, '/') != NULL;
}

static void say_state(const char *tag)
{
    static radio_status_t st;
    static kiwi_info_t ki;
    radio_get_status(&st);
    kiwi_info(&ki);
    shim_say("@%s link=%s rx=%d why=\"%s\" state=\"%s\" server=\"%s\" line2=\"%s\" line3=\"%s\" "
             "choice=%u f=%lld mode=%s lo=%ld hi=%ld fmax=%lld dbm=%.1f model=\"%s\" samples=%llu t=%.3f "
             "rej=%lu echo=%lu nr=%d/%s sq=%u/%d ovl=%d audible=%d",
             tag, LINK[st.link], kiwi_rx_active(), st.link_why, ki.state, st.server, ki.line2, ki.line3,
             (unsigned)st.n_choices, (long long)st.f_display, st.mode, (long)st.filt_lo, (long)st.filt_hi,
             (long long)st.f_max, (double)st.smeter_dbm, st.model, (unsigned long long)shim_samples, shim_now(),
             (unsigned long)st.rejects, (unsigned long)st.echoes, st.have_gain ? st.gain : -1,
             st.have_gain && st.gain >= 0 && st.gain < st.n_gain_names ? st.gain_names[st.gain] : "",
             (unsigned)st.squelch_pct, st.has_squelch && st.have_squelch, ki.ovl, kiwi_audible());
}

/* webcfg.c's sdr_post_h on kiwi, without the form: each one's passwords are
 * the ones the same receiver had (sdr_same), unless given; the right ear's
 * receiver kept through the reshuffle (`was`), the left ear's following by
 * itself. */
static void page_save(int argc, char **argv, int *i)
{
    static sdr_cfg_t list[SDR_MAX];
    int k = 0, sel = -1;
    const int cur = sdr_rx_selected();
    while (*i + 1 < argc && k < SDR_MAX && strchr(argv[*i + 1], ':')) {
        sdr_cfg_t *c = &list[k];
        bool pw, ipl;
        given(argv[*i + 1], &pw, &ipl);
        parse_rx(argv[++*i], c);
        int was = -1;
        for (int j = 0; j < sdr_count() && was < 0; j++) {
            sdr_cfg_t o;
            /* As webcfg.c has it: the same receiver -- at its address, or at
             * the one a redirect moved it from, the move kept -- its key, and
             * its passwords where none are given. */
            if (!sdr_get(j, &o) || !sdr_same(&o, c)) continue;
            was = j;
            if (!pw)  snprintf(c->pass, sizeof c->pass, "%s", o.pass);
            if (!ipl) snprintf(c->ipl, sizeof c->ipl, "%s", o.ipl);
        }
        if (cur >= 0 && was == cur) sel = k;
        k++;
    }
    sdr_save(list, k, sel);
    shim_say("@SAVED %d", k);
}

/* webcfg.c's in_either_ear: the receiver in either ear, its session on its
 * way or playing -- a Test of it answers from that. */
static bool in_either_ear(uint32_t hp) { return kiwi_rx_in_session(hp) || sdr_rx_in_session(hp); }

static void say_right(const char *tag)
{
    sdr_status_t s;
    sdr_rx_status(&s);
    shim_say("@%s want=%d sel=%d streaming=%d note=\"%s\" state=\"%s\" samples=%llu dbm=%.1f t=%.3f "
             "range=%lld..%lld", tag, sdr_rx_selected(), s.sel, s.streaming, s.note, s.state,
             (unsigned long long)shim_sdr_samples, (double)s.smeter_dbm, shim_now(), (long long)s.lo_hz,
             (long long)s.hi_hz);
}

/* app_main's: the right ear follows the knob's dial, every 50 ms. */
static void *follow(void *arg)
{
    (void)arg;
    static radio_status_t st;
    for (;;) {
        radio_get_status(&st);
        if (st.f_display > 0) sdr_rx_tune(st.f_display, st.mode, st.filt_lo, st.filt_hi);
        usleep(50000);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: kiwi_host NVSFILE CMD...\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    setenv("SHIM_NVS", argv[1], 1);
    /* As app_main: the list and the marks at bring-up, then the right ear,
     * the client once WiFi is up -- and the dial followed by the right ear. */
    sdr_list_init();
    kiwi_mark_init("kiwi");
    sdr_rx_init();
    sdr_rx_quiet_cb(kiwi_audible);
    radio_start("Kiwi", 8073, "", "");
    pthread_t ft;
    pthread_create(&ft, NULL, follow, NULL);
    pthread_detach(ft);
    say_state("BOOT");

    for (int i = 2; i < argc; i++) {
        const char *cmd = argv[i];
        const char *arg = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(cmd, "save")) {
            page_save(argc, argv, &i);
        } else if (!strcmp(cmd, "use")) {
            shim_say("@USE %d %s", atoi(arg), kiwi_rx_use(atoi(arg), true) ? "ok" : "refused");
            i++;
        } else if (!strcmp(cmd, "right")) {
            shim_say("@RIGHT %d %s", atoi(arg), sdr_rx_choose(atoi(arg)) ? "ok" : "refused");
            i++;
        } else if (!strcmp(cmd, "balance")) {
            sdr_rx_set_balance((int8_t)atoi(arg));
            i++;
        } else if (!strcmp(cmd, "rstate")) {
            say_right("RSTATE");
        } else if (!strcmp(cmd, "raudio")) {
            size_t level;
            uint32_t dropped, underruns;
            shim_sdr_audio(&level, &dropped, &underruns);
            shim_say("@RAUDIO level=%zu dropped=%lu underruns=%lu", level, (unsigned long)dropped,
                     (unsigned long)underruns);
        } else if (!strcmp(cmd, "rpitch")) {
            shim_sdr_pitch_reset();
            usleep((useconds_t)(atof(arg) * 1e6));
            uint64_t n = 0;
            const double hz = shim_sdr_pitch(&n);
            shim_say("@RPITCH hz=%.1f samples=%llu", hz, (unsigned long long)n);
            i++;
        } else if (!strcmp(cmd, "runtil")) {
            const char *word = arg;
            const double secs = i + 2 < argc ? atof(argv[i + 2]) : 10;
            bool hit = false;
            for (int t = 0; t < secs * 20 && !hit; t++) {
                sdr_status_t s;
                sdr_rx_status(&s);
                hit = strstr(s.state, word) != NULL;
                if (!hit) usleep(50000);
            }
            if (!hit) shim_say("@TIMEOUT waiting for the right ear's \"%s\"", word);
            say_right("RSTATE");
            i += 2;
        } else if (!strcmp(cmd, "relisten")) {
            radio_choose(0);
        } else if (!strcmp(cmd, "test")) {
            static char json[1024];
            sdr_cfg_t c;
            if (sdr_get(atoi(arg), &c)) {
                /* As webcfg.c's sdr_test_h: the one in use in either ear,
                 * its session on its way or playing, answers from that. */
                sdr_test(&c, in_either_ear, json, sizeof json, "kiwi");
                shim_say("@TEST %s", json);
            } else {
                shim_say("@TEST {\"error\":\"no entry %s\"}", arg);
            }
            i++;
        } else if (!strcmp(cmd, "tune")) {
            radio_goto_freq(atoll(arg));
            i++;
        } else if (!strcmp(cmd, "mode")) {
            radio_set_mode(arg);
            i++;
        } else if (!strcmp(cmd, "filter")) {
            radio_set_filter(atoi(arg), i + 2 < argc ? atoi(argv[i + 2]) : 0);
            i += 2;
        } else if (!strcmp(cmd, "agc")) {
            radio_set_agc(arg);
            i++;
        } else if (!strcmp(cmd, "nr")) {
            radio_set_gain((int8_t)atoi(arg));
            i++;
        } else if (!strcmp(cmd, "squelch")) {
            radio_set_squelch((uint8_t)atoi(arg));
            i++;
        } else if (!strcmp(cmd, "ident")) {
            char who[KIWI_IDENT_MAX];
            const esp_err_t e = sdr_ident_save(arg);
            sdr_ident(who, sizeof who);
            shim_say("@IDENT %s \"%s\"", e == ESP_OK ? "saved" : "failed", who);
            i++;
        } else if (!strcmp(cmd, "label")) {
            char l[16] = "";
            const bool ok = kiwi_rx_label(atoi(arg), l, sizeof l);
            shim_say("@LABEL %s %s\"%s\"", arg, ok ? "" : "none ", l);
            i++;
        } else if (!strcmp(cmd, "touches")) {
            const int n = atoi(arg);
            const double gap = i + 2 < argc ? atof(argv[i + 2]) : 1;
            for (int t = 0; t < n; t++) {
                radio_user_activity();
                shim_say("@TOUCH %.3f", shim_now());
                usleep((useconds_t)(gap * 1e6));
            }
            i += 2;
        } else if (!strcmp(cmd, "pitch")) {
            shim_pitch_reset();
            usleep((useconds_t)(atof(arg) * 1e6));
            uint64_t n = 0;
            const double hz = shim_pitch(&n);
            shim_say("@PITCH hz=%.1f samples=%llu", hz, (unsigned long long)n);
            i++;
        } else if (!strcmp(cmd, "audio")) {
            audio_stats_t a;
            audio_out_stats(&a);
            shim_say("@AUDIO level=%zu dropped=%lu underruns=%lu feeds=%lu", audio_out_queued(),
                     (unsigned long)a.dropped, (unsigned long)a.underruns, (unsigned long)a.frames);
        } else if (!strcmp(cmd, "nvs")) {
            shim_say("@NVS writes=%lu t=%.3f", (unsigned long)shim_nvs_writes, shim_now());
        } else if (!strcmp(cmd, "wait")) {
            usleep((useconds_t)(atof(arg) * 1e6));
            i++;
        } else if (!strcmp(cmd, "until")) {
            const char *word = arg;
            const double secs = i + 2 < argc ? atof(argv[i + 2]) : 10;
            bool hit = false;
            for (int t = 0; t < secs * 20 && !hit; t++) {
                static kiwi_info_t ki;
                kiwi_info(&ki);
                hit = strstr(ki.state, word) != NULL;
                if (!hit) usleep(50000);
            }
            if (!hit) shim_say("@TIMEOUT waiting for \"%s\"", word);
            say_state("STATE");
            i += 2;
        } else if (!strcmp(cmd, "state")) {
            say_state("STATE");
        } else if (!strcmp(cmd, "marks")) {
            int n = 0;
            for (int j = 0; j < sdr_count(); j++) {
                sdr_cfg_t c;
                kiwi_mark_t m;
                if (!sdr_get(j, &c)) continue;
                const uint32_t hp = sdr_hp(&c);
                if (kiwi_mark_get(hp, &m)) {
                    shim_say("@MARK %d %s:%u strikes=%u kind=%u boot=%lu utc=%lu unsure=%d hg=%d", j, c.host,
                             (unsigned)c.port, (unsigned)m.strikes, (unsigned)m.kind, (unsigned long)m.rx_boot,
                             (unsigned long)m.mark_utc, !!(m.flags & KIWI_MARK_UNSURE), !!(m.flags & KIWI_MARK_HG));
                    n++;
                } else if (kiwi_mark_find(hp, &m)) {
                    /* At rest: it holds nothing back, its strikes kept. */
                    shim_say("@REST %d %s:%u strikes=%u", j, c.host, (unsigned)c.port, (unsigned)m.strikes);
                }
            }
            shim_say("@MARKS %d", n);
        } else if (!strcmp(cmd, "list")) {
            for (int j = 0; j < sdr_count(); j++) {
                sdr_cfg_t c;
                if (sdr_get(j, &c))
                    shim_say("@LIST %d %s %u tls=%d kport=%u", j, c.host, (unsigned)c.port, c.tls, (unsigned)c.kport);
            }
        } else if (!strcmp(cmd, "off")) {
            shim_say("@OFF");
            _exit(0);                         /* timers and all, mid-flight */
        } else if (!strcmp(cmd, "quit")) {
            kiwi_mark_flush();
            for (int t = 0; t < 100 && !(shim_timers_idle() && kiwi_mark_saved()); t++) usleep(100000);
            shim_say("@QUIT saved=%d", kiwi_mark_saved());
            _exit(0);
        } else {
            fprintf(stderr, "kiwi_host: what is \"%s\"?\n", cmd);
            return 2;
        }
    }
    shim_say("@END");
    _exit(0);
}
