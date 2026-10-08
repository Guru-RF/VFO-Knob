/* The openwebrx firmware's receiver -- components/owrx_client on components/
 * owrx_proto's session and the web SDRs' link, as the firmware builds them --
 * run on the PC against tools/mock_owrx.py. One run of this program is one
 * boot of the knob; its NVS is a file that outlives the run, so a run after a
 * run is a restart. The receivers' list is net_prov's on the knob: here a
 * small one of its own, kept in that same file. test/host/owrx_client.py
 * drives it and asks the mock what it saw.
 *
 *   owrx_host NVSFILE CMD...
 *
 *   save [NAME=]URL ...        the page's Save: the list replaced, the one in
 *                              use kept where it is still in it (else the first)
 *   list                       @LIST N "host" active
 *   use N                      the operator's act: the dial's, the page's
 *                              (owrx_rx_use(N, true)): @USE N ok|refused
 *   bands                      @BANDS n, then @B i "name" "sdr" lo hi
 *   band I                     band I asked for (owrx_band_choose): @BAND I ok|refused
 *   tune HZ | mode M           the dial: radio_goto_freq, radio_set_mode
 *   turn N STEP                N detents at STEP Hz (radio_tune_by): @TURN f=..
 *   filter LO HI | squelch P   the filter's passband, the squelch (0-100 %)
 *   pitch S                    the audio's pitch over S s: @PITCH hz=.. samples=..
 *   audio                      the ring: @AUDIO level=.. dropped=.. underruns=..
 *   nvs                        flash written so far: @NVS writes=..
 *   wait S                     seconds
 *   until WORD S               until the state says WORD, S seconds at most
 *   untilrx N S                until receiver N plays, S seconds at most
 *   state                      @STATE link=.. rx=.. why=".." state=".." ...
 *   off                        power off now: what is not in flash is lost
 *   quit                       flash what waits, then end */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "audio_out.h"
#include "net_prov.h"
#include "nvs.h"
#include "owrx.h"
#include "radio.h"
#include "shim.h"

static const char *LINK[] = { "DOWN", "CONNECTING", "GREETING", "READY", "DEGRADED" };

/* ------------------------------------------------- net_prov's radios */

static pthread_mutex_t s_lm = PTHREAD_MUTEX_INITIALIZER;
static net_radio_t     s_list[NET_PROV_RADIOS];
static int             s_n, s_sel;
static uint32_t        s_gen;

static void list_write(void)
{
    char blob[NET_PROV_RADIOS * 200 + 1] = "";
    size_t o = 0;
    for (int i = 0; i < s_n; i++)
        o += (size_t)snprintf(blob + o, sizeof blob - o, "%s\t%s\n", s_list[i].name, s_list[i].host);
    nvs_handle_t h;
    if (nvs_open("vfo", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "owlist", blob);
    nvs_set_i8(h, "owsel", (int8_t)s_sel);
    nvs_commit(h);
    nvs_close(h);
}

static void list_load(void)
{
    char blob[NET_PROV_RADIOS * 200 + 1] = "";
    size_t n = sizeof blob;
    int8_t sel = 0;
    nvs_handle_t h;
    if (nvs_open("vfo", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "owlist", blob, &n) != ESP_OK) blob[0] = 0;
        nvs_get_i8(h, "owsel", &sel);
        nvs_close(h);
    }
    s_n = 0;
    for (char *line = blob, *nl; *line && s_n < NET_PROV_RADIOS; line = nl + 1) {
        nl = strchr(line, '\n');
        if (!nl) break;
        *nl = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        net_radio_t *r = &s_list[s_n++];
        memset(r, 0, sizeof *r);
        snprintf(r->name, sizeof r->name, "%.23s", line);
        snprintf(r->host, sizeof r->host, "%.63s", tab + 1);
    }
    if (!s_n) {                                 /* as net_prov: the one entry a new knob has */
        memset(&s_list[0], 0, sizeof s_list[0]);
        s_n = 1;
    }
    s_sel = sel >= 0 && sel < s_n ? sel : 0;
}

int net_prov_radio_count(void) { return s_n; }
int net_prov_radio_active(void) { return s_sel; }
uint32_t net_prov_radios_gen(void) { return s_gen; }

bool net_prov_radio_get(int i, net_radio_t *out)
{
    pthread_mutex_lock(&s_lm);
    const bool ok = i >= 0 && i < s_n && out;
    if (ok) *out = s_list[i];
    pthread_mutex_unlock(&s_lm);
    return ok;
}

esp_err_t net_prov_radio_activate(int i)
{
    pthread_mutex_lock(&s_lm);
    if (i < 0 || i >= s_n) {
        pthread_mutex_unlock(&s_lm);
        return ESP_ERR_INVALID_ARG;
    }
    s_sel = i;
    list_write();
    pthread_mutex_unlock(&s_lm);
    shim_say("@ACTIVATED %d", i);
    return ESP_OK;
}

esp_err_t net_prov_radios_save(const net_radio_t *list, int n, int active)
{
    if (!list || n < 1 || n > NET_PROV_RADIOS || active < 0 || active >= n) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lm);
    memcpy(s_list, list, (size_t)n * sizeof *list);
    s_n = n;
    s_sel = active;
    s_gen++;
    list_write();
    pthread_mutex_unlock(&s_lm);
    return ESP_OK;
}

/* ------------------------------------------------- the harness */

static void say_state(const char *tag)
{
    static radio_status_t st;
    static owrx_info_t oi;
    radio_get_status(&st);
    owrx_info(&oi);
    shim_say("@%s link=%s rx=%d why=\"%s\" state=\"%s\" server=\"%s\" line2=\"%s\" line3=\"%s\" f=%lld mode=%s "
             "lo=%ld hi=%ld fmin=%lld fmax=%lld db=%.1f users=%d band=\"%s\" sel=%d wait=%d plus=%d "
             "version=\"%s\" sq=%u sqdb=%d mlo=%.1f mhi=%.1f audible=%d model=\"%s\" connects=%lu closes=%lu "
             "note=\"%s\" samples=%llu t=%.3f",
             tag, LINK[st.link], owrx_rx_active(), st.link_why, oi.state, st.server, oi.line2, oi.line3,
             (long long)st.f_display, st.mode, (long)st.filt_lo, (long)st.filt_hi, (long long)st.f_min,
             (long long)st.f_max, (double)st.smeter_dbm, oi.users, oi.band, oi.band_sel, oi.band_wait_s, oi.plus,
             oi.version, (unsigned)st.squelch_pct, oi.sq_db, (double)oi.meter_lo, (double)oi.meter_hi,
             owrx_audible(), st.model, (unsigned long)st.connects, (unsigned long)st.closes, st.note,
             (unsigned long long)shim_samples, shim_now());
}

/* webcfg.c's radios_post_h on owrx, without the form: the list replaced, the
 * one in use kept where its address still is in it. */
static void page_save(int argc, char **argv, int *i)
{
    static net_radio_t list[NET_PROV_RADIOS];
    net_radio_t cur;
    const bool have = net_prov_radio_get(net_prov_radio_active(), &cur);
    int k = 0, use = 0;
    while (*i + 1 < argc && k < NET_PROV_RADIOS && strchr(argv[*i + 1], ':')) {
        const char *a = argv[++*i];
        net_radio_t *r = &list[k];
        memset(r, 0, sizeof *r);
        const char *eq = strchr(a, '=');
        if (eq && eq < strchr(a, ':')) {
            snprintf(r->name, sizeof r->name, "%.*s", (int)(eq - a), a);
            a = eq + 1;
        }
        snprintf(r->host, sizeof r->host, "%s", a);
        if (have && !strcmp(cur.host, r->host)) use = k;
        k++;
    }
    if (k) net_prov_radios_save(list, k, use);
    shim_say("@SAVED %d", k);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: owrx_host NVSFILE CMD...\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    setenv("SHIM_NVS", argv[1], 1);
    list_load();
    radio_start("OWRX", 8073, "", "");

    for (int i = 2; i < argc; i++) {
        const char *cmd = argv[i];
        const char *arg = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(cmd, "save")) {
            page_save(argc, argv, &i);
        } else if (!strcmp(cmd, "list")) {
            for (int j = 0; j < net_prov_radio_count(); j++) {
                net_radio_t r;
                if (net_prov_radio_get(j, &r))
                    shim_say("@LIST %d \"%s\" %d", j, r.host, j == net_prov_radio_active());
            }
        } else if (!strcmp(cmd, "use")) {
            const int n = atoi(arg);
            shim_say("@USE %d %s", n, owrx_rx_use(n, true) ? "ok" : "refused");
            i++;
        } else if (!strcmp(cmd, "bands")) {
            static owrx_band_info_t b[24];
            const int n = owrx_bands(b, 24);
            shim_say("@BANDS %d", n);
            for (int j = 0; j < n; j++)
                shim_say("@B %d \"%s\" \"%s\" %lld %lld", j, b[j].name, b[j].sdr, (long long)b[j].lo,
                         (long long)b[j].hi);
        } else if (!strcmp(cmd, "band")) {
            const int n = atoi(arg);
            shim_say("@BAND %d %s", n, owrx_band_choose(n) ? "ok" : "refused");
            i++;
        } else if (!strcmp(cmd, "tune")) {
            radio_goto_freq(atoll(arg));
            i++;
        } else if (!strcmp(cmd, "mode")) {
            radio_set_mode(arg);
            i++;
        } else if (!strcmp(cmd, "turn")) {
            const int n = atoi(arg);
            const int step = i + 2 < argc ? atoi(argv[i + 2]) : 1000;
            const int64_t f = radio_tune_by(n, 1, step);
            shim_say("@TURN f=%lld", (long long)f);
            i += 2;
        } else if (!strcmp(cmd, "filter")) {
            radio_set_filter(atoi(arg), i + 2 < argc ? atoi(argv[i + 2]) : 0);
            i += 2;
        } else if (!strcmp(cmd, "squelch")) {
            radio_set_squelch((uint8_t)atoi(arg));
            i++;
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
        } else if (!strcmp(cmd, "until") || !strcmp(cmd, "untilrx")) {
            const bool rx = cmd[5] == 'r';
            const char *word = arg;
            const double secs = i + 2 < argc ? atof(argv[i + 2]) : 10;
            bool hit = false;
            for (int t = 0; t < secs * 20 && !hit; t++) {
                static owrx_info_t oi;
                owrx_info(&oi);
                hit = rx ? owrx_rx_active() == atoi(word) && radio_is_ready() : strstr(oi.state, word) != NULL;
                if (!hit) usleep(50000);
            }
            if (!hit) shim_say("@TIMEOUT waiting for %s\"%s\"", rx ? "receiver " : "", word);
            say_state("STATE");
            i += 2;
        } else if (!strcmp(cmd, "state")) {
            say_state("STATE");
        } else if (!strcmp(cmd, "off")) {
            shim_say("@OFF");
            _exit(0);                         /* timers and all, mid-flight */
        } else if (!strcmp(cmd, "quit")) {
            /* A save may wait 30 s for a quiet moment while it plays. */
            for (int t = 0; t < 400 && !shim_timers_idle(); t++) usleep(100000);
            shim_say("@QUIT");
            _exit(0);
        } else {
            fprintf(stderr, "owrx_host: what is \"%s\"?\n", cmd);
            return 2;
        }
    }
    shim_say("@END");
    _exit(0);
}
