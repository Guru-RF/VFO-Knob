/* Deterministic byte fuzzer for PA3FWM's WebSDR protocol
 * (components/wsdr_proto).
 *
 * Everything a WebSDR sends reaches these functions -- its audio items, its
 * bandinfo.js, its websdr-sound.js -- from any address the operator types
 * in, and from a server that may be something else entirely. So they have to
 * survive arbitrary bytes, in messages of any length and pieces broken
 * anywhere. Run under ASan/UBSan (on by default) this catches overreads and
 * overflows; the fixed PRNG keeps a failure reproducible. A million rounds by
 * default: random bytes, mutated real items, every truncation.
 *
 *   fuzz_wsdr [ROUNDS [SEED]]     ROUNDS 0: until killed (an hour's run) */
#include "tiny.h"
#include "wsdr_proto.h"

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static const char *TEXTS[] = {
    "var nbands=2;\nvar ini_freq=3630.000000;\nvar ini_mode='lsb';\nvar bandinfo= [\n"
    "  { centerfreq: 1895.000000, samplerate: 192.000000, tuningstep: 0.031250, maxlinbw: 8.000000,\n"
    "    vfo: 1905.000000, maxzoom: 4, name: '160m', scaleimgs: [ [\"tmp/1-b0z0i0.png\"] ] }\n"
    ",  { centerfreq: 3660.000000, samplerate: 384.000000, name: \"80\\\"m\", scaleimgs: [] }\n"
    "];\nvar idletimeout=14400000;\n",
    "var freqbands=[];\nfreqbands.push( { min:3500.000000, max:3800.000000 } )\n// x { [\n/* ] } */\n",
    "x=new WebSocket(\"ws://\"+window.location.host+\"/~~stream?v=11\");",
    "<html><head><title>WebSDR &amp; &#39;x&#39; &nbsp; \xc3\xa9</title></head>",
};

static size_t blocks_seen;
static void on_audio(void *c, const int16_t *p, bool s)
{
    (void)c;
    (void)s;
    volatile int16_t x = p[0] ^ p[WSDR_BLOCK - 1];
    (void)x;
    blocks_seen++;
}
static void on_rate(void *c, unsigned hz) { (void)c; CHECK(hz <= 65535); }
static void on_meter(void *c, int v) { (void)c; CHECK(v >= 0 && v <= 4095); }
static void on_carrier(void *c, uint64_t mhz, int lock) { (void)c; CHECK(mhz < (1ull << 44) && lock >= 0 && lock <= 15); }
static const wsdr_cb_t CB = { on_audio, on_rate, on_meter, on_carrier, NULL, NULL };

static uint8_t buf[8192];

static size_t make(void)
{
    const uint32_t k = rnd() % 8;
    size_t n = rnd() % (k < 4 ? 400 : sizeof buf);
    if (k == 0) {
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rnd();
    } else if (k < 6) {
        /* Items as a server sends them, then mutated, cut anywhere. */
        size_t o = 0;
        while (o + 140 < sizeof buf && o < n) {
            switch (rnd() % 9) {
            case 0: buf[o++] = 0x81; buf[o++] = (uint8_t)rnd(); buf[o++] = (uint8_t)rnd(); break;
            case 1: buf[o++] = 0x82; buf[o++] = (uint8_t)(rnd() % 3); buf[o++] = (uint8_t)rnd(); break;
            case 2: buf[o++] = 0x83; buf[o++] = (uint8_t)rnd(); break;
            case 3: buf[o++] = (uint8_t)(0xF0 | rnd() % 16); buf[o++] = (uint8_t)rnd(); break;
            case 4: buf[o++] = 0x84; break;
            case 5: buf[o++] = 0x80; for (int i = 0; i < 128; i++) buf[o++] = (uint8_t)rnd(); break;
            default:
                buf[o++] = (uint8_t)(rnd() & 1 ? 0x90 + 16 * (rnd() % 5) + rnd() % 16 : rnd() % 128);
                for (int i = 0, m = (int)(rnd() % 130); i < m; i++)
                    buf[o++] = (uint8_t)(rnd() % 4 ? rnd() : rnd() % 2 ? 0 : 0xFF);
                break;
            }
        }
        n = o;
        for (int i = 0, m = (int)(rnd() % 4); i < m && n; i++) buf[rnd() % n] = (uint8_t)rnd();
        if (n && rnd() % 3 == 0) n = rnd() % n;
    } else {
        const char *t = TEXTS[rnd() % (sizeof TEXTS / sizeof TEXTS[0])];
        n = strlen(t);
        memcpy(buf, t, n);
        for (int i = 0, m = (int)(rnd() % 6); i < m; i++) buf[rnd() % n] = (uint8_t)(rnd() % 3 ? rnd() % 128 : rnd());
        if (rnd() % 4 == 0) n = rnd() % (n + 1);
    }
    return n;
}

int main(int argc, char **argv)
{
    long rounds = argc > 1 ? atol(argv[1]) : 1000000;
    if (argc > 2) rng_state = (uint32_t)strtoul(argv[2], NULL, 0);
    static wsdr_dec_t d;
    static wsdr_info_t in;
    wsdr_dec_reset(&d);
    for (long r = 0; rounds == 0 || r < rounds; r++) {
        const size_t n = make();
        if (rnd() % 16 == 0) wsdr_dec_reset(&d);
        /* As audio: the message whole. */
        wsdr_items(&d, buf, n, &CB);
        CHECK(d.width >= 1 && d.width <= 5);
        /* As bandinfo.js and websdr-sound.js, in pieces. */
        wsdr_info_rd_t rd;
        wsdr_info_begin(&rd, &in);
        wsdr_v11_t v = { 0 };
        wsdr_title_t ti = { 0 };
        for (size_t i = 0; i < n;) {
            size_t k = 1 + rnd() % 300;
            if (k > n - i) k = n - i;
            wsdr_info_feed(&rd, buf + i, k);
            wsdr_v11_feed(&v, buf + i, k);
            wsdr_title_feed(&ti, buf + i, k);
            i += k;
        }
        wsdr_info_end(&rd);
        CHECK(strlen(wsdr_title_end(&ti)) < sizeof ti.title);
        CHECK(in.n_bands >= 0 && in.n_bands <= WSDR_BANDS && in.n_plan >= 0 && in.n_plan <= WSDR_PLAN);
        for (int i = 0; i < in.n_bands; i++) {
            CHECK(in.band[i].span_hz > 0 && in.band[i].step_hz > 0 && in.band[i].maxbw_hz > 0);
            CHECK(memchr(in.band[i].name, 0, sizeof in.band[i].name) != NULL);
            /* ...and every band's tuning written, whatever its numbers. */
            wsdr_tune_t t = { .dial_hz = in.band[i].vfo_hz, .band = i, .mode = (uint8_t)(rnd() % 5),
                              .cw = rnd() & 1, .lo = (int32_t)(rnd() % 40000) - 20000,
                              .hi = (int32_t)(rnd() % 40000) - 20000 };
            wsdr_clamp_pass(&in.band[i], &t);
            CHECK(t.lo <= t.hi);
            char cmd[200];
            const int len = wsdr_param_cmd(cmd, sizeof cmd, &in.band[i], &t, in.band[i].name);
            CHECK(len < 0 || (size_t)len == strlen(cmd));
            wsdr_band_of(&in, (int64_t)(rnd() % 40000000), (int)(rnd() % 20) - 2);
        }
        CHECK(memchr(in.ini_mode, 0, sizeof in.ini_mode) != NULL);
        if (t_fail) {
            fprintf(stderr, "round %ld\n", r);
            break;
        }
    }
    printf("%s: %d checks, %d failed, %zu blocks decoded\n", __FILE__, t_run, t_fail, blocks_seen);
    return t_fail ? 1 : 0;
}
