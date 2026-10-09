/* OpenWebRX's protocol (components/owrx_proto): what the knob says and what
 * it makes of what a receiver says -- messages as the servers' json.dumps
 * writes them, in pieces broken at every byte, status.json and its join to
 * the bands, the audio -- and the receivers' own rules.
 *
 *   test_owrx_proto                        the checks below
 *   test_owrx_proto STREAM EXPECT          ...and the decoder against
 *                                          tools/owrx_adpcm.py's vectors:
 *                                          csdr's encoder's stream, and the
 *                                          samples it must give back */
#include "tiny.h"
#include "owrx_proto.h"

#include <limits.h>
#include <math.h>

#define PI 3.14159265358979323846

/* -------------------------------------------------------------- helpers */

static uint32_t rng = 12345u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* One text frame, in pieces `piece` bytes long (0: whole). */
static uint32_t text(owrx_said_t *s, const char *m, size_t piece)
{
    const size_t n = strlen(m);
    owrx_text_begin(s);
    if (!piece) piece = n ? n : 1;
    for (size_t i = 0; i < n; i += piece)
        owrx_text_feed(s, (const uint8_t *)m + i, n - i < piece ? n - i : piece);
    return owrx_text_end(s);
}

/* What a session holds, as text: two parses that agree give the same. */
static void summary(const owrx_said_t *s, char *out, size_t cap)
{
    int o = snprintf(out, cap, "%d|%s|%lld|%lld|%d|%s|%s|%s|%d|%.2f|%.2f|%d|%d|%d|%d|%d|%d|%.6g|%d|%d|%s|%s|%d|%s|%u|%u",
                     s->hello, s->version, (long long)s->center, (long long)s->start, (int)s->rate,
                     s->start_mod, s->sdr_id, s->profile_id, s->sq_init, s->wf_min, s->wf_max,
                     s->audio_raw, s->fft_raw, (int)s->fft_size, (int)s->max_clients,
                     (int)s->tuning_step, s->plus, s->meter, s->have_meter, s->clients, s->name,
                     s->log, (int)s->end, s->reason, s->n_bands, s->bands_seen);
    for (int i = 0; i < s->n_bands && o > 0 && (size_t)o < cap; i++)
        o += snprintf(out + o, cap - (size_t)o, "|%s=%s/%08x", s->bands[i].id, s->bands[i].name,
                      (unsigned)s->bands[i].name_h);
}

/* The same frame, whole and in pieces of every size: the same session. */
static void every_split(const char *m)
{
    static owrx_said_t a, b;
    static char sa[8192], sb[8192];
    owrx_said_init(&a);
    const uint32_t ev = text(&a, m, 0);
    summary(&a, sa, sizeof sa);
    const size_t n = strlen(m);
    for (size_t piece = 1; piece <= n; piece += piece < 40 ? 1 : 97) {
        owrx_said_init(&b);
        const uint32_t evb = text(&b, m, piece);
        summary(&b, sb, sizeof sb);
        CHECK_EQ(evb, ev);
        CHECK_STR(sb, sa);
    }
}

/* -------------------------------------------------------------- fixtures */

/* As the servers write them: json.dumps' ", " and ": ", non-ASCII as \u. */
static const char HELLO_P[] = "CLIENT DE SERVER server=openwebrx version=v1.2.126";
static const char HELLO_U[] = "CLIENT DE SERVER server=openwebrx version=v1.2.2";

static const char DETAILS[] =
    "{\"type\": \"receiver_details\", \"value\": {\"receiver_name\": \"ON4PRA Br\\u00fcgge \\ud83d\\udce1\", "
    "\"receiver_location\": \"Brugge, Belgium\", \"receiver_asl\": 12, \"receiver_gps\": {\"lat\": 51.2, "
    "\"lon\": 3.22}, \"photo_title\": \"Panorama\", \"photo_desc\": \"<b>\\\"quoted\\\"</b>\\n\", "
    "\"locator\": \"JO11um\", \"receiver_help\": \"\", \"usage_policy_url\": \"policy\", "
    "\"session_timeout\": 0, \"keep_files\": 20}}";

static const char CONFIG_GLOBAL_U[] =
    "{\"type\": \"config\", \"value\": {\"waterfall_colors\": [0, 2236962, 4473924, 16777215], "
    "\"waterfall_levels\": {\"min\": -88, \"max\": -20}, \"waterfall_auto_level_default_mode\": false, "
    "\"waterfall_auto_levels\": {\"min\": 3, \"max\": 10}, \"waterfall_auto_min_range\": 50, "
    "\"fft_size\": 4096, \"audio_compression\": \"adpcm\", \"fft_compression\": \"adpcm\", "
    "\"max_clients\": 20, \"tuning_precision\": 2, \"squelch_auto_margin\": 10, "
    "\"aircraft_tracking_service\": \"flightaware\"}}";

static const char CONFIG_PROFILE_U[] =
    "{\"type\": \"config\", \"value\": {\"center_freq\": 14100000, \"samp_rate\": 2400000, "
    "\"start_freq\": 14070000, \"start_mod\": \"usb\", \"initial_squelch_level\": -150, "
    "\"profile_id\": \"b6ba4c2e-2f6e-4d5a-9a7c-3b1f2a9e8d01\", "
    "\"sdr_id\": \"9f3c1d2e-0a1b-4c5d-8e7f-6a5b4c3d2e1f\", \"start_offset_freq\": -30000}}";

static const char CONFIG_PROFILE_P[] =
    "{\"type\": \"config\", \"value\": {\"center_freq\": 7100000, \"samp_rate\": 500000, "
    "\"start_freq\": 7074000, \"start_mod\": \"ft8\", \"initial_squelch_level\": -60.0, "
    "\"profile_id\": \"40m\", \"sdr_id\": \"rspdx\", \"start_offset_freq\": -26000, "
    "\"tuning_step\": \"500\", \"allow_center_freq_changes\": false, \"ui_theme\": \"default\", "
    "\"waterfall_levels\": {\"min\": -95.5, \"max\": -35.0}, \"audio_compression\": \"none\"}}";

/* Moving to another SDR: its layer goes first, all nulls. */
static const char CONFIG_NULLS[] =
    "{\"type\": \"config\", \"value\": {\"center_freq\": null, \"samp_rate\": null, \"start_freq\": null, "
    "\"start_mod\": null, \"initial_squelch_level\": null, \"profile_id\": null, \"sdr_id\": null}}";

static const char PROFILES[] =
    "{\"type\": \"profiles\", \"value\": [{\"id\": \"rspdx|40m\", \"name\": \"RSPdx 40m\"}, "
    "{\"id\": \"rspdx|20m\", \"name\": \"RSPdx 20m\"}, "
    "{\"id\": \"9f3c1d2e-0a1b-4c5d-8e7f-6a5b4c3d2e1f|b6ba4c2e-2f6e-4d5a-9a7c-3b1f2a9e8d01\", "
    "\"name\": \"RTL-SDR v3 2m FM \\u00e9\"}]}";

static const char STATUS_JSON[] =
    "{\"receiver\": {\"name\": \"ON4PRA SDR\", \"admin\": \"on4pra@example.org\", \"gps\": {\"lat\": 51.2, "
    "\"lon\": 3.22}, \"asl\": 12, \"location\": \"Brugge\"}, \"max_clients\": 20, \"version\": \"v1.2.126\", "
    "\"sdrs\": [{\"name\": \"RSPdx\", \"type\": \"SdrplaySource\", \"profiles\": "
    "[{\"name\": \"40m\", \"center_freq\": 7100000, \"sample_rate\": 500000}, "
    "{\"name\": \"20m\", \"center_freq\": 14175000, \"sample_rate\": 500000}]}, "
    "{\"name\": \"RTL-SDR v3\", \"type\": \"RtlSdrSource\", \"profiles\": "
    "[{\"name\": \"2m FM \\u00e9\", \"center_freq\": 145000000, \"sample_rate\": 2400000}]}]}";

/* ---------------------------------------------------------- the checks */

static void test_url(void)
{
    CASE("url");
    owrx_url_t u;
    CHECK(owrx_url("http://sdr.on4pra.be/", &u));
    CHECK_STR(u.host, "sdr.on4pra.be");
    CHECK_EQ(u.port, 80);
    CHECK(!u.tls);
    CHECK_STR(u.path, "/");
    CHECK(owrx_url("https://fms.komkon.org/OWRX/", &u));
    CHECK_STR(u.host, "fms.komkon.org");
    CHECK_EQ(u.port, 443);
    CHECK(u.tls);
    CHECK_STR(u.path, "/OWRX/");
    CHECK(owrx_url("http://watou.on4ipr.be:8076/", &u));
    CHECK_EQ(u.port, 8076);
    CHECK_STR(u.path, "/");
    /* As people type them. */
    CHECK(owrx_url("  watou.on4ipr.be:8076", &u));
    CHECK_STR(u.host, "watou.on4ipr.be");
    CHECK_EQ(u.port, 8076);
    CHECK(!u.tls);
    CHECK(owrx_url("sdr.on4pra.be", &u));
    CHECK_EQ(u.port, 80);
    CHECK(owrx_url("HTTPS://fms.komkon.org/OWRX", &u));
    CHECK(u.tls);
    CHECK_STR(u.path, "/OWRX/");
    CHECK(owrx_url("https://fms.komkon.org/OWRX/index.html#freq=7074000,mod=usb", &u));
    CHECK_STR(u.path, "/OWRX/");
    CHECK(owrx_url("https://fms.komkon.org//OWRX//?x=1", &u));
    CHECK_STR(u.path, "/OWRX/");
    CHECK(owrx_url("https://host:8443/a/b", &u));
    CHECK_EQ(u.port, 8443);
    CHECK_STR(u.path, "/a/b/");
    CHECK(owrx_url("http://host/#", &u));
    CHECK_STR(u.path, "/");
    /* Not addresses it can use. */
    CHECK(!owrx_url("", &u));
    CHECK(!owrx_url("http://", &u));
    CHECK(!owrx_url("ftp://host/", &u));
    CHECK(!owrx_url("ws://host/", &u));
    CHECK(!owrx_url("host:0", &u));
    CHECK(!owrx_url("host:70000", &u));
    CHECK(!owrx_url("host:80x/", &u));
    CHECK(!owrx_url("[::1]:8073", &u));
    CHECK(!owrx_url("ho st", &u) || !strcmp(u.host, "ho"));
    CHECK(!owrx_url("http://h\xc3\xa9.be/", &u));
    CHECK(!owrx_url("http://a.b/" "0123456789012345678901234567890123456789012345678901234567890123/", &u));
    char long_host[80];
    memset(long_host, 'a', 70);
    long_host[70] = 0;
    CHECK(!owrx_url(long_host, &u));
    /* A scheme's "://" further on is no scheme of its own. */
    CHECK(owrx_url("host/x?u=ftp://y", &u));
    CHECK_STR(u.host, "host");

    CASE("path and key");
    char p[80];
    CHECK(owrx_url("https://fms.komkon.org/OWRX/", &u));
    CHECK_EQ(owrx_path(&u, "ws/", p, sizeof p), 9);
    CHECK_STR(p, "/OWRX/ws/");
    owrx_path(&u, "status.json", p, sizeof p);
    CHECK_STR(p, "/OWRX/status.json");
    CHECK_EQ(owrx_path(&u, "status.json", p, 10), -1);
    owrx_url_t a, b;
    owrx_url("http://SDR.on4pra.be/", &a);
    const char hp[] = "sdr.on4pra.be:80";
    CHECK_EQ(owrx_key(&a), owrx_fnv(OWRX_FNV0, hp, sizeof hp - 1));   /* kiwi_hp's, at "/" */
    owrx_url("https://fms.komkon.org/OWRX/", &a);
    owrx_url("https://fms.komkon.org/other/", &b);
    CHECK(owrx_key(&a) != owrx_key(&b));
    owrx_url("https://fms.komkon.org/OWRX", &b);
    CHECK_EQ(owrx_key(&a), owrx_key(&b));
}

static void test_writers(void)
{
    char o[512];
    CASE("hello, props, start");
    CHECK_EQ(owrx_hello_cmd(o, sizeof o), 46);
    CHECK_STR(o, "SERVER DE CLIENT client=VFO-Knob type=receiver");
    owrx_props_cmd(o, sizeof o);
    CHECK_STR(o, "{\"type\":\"connectionproperties\",\"params\":{\"output_rate\":12000,\"hd_output_rate\":48000}}");
    owrx_start_cmd(o, sizeof o);
    CHECK_STR(o, "{\"type\":\"dspcontrol\",\"action\":\"start\"}");
    CHECK_EQ(owrx_start_cmd(o, 10), -1);

    CASE("tune");
    owrx_tune_t t = { 14074000, "usb", 300, 3000, -150 };
    CHECK(owrx_tune_cmd(o, sizeof o, &t, 14100000, 2400000, false, OWRX_P_ALL) > 0);
    CHECK_STR(o, "{\"type\":\"dspcontrol\",\"params\":{\"low_cut\":300,\"high_cut\":3000,\"offset_freq\":-26000,"
                 "\"mod\":\"usb\",\"squelch_level\":-150,\"secondary_mod\":false}}");
    owrx_tune_cmd(o, sizeof o, &t, 14100000, 2400000, false, OWRX_P_OFFSET);
    CHECK_STR(o, "{\"type\":\"dspcontrol\",\"params\":{\"offset_freq\":-26000}}");
    /* CW: the dial is the carrier, the receiver listens 800 Hz below it. */
    owrx_tune_t c = { 7030000, "cw", -100, 100, -150 };
    owrx_tune_cmd(o, sizeof o, &c, 7100000, 500000, true, OWRX_P_PASS | OWRX_P_OFFSET | OWRX_P_MOD);
    CHECK_STR(o, "{\"type\":\"dspcontrol\",\"params\":{\"low_cut\":700,\"high_cut\":900,\"offset_freq\":-70800,"
                 "\"mod\":\"cw\"}}");
    /* Within its span and its page's passband limits; 100 Hz at the least. */
    owrx_tune_t w = { 30000000, "am", -9000, 9000, -200 };
    owrx_tune_cmd(o, sizeof o, &w, 14100000, 2400000, false, OWRX_P_ALL);
    CHECK_STR(o, "{\"type\":\"dspcontrol\",\"params\":{\"low_cut\":-5999,\"high_cut\":5999,\"offset_freq\":1200000,"
                 "\"mod\":\"am\",\"squelch_level\":-150,\"secondary_mod\":false}}");
    owrx_tune_t f = { 98000000, "wfm", -120000, 120000, 5 };
    owrx_tune_cmd(o, sizeof o, &f, 98500000, 2400000, false, OWRX_P_PASS | OWRX_P_SQ);
    CHECK_STR(o, "{\"type\":\"dspcontrol\",\"params\":{\"low_cut\":-100000,\"high_cut\":100000,\"squelch_level\":0}}");
    owrx_tune_t nn = { 14074000, "usb", 3000, 3010, -150 };
    owrx_tune_cmd(o, sizeof o, &nn, 14100000, 2400000, false, OWRX_P_PASS);
    CHECK_STR(o, "{\"type\":\"dspcontrol\",\"params\":{\"low_cut\":2955,\"high_cut\":3055}}");
    owrx_tune_t edge = { 14074000, "usb", 5990, 5999, -150 };
    owrx_tune_cmd(o, sizeof o, &edge, 14100000, 2400000, false, OWRX_P_PASS);
    CHECK_STR(o, "{\"type\":\"dspcontrol\",\"params\":{\"low_cut\":5899,\"high_cut\":5999}}");
    /* SAM is OpenWebRX+'s alone: upstream's audio would stop. */
    owrx_tune_t sam = { 6000000, "sam", -4000, 4000, -150 };
    CHECK_EQ(owrx_tune_cmd(o, sizeof o, &sam, 6000000, 1000000, false, OWRX_P_MOD), -1);
    CHECK(owrx_tune_cmd(o, sizeof o, &sam, 6000000, 1000000, true, OWRX_P_MOD) > 0);
    owrx_tune_t bad = { 6000000, "drm", -4000, 4000, -150 };
    CHECK_EQ(owrx_tune_cmd(o, sizeof o, &bad, 6000000, 1000000, true, OWRX_P_MOD), -1);
    CHECK_EQ(owrx_tune_cmd(o, sizeof o, &t, 14100000, 2400000, false, 0), -1);
    CHECK_EQ(owrx_tune_cmd(o, 40, &t, 14100000, 2400000, false, OWRX_P_ALL), -1);
    /* Every size a buffer can be: never past it, always ended. */
    for (size_t cap = 1; cap < 160; cap++) {
        char small[160];
        memset(small, 'x', sizeof small);
        const int n = owrx_tune_cmd(small, cap, &t, 14100000, 2400000, false, OWRX_P_ALL);
        CHECK(n == -1 || (size_t)n < cap);
        CHECK(small[cap] == 'x' || cap == sizeof small);
    }

    CASE("select");
    owrx_select_cmd(o, sizeof o, "rspdx|40m");
    CHECK_STR(o, "{\"type\":\"selectprofile\",\"params\":{\"profile\":\"rspdx|40m\"}}");
    owrx_select_cmd(o, sizeof o, "a\"b\\c\nd");
    CHECK_STR(o, "{\"type\":\"selectprofile\",\"params\":{\"profile\":\"a\\\"b\\\\c\\u000ad\"}}");
    CHECK_EQ(owrx_select_cmd(o, sizeof o, ""), -1);
    for (size_t cap = 1; cap < 80; cap++) {
        char small[80];
        memset(small, 'x', sizeof small);
        const int n = owrx_select_cmd(small, cap, "rspdx|40m");
        CHECK(n == -1 || (n > 0 && (size_t)n < cap && small[n] == 0));
    }
}

static void test_said(void)
{
    static owrx_said_t s;
    CASE("hello");
    owrx_said_init(&s);
    CHECK_EQ(text(&s, HELLO_P, 0), OWRX_EV_HELLO);
    CHECK(s.hello);
    CHECK_STR(s.version, "v1.2.126");
    owrx_said_init(&s);
    CHECK_EQ(text(&s, "CLIENT DE SERVER server=other version=v1", 3), OWRX_EV_END);
    CHECK_EQ(s.end, OWRX_END_NOT_OWRX);
    owrx_said_init(&s);
    CHECK_EQ(text(&s, "hello there", 0), 0);
    CHECK(!s.hello);

    CASE("details, clients, meter");
    owrx_said_init(&s);
    CHECK_EQ(text(&s, DETAILS, 0), OWRX_EV_NAME);
    CHECK_STR(s.name, "ON4PRA Br\xc3\xbcgge \xf0\x9f\x93\xa1");
    /* Half a surrogate pair, at the end or before another character. */
    text(&s, "{\"type\": \"receiver_details\", \"value\": {\"receiver_name\": \"A\\ud83dB\\udce1\\ud83d\"}}", 0);
    CHECK_STR(s.name, "A\xef\xbf\xbd" "B\xef\xbf\xbd\xef\xbf\xbd");
    CHECK_EQ(text(&s, "{\"type\": \"clients\", \"value\": 3}", 0), OWRX_EV_CLIENTS);
    CHECK_EQ(s.clients, 3);
    CHECK_EQ(text(&s, "{\"type\": \"smeter\", \"value\": 3.0517578125e-05}", 0), OWRX_EV_METER);
    CHECK(s.have_meter);
    CHECK(fabsf(owrx_db(s.meter) - (-45.154f)) < 0.01f);
    CHECK_EQ(text(&s, "{\"type\": \"smeter\", \"value\": 0.0}", 0), OWRX_EV_METER);
    CHECK_EQ((int)owrx_db(s.meter), -150);
    CHECK_EQ((int)owrx_db(NAN), -150);
    CHECK_EQ((int)owrx_db(-1.0f), -150);
    CHECK_EQ((int)lroundf(owrx_db(1.0f)), 0);

    CASE("config");
    owrx_said_init(&s);
    CHECK_EQ(text(&s, CONFIG_GLOBAL_U, 0), OWRX_EV_CONFIG);
    CHECK_EQ(s.fft_size, 4096);
    CHECK_EQ(s.max_clients, 20);
    CHECK(!s.audio_raw);
    CHECK(!s.plus);
    float lo, hi;
    owrx_meter_range(&s, &lo, &hi);
    CHECK_EQ((int)lo, -108);
    CHECK_EQ((int)hi, 0);
    uint32_t ev = text(&s, CONFIG_PROFILE_U, 0);
    CHECK_EQ(ev, OWRX_EV_SPAN | OWRX_EV_SDR | OWRX_EV_CONFIG);
    CHECK_EQ(s.center, 14100000);
    CHECK_EQ(s.rate, 2400000);
    CHECK_EQ(s.start, 14070000);
    CHECK_STR(s.start_mod, "usb");
    CHECK_STR(s.sdr_id, "9f3c1d2e-0a1b-4c5d-8e7f-6a5b4c3d2e1f");
    CHECK_EQ(s.sq_init, -150);
    CHECK(owrx_in_span(&s, 12900000));
    CHECK(owrx_in_span(&s, 15300000));
    CHECK(!owrx_in_span(&s, 15300001));
    /* The same SDR again: a retune, not a new start of its audio. */
    CHECK_EQ(text(&s, CONFIG_PROFILE_U, 5), OWRX_EV_SPAN | OWRX_EV_CONFIG);
    /* Another SDR's layer, all nulls: nothing it had is lost, nothing to do. */
    CHECK_EQ(text(&s, CONFIG_NULLS, 0), 0);
    CHECK_EQ(s.center, 14100000);
    CHECK_STR(s.start_mod, "usb");
    ev = text(&s, CONFIG_PROFILE_P, 0);
    CHECK_EQ(ev, OWRX_EV_SPAN | OWRX_EV_SDR | OWRX_EV_CONFIG);
    CHECK(s.plus);
    CHECK_EQ(s.tuning_step, 500);
    CHECK_EQ(s.sq_init, -60);
    CHECK(s.audio_raw);
    owrx_meter_range(&s, &lo, &hi);
    CHECK(fabsf(lo - -115.5f) < 0.01f && fabsf(hi - -15.0f) < 0.01f);
    CHECK_EQ(owrx_squelch_db(&s, 0), -150);
    CHECK_EQ(owrx_squelch_db(&s, 50), -65);
    CHECK_EQ(owrx_squelch_db(&s, 100), -15);
    CHECK_EQ(owrx_squelch_db(&s, 200), -15);
    /* A squelch level that is no integer, or none: open. */
    text(&s, "{\"type\": \"config\", \"value\": {\"initial_squelch_level\": -60.5}}", 0);
    CHECK_EQ(s.sq_init, -150);
    text(&s, "{\"type\": \"config\", \"value\": {\"initial_squelch_level\": -60}}", 0);
    text(&s, "{\"type\": \"config\", \"value\": {\"initial_squelch_level\": null}}", 0);
    CHECK_EQ(s.sq_init, -150);
    /* A center past 2^31 (a microwave profile), a float written as one. */
    text(&s, "{\"type\": \"config\", \"value\": {\"center_freq\": 10489750000, \"samp_rate\": 1e6}}", 0);
    CHECK_EQ(s.center, 10489750000LL);
    CHECK_EQ(s.rate, 1000000);
    /* The P-only key as an object. */
    owrx_said_init(&s);
    text(&s, "{\"type\": \"config\", \"value\": {\"receiver_gps\": {\"lat\": 1, \"lon\": 2}}}", 1);
    CHECK(s.plus);
    owrx_said_init(&s);
    text(&s, "{\"type\": \"temperature\", \"value\": 51.5}", 0);
    CHECK(s.plus);

    CASE("profiles");
    owrx_said_init(&s);
    CHECK_EQ(text(&s, PROFILES, 0), OWRX_EV_BANDS);
    CHECK_EQ(s.n_bands, 3);
    CHECK_EQ(s.bands_seen, 3);
    CHECK_STR(s.bands[0].id, "rspdx|40m");
    CHECK_STR(s.bands[0].name, "RSPdx 40m");
    CHECK_STR(s.bands[2].id, "9f3c1d2e-0a1b-4c5d-8e7f-6a5b4c3d2e1f|b6ba4c2e-2f6e-4d5a-9a7c-3b1f2a9e8d01");
    CHECK_STR(s.bands[2].name, "RTL-SDR v3 2m FM \xc3\xa9");
    const char full[] = "RTL-SDR v3 2m FM \xc3\xa9";
    CHECK_EQ(s.bands[2].name_h, owrx_fnv(OWRX_FNV0, full, sizeof full - 1));
    text(&s, CONFIG_PROFILE_U, 0);
    CHECK_EQ(owrx_band_now(&s), 2);
    /* None, then more than it keeps -- and ids too long to send back. */
    CHECK_EQ(text(&s, "{\"type\": \"profiles\", \"value\": []}", 0), OWRX_EV_BANDS);
    CHECK_EQ(s.n_bands, 0);
    CHECK_EQ(owrx_band_now(&s), -1);
    static char big[16384];
    int o = snprintf(big, sizeof big, "{\"type\": \"profiles\", \"value\": [");
    for (int i = 0; i < 30; i++)
        o += snprintf(big + o, sizeof big - (size_t)o, "%s{\"id\": \"sdr|p%02d%s\", \"name\": \"SDR band %d with a name longer than the knob keeps of it\"}",
                      i ? ", " : "", i, i == 3 ? "012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789" : "", i);
    snprintf(big + o, sizeof big - (size_t)o, "]}");
    text(&s, big, 7);
    CHECK_EQ(s.n_bands, OWRX_BANDS);
    CHECK_EQ(s.bands_seen, 30);
    CHECK_STR(s.bands[3].id, "");
    CHECK_STR(s.bands[4].id, "sdr|p04");
    CHECK_EQ(strlen(s.bands[4].name), OWRX_NAME_MAX - 1);
    every_split(PROFILES);

    CASE("refusals");
    owrx_said_init(&s);
    CHECK_EQ(text(&s, "{\"type\": \"backoff\", \"reason\": \"Too many clients\"}", 0), OWRX_EV_END);
    CHECK_EQ(s.end, OWRX_END_FULL);
    CHECK_STR(s.reason, "Too many clients");
    owrx_said_init(&s);
    CHECK_EQ(text(&s, "{\"type\": \"backoff\", \"reason\": \"Client address banned\"}", 2), OWRX_EV_END);
    CHECK_EQ(s.end, OWRX_END_BANNED);
    owrx_said_init(&s);
    CHECK_EQ(text(&s, "{\"type\": \"backoff\"}", 0), OWRX_EV_END);
    CHECK_EQ(s.end, OWRX_END_FULL);
    owrx_said_init(&s);
    CHECK_EQ(text(&s, "{\"type\": \"sdr_error\", \"value\": \"No SDR Devices available\"}", 0), OWRX_EV_END);
    CHECK_EQ(s.end, OWRX_END_NO_SDR);
    CHECK_EQ(text(&s, "{\"type\": \"log_message\", \"value\": \"This profile is locked, keeping current profile.\"}", 0),
             OWRX_EV_LOG);
    CHECK_STR(s.log, "This profile is locked, keeping current profile.");
    CHECK_EQ(text(&s, "{\"type\": \"demodulator_error\", \"value\": \"sam\"}", 0), OWRX_EV_DEMOD);

    CASE("passed over");
    /* 300 kB of bookmarks: known by its first bytes, the rest not read. */
    owrx_said_init(&s);
    text(&s, CONFIG_PROFILE_U, 0);
    static char marks[300 * 1024];
    o = snprintf(marks, sizeof marks, "{\"type\": \"bookmarks\", \"value\": [");
    while ((size_t)o < sizeof marks - 200)
        o += snprintf(marks + o, sizeof marks - (size_t)o, "{\"name\": \"x\", \"frequency\": 1, \"modulation\": \"am\"}, ");
    snprintf(marks + o, sizeof marks - (size_t)o, "{}]}");
    CHECK_EQ(text(&s, marks, 1400), 0);
    CHECK(s.skip);
    CHECK_STR(s.type, "bookmarks");
    CHECK_EQ(s.center, 14100000);
    CHECK_EQ(text(&s, "{\"type\": \"modes\", \"value\": [{\"modulation\": \"usb\"}]}", 0), 0);
    CHECK_EQ(text(&s, "{\"type\": \"cpuusage\", \"value\": 0.25}", 0), 0);
    /* Not JSON, or "type" not first: nothing taken, nothing broken. */
    CHECK_EQ(text(&s, "{\"type\": \"smeter\", \"value\": 1e-3,,}", 0), OWRX_EV_METER);
    CHECK_EQ(text(&s, "{\"value\": 5, \"type\": \"clients\"}", 0), 0);
    CHECK_EQ(text(&s, "{\"type\": 5, \"value\": 5}", 0), 0);
    CHECK_EQ(text(&s, "{\"type\": \"clients\", \"value\": \"many\"}", 0), 0);
    CHECK_EQ(text(&s, "", 0), 0);

    CASE("every split");
    every_split(HELLO_U);
    every_split(DETAILS);
    every_split(CONFIG_GLOBAL_U);
    every_split(CONFIG_PROFILE_U);
    every_split(CONFIG_PROFILE_P);
    every_split("{\"type\": \"smeter\", \"value\": 3.0517578125e-05}");
    every_split("{\"type\": \"clients\", \"value\": 12}");
    every_split("{\"type\": \"backoff\", \"reason\": \"Client address banned\"}");
}

static void test_status(void)
{
    static owrx_status_t st;
    static owrx_said_t s;
    CASE("status.json");
    owrx_status_init(&st);
    for (size_t i = 0; i < sizeof STATUS_JSON - 1; i += 3)
        owrx_status_feed(&st, (const uint8_t *)STATUS_JSON + i, sizeof STATUS_JSON - 1 - i < 3 ? sizeof STATUS_JSON - 1 - i : 3);
    CHECK(owrx_status_end(&st));
    CHECK_STR(st.name, "ON4PRA SDR");
    CHECK_STR(st.version, "v1.2.126");
    CHECK(owrx_plus_version(st.version));
    CHECK_EQ(st.max_clients, 20);
    CHECK_EQ(st.n_sdrs, 2);
    CHECK_EQ(st.profiles, 3);
    CHECK_EQ(st.p[2].center, 145000000);
    CHECK_EQ(st.p[2].rate, 2400000);
    CHECK_EQ(st.p[2].sdr, 1);
    CHECK_EQ(st.p[2].k, 0);

    CASE("join by name");
    owrx_said_init(&s);
    text(&s, PROFILES, 0);
    CHECK_EQ(owrx_join(&s, &st), 3);
    CHECK_EQ(s.bands[0].center, 7100000);
    CHECK_EQ(s.bands[1].center, 14175000);
    CHECK_STR(s.bands[1].name + s.bands[1].label, "20m");
    CHECK_STR(s.bands[2].name + s.bands[2].label, "2m FM \xc3\xa9");
    CHECK_EQ(owrx_band_for(&s, 14200000), 1);
    CHECK_EQ(owrx_band_for(&s, 145500000), 2);
    CHECK_EQ(owrx_band_for(&s, 50000000), -1);

    CASE("join by order");
    /* Names that do not agree (an SDR renamed since): each SDR's k-th. */
    owrx_said_init(&s);
    text(&s, "{\"type\": \"profiles\", \"value\": [{\"id\": \"a|1\", \"name\": \"X 40m\"}, "
             "{\"id\": \"a|2\", \"name\": \"X 20m\"}, {\"id\": \"b|1\", \"name\": \"Y 2m\"}]}", 0);
    CHECK_EQ(owrx_join(&s, &st), 3);
    CHECK_EQ(s.bands[1].center, 14175000);
    CHECK_EQ(s.bands[2].center, 145000000);
    CHECK_EQ(s.bands[2].label, 0);
    /* ...and not where the counts do not agree. */
    owrx_said_init(&s);
    text(&s, "{\"type\": \"profiles\", \"value\": [{\"id\": \"a|1\", \"name\": \"X 40m\"}, "
             "{\"id\": \"b|1\", \"name\": \"Y 2m\"}, {\"id\": \"b|2\", \"name\": \"Y 70cm\"}]}", 0);
    CHECK_EQ(owrx_join(&s, &st), 0);
    CHECK_EQ(s.bands[0].center, 0);

    CASE("status, not OpenWebRX's");
    owrx_status_init(&st);
    const char *html = "<html><body>404</body></html>";
    owrx_status_feed(&st, (const uint8_t *)html, strlen(html));
    CHECK(!owrx_status_end(&st));
    owrx_status_init(&st);
    const char *nosdr = "{\"receiver\": {\"name\": \"x\"}, \"version\": \"v1.2.2\", \"sdrs\": []}";
    owrx_status_feed(&st, (const uint8_t *)nosdr, strlen(nosdr));
    CHECK(owrx_status_end(&st));
    CHECK(!owrx_plus_version(st.version));
    CHECK(!owrx_plus_version("v1.3.0-dev"));
    CHECK(!owrx_plus_version("1.2.2"));
    CHECK(owrx_plus_version("1.2.3"));
    CHECK(!owrx_plus_version(""));
}

static void test_modes(void)
{
    CASE("modes");
    CHECK_STR(owrx_mode_of("usb", 14000000, false), "usb");
    CHECK_STR(owrx_mode_of("SAM", 6000000, false), "am");
    CHECK_STR(owrx_mode_of("sam", 6000000, true), "sam");
    CHECK_STR(owrx_mode_of("ft8", 7074000, false), "usb");
    CHECK_STR(owrx_mode_of("packet", 144800000, false), "nfm");
    CHECK_STR(owrx_mode_of("dmr", 438000000, false), "nfm");
    CHECK_STR(owrx_mode_of("rtty45", 14080000, false), "usb");
    CHECK_STR(owrx_mode_of("sonde-rs41", 403000000, false), "nfm");
    CHECK_STR(owrx_mode_of("dab", 225648000, false), "wfm");
    CHECK_STR(owrx_mode_of("nbfm", 145000000, false), "nfm");
    CHECK_STR(owrx_mode_of("digl", 7000000, false), "lsb");
    CHECK_STR(owrx_mode_of("adsb", 1090000000, false), "nfm");
    CHECK_STR(owrx_mode_of("ism", 3500000, false), "lsb");
    CHECK_STR(owrx_mode_of("", 14000000, false), "usb");
    CHECK_STR(owrx_mode_of(NULL, 0, false), "usb");
    int32_t lo, hi;
    owrx_passband(owrx_mode_find("usb"), false, &lo, &hi);
    CHECK(lo == 300 && hi == 3000);
    owrx_passband(owrx_mode_find("usb"), true, &lo, &hi);
    CHECK(lo == 150 && hi == 2750);
    owrx_passband(owrx_mode_find("LSB"), true, &lo, &hi);
    CHECK(lo == -2750 && hi == -150);
    CHECK(owrx_mode_find("wfm")->flags & OWRX_M_HD);
    CHECK(!owrx_mode_find("ft8"));
}

static void test_ends(void)
{
    CASE("ends");
    for (int e = 0; e <= OWRX_END_LAST + 2; e++) {
        CHECK(strlen(owrx_end_word((owrx_end_t)e)) <= 15);
        if (e >= OWRX_END_NOT_FOUND && e <= OWRX_END_LAST) CHECK(*owrx_end_word((owrx_end_t)e));
    }
    CHECK_STR(owrx_end_word(OWRX_END_NO_ROUTE), "CAN'T REACH");
    CHECK_EQ(owrx_http(101), OWRX_END_NONE);
    CHECK_EQ(owrx_http(0), OWRX_END_NO_ANSWER);
    CHECK_EQ(owrx_http(404), OWRX_END_NOT_OWRX);
    CHECK_EQ(owrx_http(403), OWRX_END_REFUSED);
    CHECK_EQ(owrx_http(502), OWRX_END_DOWN);
    CHECK_EQ(owrx_http(301), OWRX_END_MOVED);
    CHECK_EQ(owrx_http(429), OWRX_END_FULL);
    CHECK_EQ(owrx_retry_ms(OWRX_END_FULL, 1), 32000);
    CHECK_EQ(owrx_retry_ms(OWRX_END_FULL, 7), 32000);
    CHECK_EQ(owrx_retry_ms(OWRX_END_NO_ROUTE, 1), 1000);
    CHECK_EQ(owrx_retry_ms(OWRX_END_NO_ROUTE, 4), 8000);
    CHECK_EQ(owrx_retry_ms(OWRX_END_NO_ROUTE, 10), 512000);
    CHECK_EQ(owrx_retry_ms(OWRX_END_NO_ROUTE, 1000), 512000);
    CHECK_EQ(owrx_retry_ms(OWRX_END_CLOSED, 0), 1000);
    CHECK_EQ(owrx_retry_ms(OWRX_END_NO_SDR, 1), 60000);
    CHECK_EQ(owrx_retry_ms(OWRX_END_NO_SDR, 9), 480000);
    CHECK_EQ(owrx_retry_ms(OWRX_END_BANNED, 1), 0);
    CHECK_EQ(owrx_retry_ms(OWRX_END_CERT, 1), 0);
    CHECK(OWRX_SWITCH_GAP_MS > 10000);
}

/* ------------------------------------------------------------- audio */

/* csdr's encoder, for streams of the test's own making; tools/owrx_adpcm.py
 * is checked against csdr's itself, and the decoder against it below. */
static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
    88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
    658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int8_t ADJ[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
typedef struct { int idx, prev, counter; } enc_t;
static int enc_dec(enc_t *e, int code)
{
    const int step = STEP[e->idx];
    int diff = step >> 3;
    if (code & 1) diff += step >> 2;
    if (code & 2) diff += step >> 1;
    if (code & 4) diff += step;
    if (code & 8) diff = -diff;
    e->prev += diff;
    if (e->prev > 32767) e->prev = 32767;
    if (e->prev < -32768) e->prev = -32768;
    e->idx += ADJ[code];
    if (e->idx < 0) e->idx = 0;
    if (e->idx > 88) e->idx = 88;
    return e->prev;
}
static int enc_one(enc_t *e, int s)
{
    int diff = s - e->prev, step = STEP[e->idx], code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= step) { code |= 4; diff -= step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; }
    step >>= 1;
    if (diff >= step) code |= 1;
    enc_dec(e, code);
    return code;
}
/* `pairs` sample pairs from `x`: the bytes into out, the samples a decoder
 * must give into want. */
static size_t encode(enc_t *e, const int16_t *x, size_t pairs, uint8_t *out, int16_t *want)
{
    size_t o = 0;
    for (size_t i = 0; i < pairs; i++) {
        if (e->counter <= 0) {
            memcpy(out + o, "SYNC", 4);
            out[o + 4] = (uint8_t)e->idx;
            out[o + 5] = (uint8_t)(e->idx >> 8);
            out[o + 6] = (uint8_t)e->prev;
            out[o + 7] = (uint8_t)((unsigned)e->prev >> 8);
            o += 8;
            e->counter = 1000;
        } else {
            e->counter--;
        }
        const int lo = enc_one(e, x[2 * i]);
        want[2 * i] = (int16_t)e->prev;
        const int hi = enc_one(e, x[2 * i + 1]);
        want[2 * i + 1] = (int16_t)e->prev;
        out[o++] = (uint8_t)(lo | hi << 4);
    }
    return o;
}

#define NS 48000
static int16_t sig[NS], want[NS], got[2 * NS + 64];
static uint8_t stream[NS + NS / 100 + 64];

/* A decoder a session's SYNC-framed audio has already gone through: what
 * it looks for from there on is the next SYNC, nothing plain. */
static void primed(owrx_adpcm_t *d)
{
    static uint8_t blk[8 + 1001];
    static int16_t scratch[2 * sizeof blk];
    memcpy(blk, "SYNC\0\0\0\0", 8);
    owrx_adpcm_reset(d);
    CHECK_EQ(owrx_adpcm_feed(d, blk, sizeof blk, scratch), 2002);
    CHECK(d->syncs == 1 && !d->plain);
}

static void test_adpcm(void)
{
    CASE("adpcm");
    for (int i = 0; i < NS; i++)
        sig[i] = (int16_t)(9000.0 * sin(2 * PI * 700.0 * i / 12000.0) + (int)(rnd() % 2001) - 1000);
    enc_t e = { 0, 0, 0 };
    const size_t n = encode(&e, sig, NS / 2, stream, want);
    CHECK_EQ(n, (size_t)(NS / 2) + 8 * ((NS / 2 + 1000) / 1001));
    CHECK(!memcmp(stream, "SYNC\0\0\0\0", 8));
    /* Whole, and in pieces of every size: SYNCs broken anywhere. */
    for (size_t piece = 1; piece <= 4100; piece += piece < 20 ? 1 : 211) {
        owrx_adpcm_t d;
        owrx_adpcm_reset(&d);
        size_t o = 0;
        for (size_t i = 0; i < n; i += piece)
            o += owrx_adpcm_feed(&d, stream + i, n - i < piece ? n - i : piece, got + o);
        CHECK_EQ(o, NS);
        CHECK(!memcmp(got, want, sizeof want));
        CHECK_EQ(d.lost, 0);
        CHECK_EQ(d.syncs, (NS / 2 + 1000) / 1001);
    }
    /* A frame lost: back in step at the next SYNC, exactly. */
    {
        owrx_adpcm_t d;
        owrx_adpcm_reset(&d);
        const size_t cut_at = 3000, cut_len = 777;
        size_t o = owrx_adpcm_feed(&d, stream, cut_at, got);
        o += owrx_adpcm_feed(&d, stream + cut_at + cut_len, n - cut_at - cut_len, got + o);
        /* The block it fell in is decoded to its count, garbage; the next
         * SYNC (the 5th, at 4 * 1009) puts it right. */
        const size_t wfrom = 2 * 4 * 1001;              /* the samples from the 5th SYNC on */
        CHECK(o >= NS - 2 * cut_len - 2002);
        CHECK(!memcmp(got + o - (NS - wfrom), want + wfrom, (NS - wfrom) * sizeof(int16_t)));
    }
    /* Joined in the middle (a session's first frame cut short): taken as
     * plain at first -- 509 bytes of noise -- until its next SYNC, the one
     * at 1009, then exactly. */
    {
        owrx_adpcm_t d;
        owrx_adpcm_reset(&d);
        const size_t o = owrx_adpcm_feed(&d, stream + 500, n - 500, got);
        CHECK_EQ(o, NS - 2002 + 2 * (509 + 8));
        CHECK(!memcmp(got + 2 * (509 + 8), want + 2002, (NS - 2002) * sizeof(int16_t)));
        CHECK(!d.plain && d.syncs > 0 && d.lost == 0);
    }
    /* "SYNC" by chance in the audio, its index past 88: not taken. */
    {
        uint8_t fake[] = { 'S', 'S', 'Y', 'N', 'C', 0xFF, 0x7F, 0, 0, 'S', 'Y', 'N', 'C', 5, 0, 0x10, 0x00, 0x77 };
        owrx_adpcm_t d;
        primed(&d);
        const size_t o = owrx_adpcm_feed(&d, fake, sizeof fake, got);
        CHECK_EQ(o, 2);
        CHECK_EQ(d.syncs, 2);
        enc_t x = { 5, 16, 1 };
        CHECK_EQ(got[0], (int16_t)enc_dec(&x, 7));
        CHECK_EQ(got[1], (int16_t)enc_dec(&x, 7));
        /* ...and a stray 'S' just before a real one, which is taken. */
        const uint8_t stray[] = { 'x', 'S', 'S', 'Y', 'N', 'C', 5, 0, 0x10, 0x00, 0x77 };
        primed(&d);
        CHECK_EQ(owrx_adpcm_feed(&d, stray, sizeof stray, got), 2);
        CHECK_EQ(d.syncs, 2);
        CHECK_EQ(d.lost, 2);
    }
    /* A new SDR's encoder, mid-block: resync, and its first SYNC is taken
     * -- never plain, whatever comes first. */
    {
        owrx_adpcm_t d;
        owrx_adpcm_reset(&d);
        size_t o = owrx_adpcm_feed(&d, stream, 600, got);
        owrx_adpcm_resync(&d);
        o = owrx_adpcm_feed(&d, stream, n, got);
        CHECK_EQ(o, NS);
        CHECK(!memcmp(got, want, sizeof want));
        owrx_adpcm_resync(&d);
        o = owrx_adpcm_feed(&d, stream + 300, n - 300, got);
        CHECK_EQ(o, NS - 2002);
        CHECK(!memcmp(got, want + 2002, (NS - 2002) * sizeof(int16_t)));
        CHECK(!d.plain && d.lost > 0);
    }

    CASE("adpcm plain (OpenWebRX 1.0, 1.1)");
    {
        /* csdr's encode_ima_adpcm_i16_u8: no SYNC ever. */
        enc_t p = { 0, 0, INT_MAX };
        const size_t pn = encode(&p, sig, NS / 2, stream, want);
        CHECK_EQ(pn, (size_t)(NS / 2));
        for (size_t piece = 1; piece <= 4100; piece += piece < 20 ? 1 : 211) {
            owrx_adpcm_t d;
            owrx_adpcm_reset(&d);
            size_t o = 0;
            for (size_t i = 0; i < pn; i += piece) {
                o += owrx_adpcm_feed(&d, stream + i, pn - i < piece ? pn - i : piece, got + o);
                /* Another SDR, a refused band: on as before, as its page goes on. */
                if (i == 7 * piece) owrx_adpcm_resync(&d);
            }
            CHECK_EQ(o, NS);
            CHECK(!memcmp(got, want, sizeof want));
            CHECK(d.plain && d.syncs == 0 && d.lost == 0);
        }
        /* Starting as "SY" by chance: those two bytes into its state, not
         * played, and on exactly from the third. */
        const uint8_t sy[] = { 'S', 'Y', 0x12, 0x9A, 0x07, 0xF3 };
        enc_t x = { 0, 0, INT_MAX };
        int16_t ref[12];
        for (int i = 0; i < 6; i++) {
            ref[2 * i] = (int16_t)enc_dec(&x, sy[i] & 0x0F);
            ref[2 * i + 1] = (int16_t)enc_dec(&x, sy[i] >> 4);
        }
        owrx_adpcm_t d;
        owrx_adpcm_reset(&d);
        CHECK_EQ(owrx_adpcm_feed(&d, sy, 1, got), 0);
        CHECK_EQ(owrx_adpcm_feed(&d, sy + 1, 5, got), 8);
        CHECK(!memcmp(got, ref + 4, 8 * sizeof(int16_t)));
        CHECK(d.plain);
        /* "SYNC" and an index past 88 to start with: plain, all 8 into its state. */
        const uint8_t bad[] = { 'S', 'Y', 'N', 'C', 0xFF, 0x7F, 0, 0, 0x77 };
        owrx_adpcm_reset(&d);
        CHECK_EQ(owrx_adpcm_feed(&d, bad, sizeof bad, got), 2);
        CHECK(d.plain && d.syncs == 0 && d.lost == 0);
    }

    CASE("pcm");
    uint8_t raw[7] = { 0x01, 0x02, 0x03, 0x04, 0xFF, 0x7F, 0x00 };
    owrx_adpcm_t d;
    owrx_adpcm_reset(&d);
    size_t o = owrx_pcm_feed(&d, raw, 3, got);
    o += owrx_pcm_feed(&d, raw + 3, 1, got + o);
    o += owrx_pcm_feed(&d, raw + 4, 2, got + o);
    o += owrx_pcm_feed(&d, raw + 6, 1, got + o);
    CHECK_EQ(o, 3);
    CHECK_EQ(got[0], 0x0201);
    CHECK_EQ(got[1], 0x0403);
    CHECK_EQ(got[2], 0x7FFF);
    CHECK(d.odd);
}

/* tools/owrx_adpcm.py's vectors: csdr's encoder (as ported, and checked
 * against csdr's own) against the knob's decoder, in pieces of odd sizes. */
static int vectors(const char *sp, const char *ep)
{
    CASE("vectors");
    FILE *f = fopen(sp, "rb"), *g = fopen(ep, "rb");
    if (!f || !g) {
        fprintf(stderr, "cannot open %s or %s\n", sp, ep);
        return 1;
    }
    static uint8_t vs[1 << 22];
    static int16_t ve[1 << 22], vo[1 << 22];
    const size_t n = fread(vs, 1, sizeof vs, f);
    const size_t m = fread(ve, 2, sizeof ve / 2, g);
    fclose(f);
    fclose(g);
    owrx_adpcm_t d;
    owrx_adpcm_reset(&d);
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        size_t k = 1 + rnd() % 1500;
        if (k > n - i) k = n - i;
        o += owrx_adpcm_feed(&d, vs + i, k, vo + o);
        i += k;
    }
    CHECK_EQ(o, m);
    CHECK(o == m && !memcmp(vo, ve, m * sizeof(int16_t)));
    CHECK_EQ(d.lost, 0);
    printf("vectors: %zu bytes, %zu samples, %u SYNCs\n", n, o, (unsigned)d.syncs);
    return 0;
}

int main(int argc, char **argv)
{
    test_url();
    test_writers();
    test_said();
    test_status();
    test_modes();
    test_ends();
    test_adpcm();
    if (argc == 3 && vectors(argv[1], argv[2])) return 1;
    printf("%s: %d checks, %d failed\n", __FILE__, t_run, t_fail);
    return t_fail ? 1 : 0;
}
