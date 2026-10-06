/* The ubersdr firmware's client and the web SDR beside a radio, on the PC,
 * against tools/mock_ubersdr.py -- the knob's own code (uber_client,
 * uber_aux, uber_net, uber_json, sdr_rx on kiwi_proto's session, vfo_tune),
 * ESP-IDF stood in for by stubs.c. One scenario a run, as radio_start() runs once in a firmware's
 * life; run.sh starts a mock for each:
 *
 *   uberhost lan     <port> <kiwi>   an UberSDR on the LAN, in the clear:
 *                                    registered, streaming, tuned, its noise
 *                                    filter, spots, voices, SSTV -- no TLS
 *   uberhost timeup  <port> <kiwi>   a guest's session ended: TIME UP, and
 *                                    LISTEN AGAIN
 *   uberhost countdown <port> <kiwi> a guest's time left, as the receiver
 *                                    counts it: through the dial and a
 *                                    reconnect, to 0:00, then TIME UP, and
 *                                    afresh after LISTEN AGAIN
 *   uberhost idle    <port> <kiwi>   an idle limit's last minute: given back
 *                                    by the knob's use, else the end
 *   uberhost daylimit <port> <kiwi>  the address's allowance for the day,
 *                                    still while the socket is down
 *   uberhost dayhold <port> <kiwi>   ...spent, 0:00 held until the receiver
 *                                    acts on it; dropped then: DAY LIMIT
 *   uberhost restart / restartshut <port> <kiwi>
 *                                    the receiver restarted mid-session, its
 *                                    clock started again: 0:00 here, its
 *                                    audio socket then refused, or closed as
 *                                    it opens -- one try at once, then the
 *                                    backoff
 *   uberhost wrongpw / rightpw / pwonly <port> <kiwi>
 *                                    the bypass password refused, taken, or
 *                                    asked for
 *   uberhost kiwi    <port> <kiwi>   UberSDR's Kiwi input as a web SDR: the
 *                                    app path's ten-digit stamp, its passband,
 *                                    AGC and squelch taken once the audio
 *                                    flows, CW on the carrier
 *   uberhost kiwisdr <port> <kiwi>   a KiwiSDR's: CW on its 500 Hz tone
 *   uberhost kiwicw  <port> <kiwi>   ...and on one its owner set at 600 Hz
 *
 * A list of receivers, the first in use: a second mock on <port> + 2, and
 * nothing on <port> + 4 and + 5 -- connections there refused.
 *   uberhost handover <port> <kiwi>  the one in use's name not found, the next
 *                                    refused: the third plays, and is in use,
 *                                    the last boot's noise filter on it
 *   uberhost fullnext <port> <kiwi>  ...the next answers, but is full: it keeps
 *                                    its turn, not in use until it plays
 *   uberhost chosen  <port> <kiwi>   another chosen on the page meanwhile:
 *                                    the next plays, the choice kept
 *   uberhost allgone <port> <kiwi>   none answering: round after round, the
 *                                    backoff between them growing
 *   uberhost fullstays / timeupstays / wrongpwstays <port> <kiwi>
 *                                    the one in use answers but will not have
 *                                    us -- full, its time up, the password
 *                                    refused: it keeps its turn
 *   uberhost gone    <port> <kiwi>   the one in use gone mid-session: its
 *                                    fair chance, then the next plays, the
 *                                    noise filter with it
 *   uberhost tunnelgone <port> <kiwi> ...gone from behind its tunnel, which
 *                                    answers 502 for it: the same
 *   uberhost proxygone <port> <kiwi> ...or a proxy's 503, a page of HTML: the
 *                                    same -- a full receiver's 503 says
 *                                    "allowed"
 *   uberhost blip    <port> <kiwi>   ...back within it: no other asked
 *   uberhost single  <port> <kiwi>   a single receiver not reached: as ever
 *
 * Each check prints PASS or FAIL; the exit status counts the failures. */
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lwip/sockets.h"
#include "net_prov.h"
#include "nvs.h"
#include "radio.h"
#include "sdr_rx.h"
#include "uber.h"
#include "uber_json.h"
#include "uber_priv.h"
#include "uberhost.h"

static int s_fails;

static void check(bool ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char what[256];
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    if (!ok) s_fails++;
}

static radio_status_t st(void)
{
    static radio_status_t s;
    radio_get_status(&s);
    return s;
}

/* Waits up to `ms` for cond(); its last answer. */
static bool wait_for(bool (*cond)(void), int ms)
{
    for (int t = 0; t < ms; t += 50) {
        if (cond()) return true;
        usleep(50 * 1000);
    }
    return cond();
}

static bool ready(void) { return st().link == RADIO_LINK_READY; }

/* GET `path` from the mock, its JSON into buf. */
static bool mock_get(uint16_t port, const char *path, char *buf, size_t cap)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) {
        if (fd >= 0) close(fd);
        return false;
    }
    char req[128];
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: mock\r\nConnection: close\r\n\r\n", path);
    if (write(fd, req, strlen(req)) < 0) {
        close(fd);
        return false;
    }
    size_t n = 0;
    ssize_t k;
    while (n + 1 < cap && (k = read(fd, buf + n, cap - 1 - n)) > 0) n += (size_t)k;
    close(fd);
    buf[n] = 0;
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) return false;
    memmove(buf, body + 4, strlen(body + 4) + 1);
    return true;
}

static bool mock_state(uint16_t port, char *buf, size_t cap) { return mock_get(port, "/mock/state", buf, cap); }

/* The last element of the mock's array `key` (an object), in `obj`. */
static const char *last_of(const char *json, const char *key)
{
    const char *e = json + strlen(json), *it = jkey(json, e, key), *v, *last = NULL;
    while (it && (v = jnext(&it, e))) last = v;
    return last;
}

static uint16_t s_port, s_kiwi;
static char s_json[1 << 18];

/* ------------------------------------------------------------- the LAN */

static uber_spot_t s_sp[UBER_SPOTS];
static int s_nsp;
static bool spots_in(void) { return (s_nsp = uber_spots(s_sp, UBER_SPOTS, NULL)) >= 2; }
static bool sstv_listed(void) { return uber_sstv_count() == 3; }
static uber_sstv_t s_pic;
static bool picture(void) { return uber_sstv_get(&s_pic, 0); }
static int64_t s_want_f;
static bool tuned(void) { return st().f_server == s_want_f; }
static bool in_lsb(void) { return !strcmp(st().mode, "lsb") && st().f_server == st().f_display; }
/* NR2 on, as the mock on `port` was last asked: on the first mock, on the
 * second. */
static bool nr2_at(uint16_t port)
{
    if (!mock_state(port, s_json, sizeof s_json)) return false;
    const char *d = last_of(s_json, "dsp"), *e = s_json + strlen(s_json);
    char f[8] = "";
    return d && jo_bool(d, e, "enabled") && jo_str(d, e, "filter", f, sizeof f) && !strcmp(f, "nr2");
}
static bool nr2_on(void)   { return nr2_at(s_port); }
static bool nr2_on_2(void) { return nr2_at((uint16_t)(s_port + 2)); }

static void lan(void)
{
    char host[48];
    snprintf(host, sizeof host, "http://127.0.0.1");
    check(radio_start(host, s_port, "", "") == ESP_OK, "radio_start(\"%s\", %u)", host, (unsigned)s_port);
    check(wait_for(ready, 10000), "READY, in the clear (%s)", radio_link_name());
    check(!strcmp(radio_link_name(), "WS"), "the link is WS, not WSS");
    uber_info_t in;
    uber_info(&in);
    check(in.known && !strcmp(in.name, "Mock UberSDR on the LAN"), "its description: %s", in.name);
    check(in.bypassed && in.max_session_s == 0, "a LAN address bypassed: no password, no time limit");
    char why = 'x';
    check(uber_time_left(&why) == -1 && !why, "...and no time left counted");
    check(in.spots && in.voice && in.sstv, "spots, voices and SSTV offered");
    sleep(2);
    radio_status_t s = st();
    uint64_t n, ns;
    double rms, rs;
    uh_audio(&n, &rms, &ns, &rs);
    check(s.echoes >= 80, "Opus frames: %u in 2 s and more", (unsigned)s.echoes);
    check(n >= 36000 && rms > 300, "audio at the jack: %llu samples, RMS %.0f",
          (unsigned long long)n, rms);
    check(s.smeter_dbm < -55 && s.smeter_dbm > -65, "the S-meter from the frames' power: %.1f dBFS",
          (double)s.smeter_dbm);
    check(s.have_snr && s.snr_db > 26 && s.snr_db < 38, "the SNR from power and noise: %.1f dB",
          (double)s.snr_db);
    check(s.f_server == s.f_display && s.f_display == 14175000, "the receiver's status: %lld Hz %s",
          (long long)s.f_server, s.mode);

    s_want_f = radio_tune_by(3, 1, 1000);
    check(wait_for(tuned, 3000), "tuned by the dial: %lld Hz confirmed", (long long)s_want_f);
    radio_set_mode("lsb");
    bool ok = wait_for(in_lsb, 3000);
    check(ok, "the mode: %s", st().mode);
    radio_set_gain(1);
    check(wait_for(nr2_on, 5000), "the noise filter NR2 asked for and confirmed");
    radio_set_mode("usb");

    ok = wait_for(spots_in, 15000);
    check(ok, "spots and voices on 20 m: %d", s_nsp);
    bool dx = false, voice = false;
    for (int i = 0; i < s_nsp; i++) {
        printf("      %c %-8s %lu %s%s\n", s_sp[i].kind, s_sp[i].call, (unsigned long)s_sp[i].hz,
               s_sp[i].mode, s_sp[i].heard ? " heard" : "");
        dx |= s_sp[i].kind == 'D' && !strcmp(s_sp[i].call, "OK1ABC");
        voice |= s_sp[i].heard;
    }
    check(dx && voice, "OK1ABC from the cluster, and a voice heard");

    ok = wait_for(sstv_listed, 8000);
    check(ok, "the SSTV gallery: %d pictures", uber_sstv_count());
    uber_sstv_want(0, 1);
    ok = wait_for(picture, 8000) && !s_pic.failed;
    check(ok, "picture 0 fetched: %s, %s", s_pic.title, s_pic.caption);
    size_t pn;
    int pw, ph;
    uh_png_last(&pn, &pw, &ph);
    check(pn > 1000 && pw == 320 && ph == 256 && s_pic.w == 260 && s_pic.h == 208,
          "its PNG whole: %zu bytes, %dx%d, fitted %ux%u", pn, pw, ph, s_pic.w, s_pic.h);
    uber_sstv_shown(s_pic.seq, true);
    char url[96];
    uber_base_url(url, sizeof url);
    char want[64];
    snprintf(want, sizeof want, "http://127.0.0.1:%u", (unsigned)s_port);
    check(!strcmp(url, want), "the gallery's address for a browser: %s", url);
    check(uh_tls_inits == 0, "no esp-tls connection made: %d", uh_tls_inits);
}

/* ------------------------------------------------------------- limits */

static bool time_up(void)
{
    char t[24] = "", n[24] = "";
    return st().n_choices == 1 && radio_get_choice(0, t, sizeof t, n, sizeof n) && !strcmp(t, "TIME UP");
}

static void timeup(void)
{
    radio_start("http://127.0.0.1", s_port, "", "");
    check(wait_for(ready, 10000), "READY, as a guest");
    uber_info_t in;
    uber_info(&in);
    check(!in.bypassed && in.max_session_s == 6, "a guest, %d s a session", in.max_session_s);
    const bool ok = wait_for(time_up, 25000);
    check(ok, "TIME UP once the receiver ended it (%s)", st().link_why);
    sleep(3);
    check(st().link != RADIO_LINK_READY && time_up(), "...and no session again by itself");
    radio_choose(0);
    check(wait_for(ready, 12000), "LISTEN AGAIN: a new session, READY");
}

static const char *s_why;
static bool says(void) { return !strcmp(st().link_why, s_why); }

/* ---------------------------------------------------------- time left */

static int64_t ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool down(void) { return st().link != RADIO_LINK_READY; }

/* What the mock counts for the knob's session, in s: its limit's "left" and
 * an open socket's "idle_left" by the UUID, the address's "day_left". */
static double mock_left(const char *key)
{
    char uuid[37] = "";
    uber_session_id(uuid, sizeof uuid, NULL);
    if (!mock_state(s_port, s_json, sizeof s_json)) return NAN;
    const char *e = s_json + strlen(s_json), *o = jkey(s_json, e, key);
    return o ? jo_num(o, e, strcmp(key, "day_left") ? uuid : "127.0.0.1", NAN) : NAN;
}

/* The knob's whole seconds, rounded up, and the mock's own count agree. */
static bool agrees(int knob, double mock) { return !isnan(mock) && fabs(knob - mock) <= 1.2; }

/* Why the mock ended the last session it ended: "time", "idle", "day". */
static bool ended_for(const char *why)
{
    if (!mock_state(s_port, s_json, sizeof s_json)) return false;
    const char *k = last_of(s_json, "kicks"), *e = s_json + strlen(s_json);
    char w[8] = "";
    return k && jo_str(k, e, "why", w, sizeof w) && !strcmp(w, why);
}

/* Watched to the end, up to `max` ms: TIME UP. The last count seen before
 * it, and how long before it the count was at 0:00, in ms. */
static bool to_the_end(int max, int *last, int *zero_ms)
{
    int64_t zero = 0;
    *last = -2;
    *zero_ms = -1;
    for (int t = 0; t < max; t += 100) {
        if (time_up()) {
            if (zero) *zero_ms = (int)(ms_now() - zero);
            return true;
        }
        const int l = uber_time_left(NULL);
        if (l >= 0) *last = l;
        if (l == 0 && !zero) zero = ms_now();
        usleep(100 * 1000);
    }
    return false;
}

/* A guest's session, 24 s: counted from its first socket, through the dial
 * and a reconnect under the same UUID, to 0:00; the receiver ends it then;
 * after LISTEN AGAIN, a new UUID counted afresh. The receiver says its
 * session limit as an idle one too (it keeps none): not taken for one. */
static void countdown(void)
{
    char why = 0, u1[37] = "", u2[37] = "";
    radio_start("http://127.0.0.1", s_port, "", "");
    check(wait_for(ready, 10000), "READY, as a guest");
    const int l1 = uber_time_left(&why);
    double m = mock_left("left");
    check(why == 'S' && l1 >= 20 && l1 <= 24 && agrees(l1, m),
          "the session's time left: %d s (the receiver's %.1f), '%c' -- not idle", l1, m, why ? why : '-');
    uber_session_id(u1, sizeof u1, NULL);
    sleep(3);
    const int l2 = uber_time_left(&why);
    check(l2 >= l1 - 4 && l2 <= l1 - 2, "counting down: %d s, 3 s on", l2);
    s_want_f = radio_tune_by(5, 1, 1000);
    check(wait_for(tuned, 3000), "tuned by the dial: %lld Hz", (long long)s_want_f);
    const int l3 = uber_time_left(&why);
    m = mock_left("left");
    check(why == 'S' && l3 <= l2 && agrees(l3, m), "the dial moves nothing: %d s (the receiver's %.1f)", l3, m);
    check(mock_get(s_port, "/mock/drop", s_json, sizeof s_json), "the socket dropped, as a network drops it");
    check(wait_for(down, 3000), "...the link down");
    check(wait_for(ready, 12000), "...and back: READY");
    uber_session_id(u2, sizeof u2, NULL);
    const int l4 = uber_time_left(&why);
    m = mock_left("left");
    check(!strcmp(u1, u2) && l4 < l3 && agrees(l4, m),
          "the same session counting on, not afresh: %d s (the receiver's %.1f)", l4, m);
    int last, zero_ms;
    const bool ok = to_the_end(40000, &last, &zero_ms);
    check(ok, "TIME UP once the receiver ended it (%s)", st().link_why);
    check(last == 0 && zero_ms >= 0 && zero_ms <= 3000,
          "...the count at 0:00 first: %d, %d ms before", last, zero_ms);
    check(ended_for("time"), "...ended by the session's limit");
    check(uber_time_left(&why) == -1, "TIME UP: no time left shown");
    radio_choose(0);
    check(wait_for(ready, 12000), "LISTEN AGAIN: READY");
    const int l5 = uber_time_left(&why);
    uber_session_id(u2, sizeof u2, NULL);
    check(strcmp(u1, u2) && why == 'S' && l5 >= 21 && l5 <= 24,
          "a new session, counted afresh: %d s", l5);
}

static int s_pings;
static bool pinged(void)
{
    return mock_state(s_port, s_json, sizeof s_json) &&
           (int)jo_num(s_json, s_json + strlen(s_json), "pings", 0) > s_pings;
}

/* An idle limit of 14 s, under a session's of 120: always in its last
 * minute, so always the count. The knob used -- a ping, its first 10 s
 * after the socket opened -- or the dial gives it back; left alone, the
 * receiver ends the session. */
static void idle(void)
{
    char why = 0;
    radio_start("http://127.0.0.1", s_port, "", "");
    check(wait_for(ready, 10000), "READY, as a guest");
    const int64_t t0 = ms_now();
    int l = uber_time_left(&why);
    double m = mock_left("idle_left");
    check(why == 'I' && l >= 11 && l <= 14 && agrees(l, m),
          "an idle limit's last minute: %d s (the receiver's %.1f), '%c'", l, m, why ? why : '-');
    while (ms_now() - t0 < 10500) usleep(50 * 1000);
    const int before = uber_time_left(&why);
    s_pings = mock_state(s_port, s_json, sizeof s_json) ? (int)jo_num(s_json, s_json + strlen(s_json), "pings", 0)
                                                         : 0;
    radio_user_activity();
    check(wait_for(pinged, 2000), "the knob used: a ping");
    l = uber_time_left(&why);
    m = mock_left("idle_left");
    check(why == 'I' && before <= 4 && l >= 13 && agrees(l, m),
          "...gives the idle time back: %d s, %d s before (the receiver's %.1f)", l, before, m);
    sleep(3);
    s_want_f = radio_tune_by(-2, 1, 1000);
    check(wait_for(tuned, 3000), "tuned by the dial: %lld Hz", (long long)s_want_f);
    l = uber_time_left(&why);
    m = mock_left("idle_left");
    check(why == 'I' && l >= 13 && agrees(l, m), "...and so does the dial: %d s (the receiver's %.1f)", l, m);
    int last, zero_ms;
    const bool ok = to_the_end(30000, &last, &zero_ms);
    check(ok, "left alone: TIME UP (%s)", st().link_why);
    check(last == 0 && zero_ms >= 0 && zero_ms <= 3000,
          "...the count at 0:00 first: %d, %d ms before", last, zero_ms);
    check(ended_for("idle"), "...ended by the idle limit");
}

/* An allowance of 16 s for the address: counted while the socket is open,
 * still while it is down, then what /connection says is left. Spent, the
 * receiver ends the session, and a new one is refused for the day. */
static void daylimit(void)
{
    char why = 0;
    radio_start("http://127.0.0.1", s_port, "", "");
    check(wait_for(ready, 10000), "READY, as a guest");
    int l = uber_time_left(&why);
    double m = mock_left("day_left");
    check(why == 'D' && l >= 13 && l <= 16 && agrees(l, m),
          "the day's allowance left: %d s (the receiver's %.1f), '%c'", l, m, why ? why : '-');
    sleep(3);
    check(mock_get(s_port, "/mock/drop", s_json, sizeof s_json), "the socket dropped");
    check(wait_for(down, 3000), "...the link down");
    l = uber_time_left(&why);
    usleep(1000 * 1000);
    const int l2 = uber_time_left(&why);
    check(down() && l2 == l && why == 'D', "the day's count still while no socket is open: %d s, %d s", l, l2);
    check(wait_for(ready, 12000), "...and back: READY");
    l = uber_time_left(&why);
    m = mock_left("day_left");
    check(why == 'D' && l <= l2 && agrees(l, m), "what /connection said was left, counted on: %d s (the receiver's %.1f)",
          l, m);
    int last, zero_ms;
    bool ok = to_the_end(30000, &last, &zero_ms);
    check(ok, "TIME UP once the allowance was spent (%s)", st().link_why);
    check(last == 0 && zero_ms >= 0 && zero_ms <= 3000,
          "...the count at 0:00 first: %d, %d ms before", last, zero_ms);
    check(ended_for("day"), "...ended by the day's allowance");
    radio_choose(0);
    s_why = "DAY LIMIT";
    ok = wait_for(says, 8000);
    check(ok, "LISTEN AGAIN: refused for the day (%s)", st().link_why);
    check(uber_time_left(NULL) == -1, "...and no time left shown");
}

/* How many of the requests the mock on `port` was sent began so. */
static int asked(uint16_t port, const char *what)
{
    if (!mock_state(port, s_json, sizeof s_json)) return -1;
    const char *e = s_json + strlen(s_json), *it = jkey(s_json, e, "requests"), *v;
    char r[64];
    int n = 0;
    while (it && (v = jnext(&it, e)))
        if (jstr(v, e, r, sizeof r) && !strncmp(r, what, strlen(what))) n++;
    return n;
}

static int requests(const char *what) { return asked(s_port, what); }

static bool at_zero(void) { return uber_time_left(NULL) == 0; }

/* An allowance of 8 s, spent -- which UberSDR checks every 30 s only: the
 * count holds 0:00 while the receiver plays on. The socket dropped then:
 * one try at once, refused for the day -- DAY LIMIT, the count at 0:00
 * still -- and no other for 30 s. */
static void dayhold(void)
{
    char why = 0;
    radio_start("http://127.0.0.1", s_port, "", "");
    check(wait_for(ready, 10000), "READY, as a guest");
    const int l = uber_time_left(&why);
    check(why == 'D' && l >= 5 && l <= 8, "the day's allowance left: %d s, '%c'", l, why ? why : '-');
    check(wait_for(at_zero, 12000), "0:00");
    sleep(3);
    const int z = uber_time_left(&why);
    check(ready() && z == 0 && why == 'D' && !ended_for("day"),
          "...held there 3 s on, the receiver playing on: it checks the day every 30 s");
    const int c0 = requests("POST /connection");
    check(mock_get(s_port, "/mock/drop", s_json, sizeof s_json), "the socket dropped");
    s_why = "DAY LIMIT";
    const bool ok = wait_for(says, 3000);
    check(ok, "at once: refused for the day (%s)", st().link_why);
    const int z2 = uber_time_left(&why);
    check(z2 == 0 && why == 'D', "...the count at 0:00 still, the day's: %d '%c'", z2, why ? why : '-');
    sleep(5);
    const int n = requests("POST /connection") - c0;
    check(n == 1 && says(), "...and no other try for 30 s: %d /connection in 5 s", n);
}

/* The receiver restarted mid-session: it forgets the session, whose time
 * starts again at the knob's next socket -- the knob's count, its own,
 * runs early. At the knob's 0:00 the receiver streams on; then its audio
 * socket fails, refused -- or, `shut`, closed as soon as it opens: one try
 * at once, then the backoff, as for any failure, never a try every moment.
 * The receiver ends the session at its own time: TIME UP. */
static void restart(bool shut)
{
    char why = 0, u1[37] = "", u2[37] = "";
    radio_start("http://127.0.0.1", s_port, "", "");
    check(wait_for(ready, 10000), "READY, as a guest");
    uber_session_id(u1, sizeof u1, NULL);
    sleep(8);
    check(mock_get(s_port, "/mock/restart", s_json, sizeof s_json), "the receiver restarted, the session forgotten");
    check(wait_for(down, 3000), "...the link down");
    check(wait_for(ready, 12000), "...and back: READY");
    uber_session_id(u2, sizeof u2, NULL);
    const int l = uber_time_left(&why);
    const double m = mock_left("left");
    check(!strcmp(u1, u2) && why == 'S' && !isnan(m) && l + 6 < m,
          "the same session, counted on here: %d s; the receiver's afresh: %.1f s", l, m);
    check(wait_for(at_zero, 15000) && ready(), "0:00 here, and the receiver streams on");
    const int c0 = requests("POST /connection");
    check(mock_get(s_port, shut ? "/mock/refuse?open=1" : "/mock/refuse", s_json, sizeof s_json),
          shut ? "...then its audio socket closes as it opens" : "...then its audio socket is refused");
    sleep(7);
    const int n = requests("POST /connection") - c0;
    check(n >= 2 && n <= 3, "one try at once, then the backoff: %d /connection in 7 s", n);
    check(at_zero(), "...the count held at 0:00");
    int last, zero_ms;
    const bool ok = to_the_end(30000, &last, &zero_ms);
    check(ok && ended_for("time"), "TIME UP at the receiver's own time (%s)", st().link_why);
}

static void password(const char *pass, const char *why)
{
    radio_start("http://127.0.0.1", s_port, "", pass);
    if (why) {
        s_why = why;
        const bool ok = wait_for(says, 8000);
        check(ok, "with password \"%s\": %s (%s)", pass, why, st().link_why);
    } else {
        check(wait_for(ready, 10000), "with the password: READY");
        uber_info_t in;
        uber_info(&in);
        check(in.bypassed, "...bypassed");
        sleep(1);
        check(uber_time_left(NULL) == -1, "...and no time left counted, a guest's limit lifted");
    }
}

/* ------------------------------------------------- a list of receivers */

/* The knob's list, as the configuration page leaves it (net_prov), the
 * first in use; the client started with that one, as app_main starts it. */
static net_radio_t s_list[NET_PROV_RADIOS];
static int s_nlist;

static void listed_as(int i, const char *name, const char *host, uint16_t port, const char *pass)
{
    net_radio_t *r = &s_list[i];
    memset(r, 0, sizeof *r);
    snprintf(r->name, sizeof r->name, "%s", name);
    snprintf(r->host, sizeof r->host, "%s", host);
    r->port = port;
    snprintf(r->pass, sizeof r->pass, "%s", pass);
    if (i >= s_nlist) s_nlist = i + 1;
}

static int64_t s_t0;

static void start_listed(void)
{
    s_t0 = ms_now();
    check(net_prov_radios_save(s_list, s_nlist, 0) == ESP_OK, "%d receivers listed, %s in use", s_nlist,
          s_list[0].name);
    radio_start(s_list[0].host, s_list[0].port, "", s_list[0].pass);
}

/* The noise filter as the last boot left it in NVS, by name; as it is kept
 * for the next. */
static void filter_kept(const char *name)
{
    nvs_handle_t h;
    nvs_open("vfo", NVS_READWRITE, &h);
    nvs_set_str(h, "ubn", name);
    nvs_close(h);
}

static void filter_saved(char *out, size_t cap)
{
    nvs_handle_t h;
    size_t n = cap;
    out[0] = 0;
    nvs_open("vfo", NVS_READONLY, &h);
    nvs_get_str(h, "ubn", out, &n);
    nvs_close(h);
}

/* The face while there is no link, as app_main.c gives it: the warning --
 * link_why, else NO LINK -- and the receiver it is about (uber_receiver);
 * READY once it plays. Each change as it came, from the start. */
typedef struct {
    int64_t at;                         /* ms from the start */
    char    says[16], name[24];
} face_t;
static face_t s_face[400];
static int    s_nface;

static void face_now(void)
{
    const radio_status_t s = st();
    face_t f = { .at = ms_now() - s_t0 };
    snprintf(f.says, sizeof f.says, "%s", s.link == RADIO_LINK_READY ? "READY" : s.link_why[0] ? s.link_why : "NO LINK");
    uber_receiver(f.name, sizeof f.name);
    if (s_nface && !strcmp(s_face[s_nface - 1].says, f.says) && !strcmp(s_face[s_nface - 1].name, f.name)) return;
    if (s_nface < (int)(sizeof s_face / sizeof *s_face)) s_face[s_nface++] = f;
    printf("      %6.1f s  %-14s %s\n", f.at / 1000.0, f.says, f.name);
}

/* The face watched for up to `ms`, or until cond(): its last answer. */
static bool watch(int ms, bool (*cond)(void))
{
    const int64_t until = ms_now() + ms;
    for (;;) {
        face_now();
        if (cond && cond()) return true;
        if (ms_now() >= until) return false;
        usleep(50 * 1000);
    }
}

/* Where the face first said `says` of `name` (NULL: of any), from `from`
 * on: its place in s_face; -1, never. */
static int face_said(int from, const char *says, const char *name)
{
    for (int i = from < 0 ? 0 : from; i < s_nface; i++)
        if (!strcmp(s_face[i].says, says) && (!name || !strcmp(s_face[i].name, name))) return i;
    return -1;
}

static int  s_on = -1;                  /* the receiver READY is wanted on */
static bool ready_on(void)
{
    char n[24];
    return ready() && uber_receiver(n, sizeof n) == s_on;
}

/* The receiver the client is on: its place, and its name. */
static int on_now(char *name)
{
    return uber_receiver(name, 24);
}

/* The one in use's name not found, the next refused -- the third answers:
 * it plays, the spots' task with it, and it is in use from now on. The
 * face names each with its state as it goes. The noise filter the last boot
 * left, NR2, goes with it: neither before it was read. */
static void handover(void)
{
    filter_kept("nr2");
    listed_as(0, "GONE", "http://ubersdr-gone.invalid", 8080, "");
    listed_as(1, "SHUT", "http://127.0.0.1", (uint16_t)(s_port + 4), "");
    listed_as(2, "LAN", "http://127.0.0.1", s_port, "");
    start_listed();
    s_on = 2;
    check(watch(40000, ready_on), "READY on the third, LAN");
    /* What the face shows long enough to be seen -- a try refused at once
     * is CONNECTING for no time at all. */
    const int b = face_said(0, "NOT FOUND", "GONE"), f = face_said(b, "NO ANSWER", "SHUT"),
              h = face_said(f, "READY", "LAN");
    check(b >= 0 && f > b && h > f, "the face named each with its state: GONE NOT FOUND, SHUT NO ANSWER, LAN READY");
    check(!s_face[h - 1].name[0] || !strcmp(s_face[h - 1].name, "LAN"), "...LAN %s before it played",
          s_face[h - 1].says);
    const char *tried = "GONE, ubersdr-gone.invalid:8080: no description (name not found)";
    check(uh_log_count(tried) == 2, "GONE, the one in use, tried twice before another");
    const int64_t gap = uh_log_ms(tried, 1) - uh_log_ms(tried, 0);
    check(gap >= 3500 && gap <= 5500, "...some seconds apart: %.1f s", gap / 1000.0);
    check(uh_log_count("SHUT, 127.0.0.1:") == 1, "SHUT, not in use, tried once, %.1f s after GONE's second",
          (uh_log_ms("SHUT, 127.0.0.1:", 0) - uh_log_ms(tried, 1)) / 1000.0);
    const int64_t said = s_face[h - 1].at - s_face[f].at;
    check(said >= 1500 && said <= 3000, "SHUT's NO ANSWER said a moment, %.1f s, before the next", said / 1000.0);
    check(net_prov_radio_active() == 2 && uh_radio_uses() == 1, "LAN in use from now on, as the list reads back: %d",
          net_prov_radio_active());
    check(uh_logged("LAN lets the knob in: in use from now on, saved"), "...saved as it let the knob in");
    uber_info_t in;
    uber_info(&in);
    check(in.known && !strcmp(in.name, "LAN"), "its description: %s", in.name);
    bool ok = wait_for(nr2_on, 6000);
    check(ok && st().gain == 1, "the last boot's noise filter, NR2, on LAN too: gain %d", st().gain);
    radio_tune_by(2, 1, 1000);
    sleep(4);
    char kept[8];
    filter_saved(kept, sizeof kept);
    check(!strcmp(kept, "nr2"), "...and kept for the next boot, the dial turned: \"%s\"", kept);
    sleep(2);
    uint64_t n, ns;
    double rms, rs;
    uh_audio(&n, &rms, &ns, &rs);
    check(n >= 36000 && rms > 300, "its audio at the jack: %llu samples", (unsigned long long)n);
    ok = wait_for(spots_in, 15000);
    check(ok, "its spots and voices: %d", s_nsp);
    ok = wait_for(sstv_listed, 10000);
    check(ok, "its SSTV gallery: %d pictures", uber_sstv_count());
    char url[96], want[64];
    uber_base_url(url, sizeof url);
    snprintf(want, sizeof want, "http://127.0.0.1:%u", (unsigned)s_port);
    check(!strcmp(url, want), "the gallery's address for a browser, LAN's: %s", url);
}

/* None answering: round after round -- the one in use twice, the others
 * once each -- the backoff between rounds growing as a single receiver's
 * does; nothing saved, no try every moment. */
static void allgone(void)
{
    listed_as(0, "GONE", "http://ubersdr-gone.invalid", 8080, "");
    listed_as(1, "SHUT", "http://127.0.0.1", (uint16_t)(s_port + 4), "");
    listed_as(2, "SHUT2", "http://127.0.0.1", (uint16_t)(s_port + 5), "");
    start_listed();
    watch(43000, NULL);
    check(face_said(0, "READY", NULL) < 0 && !ready(), "none answering: no link");
    check(uh_logged("nor any other in the list: again in 2 s") && uh_logged("nor any other in the list: again in 4 s") &&
          uh_logged("nor any other in the list: again in 8 s"), "round after round, the backoff growing: 2 s, 4, 8");
    const int g = uh_log_count("GONE, ubersdr-gone.invalid:8080: no description"),
              s1 = uh_log_count("SHUT, 127.0.0.1:"), s2 = uh_log_count("SHUT2, 127.0.0.1:");
    check(g >= 7 && g <= 8 && s1 == 3 && s2 == 3, "...each in turn, the one in use twice a round: %d, %d, %d tries in 43 s",
          g, s1, s2);
    const int a = face_said(0, "NOT FOUND", "GONE"), b = face_said(a, "NO ANSWER", "SHUT"),
              c = face_said(b, "NO ANSWER", "SHUT2"), d = face_said(c, "NOT FOUND", "GONE");
    check(a >= 0 && b > a && c > b && d > c, "the face naming each as it goes, round after round");
    check(net_prov_radio_active() == 0 && uh_radio_uses() == 0, "GONE in use still: none saved");
}

/* The one in use answers, but will not have us: that is its word -- no
 * other is asked. `why` as the face says it. */
static void stays(const char *why)
{
    char n[24] = "";
    int at = on_now(n);
    check(at == 0, "on the one in use: %s", n);
    watch(14000, NULL);
    s_why = why;
    at = on_now(n);
    check(says() && at == 0, "...it keeps its turn, 14 s on: %s, %s", st().link_why, n);
    check(asked((uint16_t)(s_port + 2), "GET /api/description") == 0 &&
          asked((uint16_t)(s_port + 2), "POST /connection") == 0, "SPARE never asked");
    check(net_prov_radio_active() == 0 && uh_radio_uses() == 0, "the one in use unchanged");
}

static void fullstays(void)
{
    listed_as(0, "FULL", "http://127.0.0.1", s_port, "");
    listed_as(1, "SPARE", "http://127.0.0.1", (uint16_t)(s_port + 2), "");
    start_listed();
    s_why = "RECEIVER FULL";
    const bool ok = wait_for(says, 10000);
    check(ok, "the one in use answers: %s", st().link_why);
    stays("RECEIVER FULL");
}

static void wrongpwstays(void)
{
    listed_as(0, "LOCKED", "http://127.0.0.1", s_port, "wrong");
    listed_as(1, "SPARE", "http://127.0.0.1", (uint16_t)(s_port + 2), "");
    start_listed();
    s_why = "WRONG PASSWORD";
    const bool ok = wait_for(says, 10000);
    check(ok, "the one in use answers: %s", st().link_why);
    stays("WRONG PASSWORD");
}

static void timeupstays(void)
{
    listed_as(0, "LIMITED", "http://127.0.0.1", s_port, "");
    listed_as(1, "SPARE", "http://127.0.0.1", (uint16_t)(s_port + 2), "");
    start_listed();
    check(wait_for(ready, 10000), "READY on the one in use, a guest's 5 s");
    check(wait_for(time_up, 20000), "TIME UP once it ended the session");
    char n[24] = "";
    watch(8000, NULL);
    int at = on_now(n);
    check(time_up() && !ready() && at == 0, "...it keeps its turn, 8 s on: the question, on %s", n);
    check(asked((uint16_t)(s_port + 2), "GET /api/description") == 0, "SPARE never asked");
    radio_choose(0);
    const bool ok = wait_for(ready, 12000);
    at = on_now(n);
    check(ok && at == 0, "LISTEN AGAIN: READY on %s again", n);
    check(asked((uint16_t)(s_port + 2), "GET /api/description") == 0 && net_prov_radio_active() == 0 &&
          uh_radio_uses() == 0, "...SPARE never asked, the one in use unchanged");
}

/* The one in use gone mid-session: its fair chance -- two tries, some
 * seconds apart -- then the next plays, and is in use. */
static void gone(void)
{
    listed_as(0, "FIRST", "http://127.0.0.1", s_port, "");
    listed_as(1, "SECOND", "http://127.0.0.1", (uint16_t)(s_port + 2), "");
    start_listed();
    check(watch(10000, ready), "READY on FIRST, the one in use");
    radio_set_gain(1);
    check(wait_for(nr2_on, 6000), "its noise filter NR2 on");
    const int from = s_nface;
    check(mock_get(s_port, "/mock/vanish", s_json, sizeof s_json), "FIRST gone mid-session: its port let go");
    s_on = 1;
    check(watch(30000, ready_on), "...SECOND answers: READY on it");
    bool ok = wait_for(nr2_on_2, 6000);
    check(ok && st().gain == 1, "NR2 on SECOND too, which runs it: gain %d", st().gain);
    const int a = face_said(from, "NO ANSWER", "FIRST"), d = face_said(a, "READY", "SECOND");
    check(a >= 0 && d > a, "the face: FIRST NO ANSWER, then SECOND READY");
    check(uh_log_count("/connection: refused") == 2, "FIRST given its fair chance: two tries");
    const int64_t gap = uh_log_ms("/connection: refused", 1) - uh_log_ms("/connection: refused", 0);
    check(gap >= 3500 && gap <= 5500, "...some seconds apart: %.1f s", gap / 1000.0);
    check(net_prov_radio_active() == 1 && uh_radio_uses() == 1, "SECOND in use from now on: %d",
          net_prov_radio_active());
    ok = wait_for(sstv_listed, 10000);
    check(ok && asked((uint16_t)(s_port + 2), "GET /addon/sstv/api/images") >= 1,
          "the spots' task on SECOND too: its gallery, %d pictures", uber_sstv_count());
    ok = wait_for(spots_in, 15000);
    check(ok && asked((uint16_t)(s_port + 2), "GET /api/bands") >= 1, "...its bands, spots and voices: %d", s_nsp);
}

/* ...gone from behind its tunnel, which answers for it -- 502 Bad Gateway,
 * a page of HTML, no answer of an UberSDR's: not reached, the same. */
static void tunnelgone(void)
{
    listed_as(0, "FIRST", "http://127.0.0.1", s_port, "");
    listed_as(1, "SECOND", "http://127.0.0.1", (uint16_t)(s_port + 2), "");
    start_listed();
    check(watch(10000, ready), "READY on FIRST, the one in use");
    sleep(2);
    const int from = s_nface;
    check(mock_get(s_port, "/mock/gateway", s_json, sizeof s_json),
          "FIRST gone from behind its tunnel: 502 Bad Gateway from now on");
    s_on = 1;
    check(watch(30000, ready_on), "...SECOND answers: READY on it");
    check(face_said(from, "NO ANSWER", "FIRST") >= 0, "the face: FIRST NO ANSWER");
    check(uh_log_count("/connection: 502") == 2, "FIRST given its fair chance: two tries, 502 both");
    check(net_prov_radio_active() == 1 && uh_radio_uses() == 1, "SECOND in use from now on: %d",
          net_prov_radio_active());
}

/* ...or from behind a proxy, which answers 503 Service Unavailable for it, a
 * page of HTML: not reached, the same -- a full receiver's 503 says
 * "allowed" (fullstays). */
static void proxygone(void)
{
    listed_as(0, "FIRST", "http://127.0.0.1", s_port, "");
    listed_as(1, "SECOND", "http://127.0.0.1", (uint16_t)(s_port + 2), "");
    start_listed();
    check(watch(10000, ready), "READY on FIRST, the one in use");
    sleep(2);
    const int from = s_nface;
    check(mock_get(s_port, "/mock/gateway?status=503", s_json, sizeof s_json),
          "FIRST gone from behind its proxy: 503 Service Unavailable from now on");
    s_on = 1;
    check(watch(30000, ready_on), "...SECOND answers: READY on it");
    check(face_said(from, "NO ANSWER", "FIRST") >= 0 && face_said(from, "RECEIVER FULL", NULL) < 0,
          "the face: FIRST NO ANSWER, never RECEIVER FULL");
    check(uh_log_count("/connection: 503") == 2, "FIRST given its fair chance: two tries, 503 both");
    check(net_prov_radio_active() == 1 && uh_radio_uses() == 1, "SECOND in use from now on: %d",
          net_prov_radio_active());
}

/* The one in use's name not found; the next answers, but is full: a
 * stand-in, never chosen, so passed by -- the one in use has its turn
 * again, round after round, and is the one in use still. Room again, the
 * stand-in plays at its next turn: in use from then on. */
static void fullnext(void)
{
    listed_as(0, "GONE", "http://ubersdr-gone.invalid", 8080, "");
    listed_as(1, "FULL", "http://127.0.0.1", s_port, "");
    start_listed();
    s_why = "RECEIVER FULL";
    const bool full = watch(20000, says);
    check(full, "FULL answers, full: %s", st().link_why);
    watch(15000, NULL);
    check(uh_logged("FULL: RECEIVER FULL -- a stand-in, so on to the next"), "...a stand-in: passed by");
    const int g = uh_log_count("GONE, ubersdr-gone.invalid:8080: no description");
    check(g > 2, "GONE, the one in use, has its turn again: %d tries", g);
    check(net_prov_radio_active() == 0 && uh_radio_uses() == 0,
          "GONE in use still: one that never played is not saved");
    check(mock_get(s_port, "/mock/full?on=0", s_json, sizeof s_json), "room on FULL again");
    s_on = 1;
    check(watch(60000, ready_on), "...READY on FULL at its next turn");
    check(net_prov_radio_active() == 1 && uh_radio_uses() == 1, "FULL in use from now on: %d",
          net_prov_radio_active());
}

/* ...and with a third in the list: the full stand-in passed by, the third
 * plays, and is the one in use from now on. */
static void standin(void)
{
    listed_as(0, "GONE", "http://ubersdr-gone.invalid", 8080, "");
    listed_as(1, "FULL", "http://127.0.0.1", s_port, "");
    listed_as(2, "SPARE", "http://127.0.0.1", (uint16_t)(s_port + 2), "");
    start_listed();
    s_on = 2;
    check(watch(30000, ready_on), "READY on SPARE, the full stand-in passed by");
    check(uh_logged("FULL: RECEIVER FULL -- a stand-in, so on to the next"), "...FULL said it was full first");
    check(net_prov_radio_active() == 2 && uh_radio_uses() == 1, "SPARE in use from now on: %d",
          net_prov_radio_active());
}

/* A stand-in that refuses the knob's password: passed by too. */
static void pwnext(void)
{
    listed_as(0, "GONE", "http://ubersdr-gone.invalid", 8080, "");
    listed_as(1, "LOCKED", "http://127.0.0.1", s_port, "wrong");
    listed_as(2, "SPARE", "http://127.0.0.1", (uint16_t)(s_port + 2), "");
    start_listed();
    s_on = 2;
    check(watch(30000, ready_on), "READY on SPARE, the locked stand-in passed by");
    check(uh_logged("LOCKED: WRONG PASSWORD -- a stand-in, so on to the next"),
          "...LOCKED refused the password first");
    check(net_prov_radio_active() == 2 && uh_radio_uses() == 1, "SPARE in use from now on: %d",
          net_prov_radio_active());
}

/* Another chosen as the one in use on the configuration page -- for the
 * next boot -- while the one in use has its fair chance: the next plays,
 * and the page's choice stands. */
static void chosen(void)
{
    listed_as(0, "GONE", "http://ubersdr-gone.invalid", 8080, "");
    listed_as(1, "LAN", "http://127.0.0.1", s_port, "");
    listed_as(2, "OTHER", "http://127.0.0.1", (uint16_t)(s_port + 4), "");
    start_listed();
    watch(1000, NULL);
    check(net_prov_radios_save(s_list, s_nlist, 2) == ESP_OK, "OTHER chosen on the page, 1 s on");
    s_on = 1;
    check(watch(30000, ready_on), "READY on LAN, the next in turn");
    check(uh_logged("LAN lets the knob in: not saved as the one in use -- another chosen meanwhile"),
          "...not saved as the one in use");
    check(net_prov_radio_active() == 2 && uh_radio_uses() == 0, "OTHER, the page's choice, in use still: %d",
          net_prov_radio_active());
}

/* ...back within its fair chance: a blip. No other asked. */
static void blip(void)
{
    listed_as(0, "FIRST", "http://127.0.0.1", s_port, "");
    listed_as(1, "SECOND", "http://127.0.0.1", (uint16_t)(s_port + 2), "");
    start_listed();
    check(watch(10000, ready), "READY on FIRST, the one in use");
    sleep(2);
    const int from = s_nface;
    check(mock_get(s_port, "/mock/vanish?for=4", s_json, sizeof s_json), "FIRST away 4 s: a blip");
    check(watch(3000, down), "...the link down");
    char n[24] = "";
    const bool ok = watch(20000, ready);
    const int at = on_now(n);
    check(ok && at == 0, "...back on FIRST: READY on %s", n);
    check(face_said(from, "NO ANSWER", "FIRST") >= 0, "the face said so while it was away: FIRST NO ANSWER");
    check(uh_log_count("/connection: refused") == 1, "one try not answered, then it was there");
    check(asked((uint16_t)(s_port + 2), "GET /api/description") == 0, "SECOND never asked");
    check(net_prov_radio_active() == 0 && uh_radio_uses() == 0, "FIRST in use still");
}

/* A single receiver not reached: as ever -- the backoff growing, no name
 * on the face, nothing handed over. */
static void single(void)
{
    listed_as(0, "ALONE", "http://127.0.0.1", (uint16_t)(s_port + 4), "");
    start_listed();
    watch(16000, NULL);
    char n[24] = "x";
    const int at = uber_receiver(n, sizeof n);
    check(at == -1 && !n[0], "a single receiver: none named");
    s_why = "NO ANSWER";
    check(says(), "NO ANSWER, as ever (%s)", st().link_why);
    check(face_said(0, "CONNECTING", NULL) < 0, "...NO LINK while it tries, as ever");
    check(uh_logged("no description (refused); again in 2 s") && uh_logged("no description (refused); again in 4 s") &&
          uh_logged("no description (refused); again in 8 s"), "again and again, the backoff growing: 2 s, 4, 8");
    check(!uh_logged("not reached") && uh_radio_uses() == 0, "nothing handed over, nothing saved");
}

/* ------------------------------------------------------- a web SDR */

static bool streaming(void)
{
    sdr_status_t s;
    sdr_rx_status(&s);
    return s.streaming;
}

static int64_t s_mod_f;
static int32_t s_mod_lo, s_mod_hi;
static const char *s_mod_mode;

/* The mock's channel, as the last SET mod left it, is what was asked for. */
static bool channel(void)
{
    if (!mock_state(s_kiwi, s_json, sizeof s_json)) return false;
    const char *m = last_of(s_json, "mods"), *e = s_json + strlen(s_json);
    char mode[8] = "";
    return m && jo_bool(m, e, "edges") && (int64_t)jo_num(m, e, "freq", 0) == s_mod_f &&
           (int32_t)jo_num(m, e, "lo", 0) == s_mod_lo && (int32_t)jo_num(m, e, "hi", 0) == s_mod_hi &&
           jo_str(m, e, "mode", mode, sizeof mode) && !strcmp(mode, s_mod_mode);
}

/* The mock's channel took an AGC and a squelch: UberSDR's input lets them go
 * by while it has none. */
static bool agc_taken(void)
{
    return mock_state(s_kiwi, s_json, sizeof s_json) && last_of(s_json, "agc") && last_of(s_json, "squelch");
}

static void expect(int64_t f, const char *mode, int32_t lo, int32_t hi, const char *what)
{
    s_mod_f = f;
    s_mod_mode = mode;
    s_mod_lo = lo;
    s_mod_hi = hi;
    const bool ok = wait_for(channel, 3000);
    const char *m = last_of(s_json, "mods"), *e = s_json + strlen(s_json);
    char cmd[96] = "";
    if (m) jo_str(m, e, "cmd", cmd, sizeof cmd);
    check(ok, "%s: %lld Hz %s %ld..%ld (%s)", what, (long long)f, mode, (long)lo, (long)hi, cmd);
}

/* `ctr`: where the receiver centres CW -- on the carrier on UberSDR's input,
 * a KiwiSDR's 500 Hz tone unless its owner set another. The session is
 * components/kiwi_proto's, the kiwi firmware's ears' too. */
static void kiwi(int32_t ctr)
{
    check(sdr_rx_init() == ESP_OK, "sdr_rx_init");
    sdr_cfg_t c = { .name = "UberSDR", .host = "127.0.0.1", .port = s_kiwi };
    check(sdr_save(&c, 1, -1) == ESP_OK && sdr_count() == 1, "the receiver saved: 127.0.0.1:%u", (unsigned)s_kiwi);
    sdr_rx_tune(14074000, "usb", 300, 2700);
    sdr_rx_select(0);
    check(wait_for(streaming, 8000), "streaming from the Kiwi input");
    sleep(1);
    sdr_status_t s;
    sdr_rx_status(&s);
    uint64_t n, ns;
    double rms, rs;
    uh_audio(&n, &rms, &ns, &rs);
    check(ns >= 24000 && rs > 1000, "the right ear: %llu samples, RMS %.0f", (unsigned long long)ns, rs);
    check(s.smeter_dbm > -74 && s.smeter_dbm < -72, "its S-meter: %.1f dBm", (double)s.smeter_dbm);
    /* The app path, /<ts>/SND, as kiwirecorder logs in: the Kiwi input
     * takes it only with ten digits in the stamp. */
    if (mock_state(s_kiwi, s_json, sizeof s_json)) {
        const char *e = s_json + strlen(s_json), *it = jkey(s_json, e, "paths"), *v;
        char path[64] = "";
        if (it && (v = jnext(&it, e))) jstr(v, e, path, sizeof path);
        check(path[0] == '/' && strspn(path + 1, "0123456789") == 10 && !strcmp(path + 11, "/SND"),
              "the app path, a ten-digit stamp: %s", path);
    }
    /* UberSDR's Kiwi input makes the channel from the first SET mod, with a
     * passband of its own, and takes the one asked for only from the next:
     * the knob's, once the audio flowed -- nothing else need send one soon. */
    check(uh_logged_after("streaming from", "SET mod="), "the tune again once the audio flowed");
    /* ...and the AGC and the squelch, which went before it had a channel. */
    check(wait_for(agc_taken, 3000), "the AGC and the squelch on its channel");
    expect(14074000, "usb", 300, 2700, "USB, its passband taken");
    sdr_rx_tune(7074000, "lsb", -2700, -300);
    expect(7074000, "lsb", -2700, -300, "LSB");
    sdr_rx_tune(7030000, "cw", -250, 250);
    char what[64];
    snprintf(what, sizeof what, "CW: the carrier %ld Hz below the dial", (long)ctr);
    expect(7030000 - ctr, "cwu", ctr - 250, ctr + 250, what);
    if (ctr == 500) {
        check(!uh_logged("centres CW"), "a KiwiSDR's 500 Hz kept");
    } else {
        char said[40];
        snprintf(said, sizeof said, "centres CW on %ld Hz", (long)ctr);
        check(uh_logged(said), "its CW centre read from load_cfg: %ld Hz", (long)ctr);
    }
    sdr_rx_tune(7030000, "cw", -150, 150);
    expect(7030000 - ctr, "cwu", ctr - 150, ctr + 150, "a narrower CW filter");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 4) {
        fprintf(stderr, "usage: %s lan|timeup|countdown|idle|daylimit|dayhold|restart|restartshut|wrongpw|rightpw|"
                        "pwonly|kiwi|kiwisdr|kiwicw|handover|fullnext|chosen|allgone|fullstays|timeupstays|wrongpwstays|"
                        "gone|tunnelgone|proxygone|blip|single <port> <kiwi-port>\n", argv[0]);
        return 2;
    }
    s_port = (uint16_t)atoi(argv[2]);
    s_kiwi = (uint16_t)atoi(argv[3]);
    uh_verbose = getenv("UBERHOST_VERBOSE") != NULL;
    /* lwIP's send on a socket the other end closed fails; the PC's would
     * end the run. */
    signal(SIGPIPE, SIG_IGN);
    const char *t = argv[1];
    if (!strcmp(t, "lan")) lan();
    else if (!strcmp(t, "timeup")) timeup();
    else if (!strcmp(t, "countdown")) countdown();
    else if (!strcmp(t, "idle")) idle();
    else if (!strcmp(t, "daylimit")) daylimit();
    else if (!strcmp(t, "dayhold")) dayhold();
    else if (!strcmp(t, "restart")) restart(false);
    else if (!strcmp(t, "restartshut")) restart(true);
    else if (!strcmp(t, "wrongpw")) password("wrong", "WRONG PASSWORD");
    else if (!strcmp(t, "rightpw")) password("secret", NULL);
    else if (!strcmp(t, "pwonly")) password("", "PASSWORD?");
    else if (!strcmp(t, "kiwi")) kiwi(0);
    else if (!strcmp(t, "kiwisdr")) kiwi(500);
    else if (!strcmp(t, "kiwicw")) kiwi(600);
    else if (!strcmp(t, "handover")) handover();
    else if (!strcmp(t, "fullnext")) fullnext();
    else if (!strcmp(t, "standin")) standin();
    else if (!strcmp(t, "pwnext")) pwnext();
    else if (!strcmp(t, "chosen")) chosen();
    else if (!strcmp(t, "allgone")) allgone();
    else if (!strcmp(t, "fullstays")) fullstays();
    else if (!strcmp(t, "timeupstays")) timeupstays();
    else if (!strcmp(t, "wrongpwstays")) wrongpwstays();
    else if (!strcmp(t, "gone")) gone();
    else if (!strcmp(t, "tunnelgone")) tunnelgone();
    else if (!strcmp(t, "proxygone")) proxygone();
    else if (!strcmp(t, "blip")) blip();
    else if (!strcmp(t, "single")) single();
    else {
        fprintf(stderr, "no scenario %s\n", t);
        return 2;
    }
    printf("%s: %d failed\n", t, s_fails);
    fflush(stdout);
    _exit(s_fails);                  /* the client's tasks run on: no clean-up */
}
