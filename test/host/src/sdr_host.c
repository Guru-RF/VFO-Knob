/* The knob's web-SDR code -- components/sdr_rx and components/kiwi_proto, as
 * the firmware builds them -- run on the PC against tools/mock_kiwi.py. One
 * run of this program is one boot of the knob; its NVS is a file that
 * outlives the run, so a run after a run is a restart. test/host/kiwi_limits.py
 * drives it, boot after boot, and asks the mock what it saw.
 *
 *   sdr_host NVSFILE CMD...
 *
 *   save [NAME=][PW@]ADDR[/IPL] ...  the page's Save: the list replaced, the
 *                              one listened to kept through the reshuffle, its
 *                              passwords kept unless given. ADDR as the page
 *                              takes it: host:port, http://host[:port],
 *                              https://host[:port] -- TLS, the CA the run trusts
 *                              in KIWI_TEST_CA; a receiver's address before a
 *                              redirect moved it is that receiver (sdr_same)
 *   list                       the list as it is now: @LIST N host port tls kport
 *   rawsave HOST:PORT ...      sdr_save alone, with no reshuffle
 *   choose N | select N        the operator's act / the plain setter (-1 none)
 *   ident WHO                  the page's "your name, for their owners" (sdr_ident_save)
 *   label N                    entry N's name on the dial: @LABEL N "..."
 *   radio HZ MODE LO HI        the radio, as the SDR follows it (sdr_rx_tune)
 *   test N                     the page's Test of entry N: @TEST <json>
 *   pitch S                    the SDR's audio, its pitch over S s: @PITCH hz=.. samples=..
 *   audio                      the SDR's ring: @AUDIO level=.. dropped=.. underruns=..
 *   wait S                     seconds
 *   until WORD S               until the state says WORD, S seconds at most
 *   state                      @STATE want=.. sel=.. streaming=.. note=".." state=".." ...
 *                              range=LO..HI: where its receiver tunes (0..0 not said)
 *   marks                      @MARK per entry a mark holds back, @REST per one
 *                              whose mark rests, then @MARKS n (those held)
 *   busy 0|1                   the radio on the air: flash waits
 *   off                        power off now: what is not in flash is lost
 *   quit                       flash what waits, then end
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "kiwi_mark.h"
#include "kiwi_proto.h"
#include "sdr_rx.h"
#include "shim.h"

static volatile bool s_on_air;
static bool on_air(void) { return s_on_air; }

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
    sdr_status_t s;
    sdr_rx_status(&s);
    shim_say("@%s want=%d sel=%d streaming=%d note=\"%s\" state=\"%s\" samples=%llu dbm=%.1f t=%.3f "
             "range=%lld..%lld", tag, sdr_rx_selected(), s.sel, s.streaming, s.note, s.state,
             (unsigned long long)shim_sdr_samples, (double)s.smeter_dbm, shim_now(), (long long)s.lo_hz,
             (long long)s.hi_hz);
}

/* webcfg.c's sdr_post_h, without the form: each entry's `was` is where the
 * same receiver stood (sdr_same), and its passwords are kept unless given. */
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
    shim_say("@SAVED %d sel=%d", k, sel);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sdr_host NVSFILE CMD...\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    setenv("SHIM_NVS", argv[1], 1);
    sdr_rx_init();
    kiwi_mark_busy_cb(on_air);
    /* The radio, as app_main passes it on once it is ready. */
    sdr_rx_tune(14074000, "usb", 300, 2700);
    say_state("BOOT");

    for (int i = 2; i < argc; i++) {
        const char *cmd = argv[i];
        const char *arg = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(cmd, "save")) {
            page_save(argc, argv, &i);
        } else if (!strcmp(cmd, "rawsave")) {
            static sdr_cfg_t list[SDR_MAX];
            int k = 0;
            while (i + 1 < argc && k < SDR_MAX && strchr(argv[i + 1], ':')) parse_rx(argv[++i], &list[k++]);
            sdr_save(list, k, sdr_rx_selected());   /* the place kept, whatever stands there now */
            shim_say("@SAVED %d raw", k);
        } else if (!strcmp(cmd, "choose")) {
            sdr_rx_choose(atoi(arg));
            i++;
        } else if (!strcmp(cmd, "select")) {
            sdr_rx_select(atoi(arg));
            i++;
        } else if (!strcmp(cmd, "ident")) {
            char who[KIWI_IDENT_MAX];
            const esp_err_t e = sdr_ident_save(arg);
            sdr_ident(who, sizeof who);
            shim_say("@IDENT %s \"%s\"", e == ESP_OK ? "saved" : "failed", who);
            i++;
        } else if (!strcmp(cmd, "label")) {
            char l[16] = "";
            const bool ok = sdr_rx_label(atoi(arg), l, sizeof l);
            shim_say("@LABEL %s %s\"%s\"", arg, ok ? "" : "none ", l);
            i++;
        } else if (!strcmp(cmd, "radio")) {
            if (i + 4 >= argc) return 2;
            sdr_rx_tune(atoll(argv[i + 1]), argv[i + 2], atoi(argv[i + 3]), atoi(argv[i + 4]));
            i += 4;
        } else if (!strcmp(cmd, "pitch")) {
            shim_sdr_pitch_reset();
            usleep((useconds_t)(atof(arg) * 1e6));
            uint64_t n = 0;
            const double hz = shim_sdr_pitch(&n);
            shim_say("@PITCH hz=%.1f samples=%llu", hz, (unsigned long long)n);
            i++;
        } else if (!strcmp(cmd, "audio")) {
            size_t level;
            uint32_t dropped, underruns;
            shim_sdr_audio(&level, &dropped, &underruns);
            shim_say("@AUDIO level=%zu dropped=%lu underruns=%lu", level, (unsigned long)dropped,
                     (unsigned long)underruns);
        } else if (!strcmp(cmd, "test")) {
            static char json[1024];
            sdr_cfg_t c;
            if (sdr_get(atoi(arg), &c)) {
                sdr_test(&c, NULL, json, sizeof json, NULL);
                shim_say("@TEST %s", json);
            } else {
                shim_say("@TEST {\"error\":\"no entry %s\"}", arg);
            }
            i++;
        } else if (!strcmp(cmd, "wait")) {
            usleep((useconds_t)(atof(arg) * 1e6));
            i++;
        } else if (!strcmp(cmd, "until")) {
            const char *word = arg;
            const double secs = i + 2 < argc ? atof(argv[i + 2]) : 10;
            bool hit = false;
            for (int t = 0; t < secs * 20 && !hit; t++) {
                sdr_status_t s;
                sdr_rx_status(&s);
                hit = strstr(s.state, word) != NULL;
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
        } else if (!strcmp(cmd, "busy")) {
            s_on_air = atoi(arg) != 0;
            i++;
        } else if (!strcmp(cmd, "off")) {
            shim_say("@OFF");
            _exit(0);                         /* timers and all, mid-flight */
        } else if (!strcmp(cmd, "quit")) {
            kiwi_mark_flush();
            for (int t = 0; t < 100 && !(shim_timers_idle() && kiwi_mark_saved()); t++) usleep(100000);
            shim_say("@QUIT saved=%d", kiwi_mark_saved());
            _exit(0);
        } else {
            fprintf(stderr, "sdr_host: what is \"%s\"?\n", cmd);
            return 2;
        }
    }
    shim_say("@END");
    _exit(0);
}
