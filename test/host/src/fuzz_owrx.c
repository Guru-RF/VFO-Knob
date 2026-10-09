/* Deterministic byte fuzzer for OpenWebRX's protocol (components/owrx_proto).
 *
 * Everything an OpenWebRX receiver says reaches these functions -- its text
 * frames, its audio, its status.json -- from any address the operator types
 * in, and from a server that may be something else entirely; and so does
 * the address typed on the page. So they have to survive arbitrary bytes,
 * in pieces broken anywhere. Run under ASan/UBSan (on by default) this
 * catches overreads and overflows; the fixed PRNG keeps a failure
 * reproducible. A million rounds by default: random bytes, mutated real
 * messages, every truncation, deep nesting, long strings and escapes.
 *
 *   fuzz_owrx [ROUNDS [SEED]]     ROUNDS 0: until killed (an hour's run) */
#include "tiny.h"
#include "owrx_proto.h"

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* What receivers really say, as their json.dumps writes it. */
static const char *MSGS[] = {
    "CLIENT DE SERVER server=openwebrx version=v1.2.126",
    "CLIENT DE SERVER server=openwebrx version=v1.3.0-dev",
    "{\"type\": \"receiver_details\", \"value\": {\"receiver_name\": \"ON4PRA \\u00e9\\ud83d\\udce1\", "
    "\"receiver_gps\": {\"lat\": 51.2, \"lon\": 3.22}, \"photo_desc\": \"<b>x</b>\"}}",
    "{\"type\": \"config\", \"value\": {\"center_freq\": 14100000, \"samp_rate\": 2400000, "
    "\"start_freq\": 14070000, \"start_mod\": \"usb\", \"initial_squelch_level\": -150, "
    "\"profile_id\": \"b6ba4c2e-2f6e-4d5a-9a7c-3b1f2a9e8d01\", \"sdr_id\": \"9f3c1d2e\"}}",
    "{\"type\": \"config\", \"value\": {\"waterfall_colors\": [0, 1, 2], \"waterfall_levels\": "
    "{\"min\": -88, \"max\": -20}, \"fft_size\": 4096, \"audio_compression\": \"adpcm\", "
    "\"fft_compression\": \"none\", \"max_clients\": 20, \"tuning_step\": \"5000\"}}",
    "{\"type\": \"config\", \"value\": {\"center_freq\": null, \"samp_rate\": null, \"sdr_id\": null}}",
    "{\"type\": \"profiles\", \"value\": [{\"id\": \"rspdx|40m\", \"name\": \"RSPdx 40m\"}, "
    "{\"id\": \"a|b\", \"name\": \"RTL \\u00e9 2m\"}]}",
    "{\"type\": \"smeter\", \"value\": 3.0517578125e-05}",
    "{\"type\": \"clients\", \"value\": 3}",
    "{\"type\": \"backoff\", \"reason\": \"Too many clients\"}",
    "{\"type\": \"backoff\", \"reason\": \"Client address banned\"}",
    "{\"type\": \"sdr_error\", \"value\": \"No SDR Devices available\"}",
    "{\"type\": \"log_message\", \"value\": \"This profile is locked, keeping current profile.\"}",
    "{\"type\": \"bookmarks\", \"value\": [{\"name\": \"x\", \"frequency\": 1, \"modulation\": \"am\"}]}",
    "{\"type\": \"temperature\", \"value\": 51.5}",
    "{\"receiver\": {\"name\": \"ON4PRA SDR\"}, \"max_clients\": 20, \"version\": \"v1.2.126\", "
    "\"sdrs\": [{\"name\": \"RSPdx\", \"type\": \"SdrplaySource\", \"profiles\": [{\"name\": \"40m\", "
    "\"center_freq\": 7100000, \"sample_rate\": 500000}]}]}",
    "https://fms.komkon.org/OWRX/", "watou.on4ipr.be:8076", "http://sdr.on4pra.be/index.html#freq=1",
};
/* Pieces of JSON to splice in: the reader's hard cases. */
static const char *BITS[] = {
    "\\u", "\\ud83d", "\\udce1", "\\ud83d\\u0041", "\\\"", "\\", "\"", "{", "}", "[", "]", ",", ":",
    "null", "true", "1e999", "-0", "9223372036854775807", "-9223372036854775808", "NaN", "\x01",
    "\xc3", "\xf0\x9f", "SYNC", "SYNC\x05\x00\x10\x00", "SYNC\xff\x7f\x00\x00", "\"type\": \"config\"",
    "\"value\"", "{\"type\": \"profiles\", \"value\": [", "{\"id\": \"", "|", "\x00",
};

static uint8_t buf[70000];
static int16_t pcm[2 * 70000 + 8];

/* One input, every way in, broken into pieces anywhere. */
static void feed(const uint8_t *p, size_t n, owrx_said_t *s, owrx_status_t *st, owrx_adpcm_t *d)
{
    /* As a text frame. */
    if (rnd() % 16 == 0) owrx_said_init(s);
    owrx_text_begin(s);
    for (size_t i = 0; i < n;) {
        size_t k = 1 + rnd() % (rnd() % 4 ? 16 : 4096);
        if (k > n - i) k = n - i;
        owrx_text_feed(s, p + i, k);
        i += k;
    }
    const uint32_t ev = owrx_text_end(s);
    (void)ev;
    CHECK(s->n_bands <= OWRX_BANDS);
    for (int i = 0; i < s->n_bands; i++) {
        CHECK(memchr(s->bands[i].id, 0, sizeof s->bands[i].id));
        CHECK(memchr(s->bands[i].name, 0, sizeof s->bands[i].name));
        CHECK(s->bands[i].label <= strlen(s->bands[i].name));
    }
    CHECK(memchr(s->name, 0, sizeof s->name) && memchr(s->log, 0, sizeof s->log));
    CHECK(memchr(s->version, 0, sizeof s->version) && memchr(s->reason, 0, sizeof s->reason));
    CHECK((unsigned)s->end <= OWRX_END_LAST);
    owrx_band_now(s);
    owrx_band_for(s, (int64_t)rnd() * 1000);
    owrx_in_span(s, (int64_t)rnd() * 100);
    owrx_db(s->meter);
    owrx_squelch_db(s, (uint8_t)rnd());

    /* As status.json, and joined. */
    if (rnd() % 4 == 0) {
        owrx_status_init(st);
        for (size_t i = 0; i < n;) {
            size_t k = 1 + rnd() % 64;
            if (k > n - i) k = n - i;
            owrx_status_feed(st, p + i, k);
            i += k;
        }
        owrx_status_end(st);
        CHECK(st->n <= OWRX_STATUS_MAX);
        owrx_join(s, st);
        for (int i = 0; i < s->n_bands; i++) CHECK(s->bands[i].label <= strlen(s->bands[i].name));
        owrx_plus_version(st->version);
    }

    /* As audio. */
    if (rnd() % 8 == 0) owrx_adpcm_reset(d);
    else if (rnd() % 8 == 0) owrx_adpcm_resync(d);
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        size_t k = 1 + rnd() % 2048;
        if (k > n - i) k = n - i;
        o += owrx_adpcm_feed(d, p + i, k, pcm + o);
        i += k;
    }
    CHECK(o <= 2 * n);
    CHECK(d->idx >= 0 && d->idx <= 88);
    CHECK(d->plain == (d->st == 3 /* PLAIN */) && d->watch <= 2 * (1001 + 8));
    o = owrx_pcm_feed(d, p, n, pcm);
    CHECK(o <= n / 2 + 1);

    /* As an address, and a profile id sent back. */
    char text[256], out[600];
    const size_t m = n < sizeof text - 1 ? n : sizeof text - 1;
    memcpy(text, p, m);
    text[m] = 0;
    owrx_url_t u;
    if (owrx_url(text, &u)) {
        CHECK(u.path[0] == '/' && u.path[strlen(u.path) - 1] == '/');
        CHECK(!strstr(u.path, "//"));
        CHECK(u.port >= 1 && u.host[0]);
        owrx_path(&u, "status.json", out, 1 + rnd() % 80);
        owrx_key(&u);
    }
    const int k = owrx_select_cmd(out, 1 + rnd() % sizeof out, text);
    CHECK(k < (int)sizeof out);
    owrx_tune_t t = { (int64_t)(rnd() % 2000000000u), "", (int32_t)rnd(), (int32_t)rnd(), (int16_t)rnd() };
    static const char *MODE[] = { "usb", "lsb", "cw", "am", "sam", "nfm", "wfm", "x" };
    snprintf(t.mod, sizeof t.mod, "%s", MODE[rnd() % 8]);
    const int tn = owrx_tune_cmd(out, 1 + rnd() % sizeof out, &t, (int64_t)rnd() * 7, (int32_t)rnd(),
                                 rnd() & 1, rnd() & OWRX_P_ALL);
    CHECK(tn < (int)sizeof out);
    owrx_mode_of(text, (int64_t)rnd(), rnd() & 1);
    owrx_retune(s, &t);
    CHECK(owrx_mode_find(t.mod) != NULL);
    CHECK(!s->center || !s->rate || owrx_in_span(s, t.hz) || !owrx_in_span(s, s->center));
}

int main(int argc, char **argv)
{
    const unsigned long rounds = argc > 1 ? strtoul(argv[1], NULL, 10) : 1000000ul;
    if (argc > 2) rng_state = (uint32_t)strtoul(argv[2], NULL, 10) | 1u;
    static owrx_said_t s;
    static owrx_status_t st;
    owrx_adpcm_t d;
    owrx_said_init(&s);
    owrx_status_init(&st);
    owrx_adpcm_reset(&d);

    /* Every truncation of every real message. */
    for (size_t i = 0; i < sizeof MSGS / sizeof MSGS[0]; i++) {
        const size_t n = strlen(MSGS[i]);
        for (size_t k = 0; k <= n; k++) feed((const uint8_t *)MSGS[i], k, &s, &st, &d);
    }
    /* Nesting past every level the reader keeps, and past its limit. */
    for (int depth = 1; depth < 200; depth += 7) {
        size_t n = 0;
        n += (size_t)snprintf((char *)buf, sizeof buf, "{\"type\": \"profiles\", \"value\": ");
        for (int i = 0; i < depth && n < sizeof buf - 64; i++) buf[n++] = (uint8_t)(i & 1 ? '{' : '[');
        if (depth & 1) n += (size_t)snprintf((char *)buf + n, sizeof buf - n, "\"k\": 1");
        for (int i = depth - 1; i >= 0 && n < sizeof buf - 8; i--) buf[n++] = (uint8_t)(i & 1 ? '}' : ']');
        feed(buf, n, &s, &st, &d);
    }

    for (unsigned long r = 0; rounds == 0 || r < rounds; r++) {
        size_t n = 0;
        switch (rnd() % 4) {
        case 0:                                         /* random bytes */
            n = rnd() % 600;
            for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rnd();
            break;
        case 1: {                                       /* a real message, bytes changed */
            const char *m = MSGS[rnd() % (sizeof MSGS / sizeof MSGS[0])];
            n = strlen(m);
            memcpy(buf, m, n);
            for (int k = rnd() % 6; k >= 0 && n; k--) buf[rnd() % n] = (uint8_t)rnd();
            n = rnd() % 3 ? n : rnd() % (n + 1);
            break;
        }
        case 2: {                                       /* real messages and hard bits spliced */
            for (int k = 1 + rnd() % 12; k > 0; k--) {
                const char *m = rnd() % 3 ? BITS[rnd() % (sizeof BITS / sizeof BITS[0])]
                                          : MSGS[rnd() % (sizeof MSGS / sizeof MSGS[0])];
                size_t l = strlen(m);
                if (m[0] == 0) l = 1;                   /* the NUL byte itself */
                if (n + l >= sizeof buf) break;
                memcpy(buf + n, m, l);
                n += l;
            }
            break;
        }
        default: {                                      /* long: a string, an array, ADPCM */
            const int what = rnd() % 3;
            n = (size_t)snprintf((char *)buf, sizeof buf, what == 0 ? "{\"type\": \"receiver_details\", \"value\": {\"receiver_name\": \""
                                                        : what == 1 ? "{\"type\": \"profiles\", \"value\": [" : "SYNC");
            const size_t want = 1000 + rnd() % 60000;
            while (n < want) {
                if (what == 0) buf[n++] = rnd() % 5 ? (uint8_t)('a' + rnd() % 26) : (uint8_t)"\\u00e9"[rnd() % 6];
                else if (what == 1)
                    n += (size_t)snprintf((char *)buf + n, sizeof buf - n, "{\"id\": \"s|%u\", \"name\": \"n%u\"}, ",
                                          (unsigned)rnd(), (unsigned)rnd());
                else buf[n++] = (uint8_t)rnd();
            }
            if (n > sizeof buf - 8) n = sizeof buf - 8;
            break;
        }
        }
        feed(buf, n, &s, &st, &d);
        if (t_fail) break;
        if (t_run > 1000000000) t_run = 0;              /* an hour's run counts past an int */
        if (rounds == 0 && r % 1000000 == 0 && r) {
            printf("%lu rounds\n", r);
            fflush(stdout);
        }
    }
    printf("%s: %d checks, %d failed\n", __FILE__, t_run, t_fail);
    return t_fail ? 1 : 0;
}
