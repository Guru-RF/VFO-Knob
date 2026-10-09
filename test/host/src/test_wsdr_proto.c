/* PA3FWM's WebSDR protocol, the plain-C part (components/wsdr_proto): its
 * audio items, bandinfo.js read in pieces broken at every byte, the stream
 * path, the modes and the tuning the knob sends.
 *
 *   test_wsdr_proto                          the cases below
 *   test_wsdr_proto STREAM EXPECT            ...and tools/wsdr_codec.py's
 *                                            vectors: every sample */
#include "tiny.h"
#include "wsdr_proto.h"

/* ------------------------------------------------------------- the audio */

typedef struct {
    int16_t  pcm[1 << 16];
    size_t   n;
    int      silent, blocks;
    unsigned rate[8];
    int      n_rate;
    int      meter[8];
    int      n_meter;
    uint64_t carrier;
    int      lock, resyncs;
} got_t;
static got_t G;

static void on_audio(void *c, const int16_t *p, bool silent)
{
    got_t *g = c;
    if (g->n + WSDR_BLOCK <= sizeof g->pcm / sizeof g->pcm[0]) memcpy(g->pcm + g->n, p, WSDR_BLOCK * 2);
    g->n += WSDR_BLOCK;
    g->blocks++;
    g->silent += silent;
}
static void on_rate(void *c, unsigned hz) { got_t *g = c; if (g->n_rate < 8) g->rate[g->n_rate++] = hz; }
static void on_meter(void *c, int v) { got_t *g = c; if (g->n_meter < 8) g->meter[g->n_meter++] = v; }
static void on_carrier(void *c, uint64_t mhz, int lock) { got_t *g = c; g->carrier = mhz; g->lock = lock; }
static void on_resync(void *c) { ((got_t *)c)->resyncs++; }
static const wsdr_cb_t CB = { on_audio, on_rate, on_meter, on_carrier, on_resync, &G };

static void test_items(void)
{
    CASE("items");
    wsdr_dec_t d;
    wsdr_dec_reset(&d);
    memset(&G, 0, sizeof G);
    /* The opening a server sends: S-meter, rate, step, conversion. */
    const uint8_t head[] = { 0xF2, 0x1C, 0x81, 0x1B, 0xCF, 0x82, 0x00, 0x28, 0x83, 0x10, 0xE5, 0x8A };
    CHECK_EQ(wsdr_items(&d, head, sizeof head, &CB), 0);
    CHECK_EQ(G.n_meter, 1);
    CHECK_EQ(G.meter[0], 0x21C);
    CHECK_EQ(G.n_rate, 1);
    CHECK_EQ(G.rate[0], 7119);
    CHECK_EQ(d.step, 40);
    CHECK_EQ(d.conv, 0x10);
    /* The S-meter as the page shows it: dBm = v / 10 - 127, and S-units on
     * its scale: S9 at -73, S1 at -121, dB over S9 above. */
    CHECK_EQ(wsdr_dbm10(0x21C), 540 - 1270);
    CHECK_EQ(wsdr_s10(-730), 90);
    CHECK_EQ(wsdr_s10(-1210), 10);
    CHECK_EQ(wsdr_s10(-1270), 0);
    CHECK_EQ(wsdr_s10(-790), 80);
    CHECK_EQ(wsdr_s10(-530), 110);
    /* Busy: rate 0. */
    const uint8_t busy[] = { 0x81, 0x00, 0x00 };
    wsdr_items(&d, busy, sizeof busy, &CB);
    CHECK_EQ(G.n_rate, 2);
    CHECK_EQ(G.rate[1], 0);

    /* Silence: a block, flagged, and the predictor forgotten. */
    d.tap[3] = 77;
    d.hist2[5] = 99;
    d.integ = 5;
    const uint8_t sil[] = { 0x84, 0x84 };
    CHECK_EQ(wsdr_items(&d, sil, sizeof sil, &CB), 2);
    CHECK_EQ(G.silent, 2);
    CHECK(!d.tap[3] && !d.hist2[5] && !d.integ);

    /* A-law: G.711's, and the predictor forgotten too. */
    memset(&G, 0, sizeof G);
    uint8_t al[1 + 128];
    al[0] = 0x80;
    for (int i = 0; i < 128; i++) al[1 + i] = (uint8_t)(i * 2);
    al[1 + 0] = 0xD5;
    al[1 + 1] = 0x55;
    al[1 + 2] = 0x80;
    al[1 + 3] = 0x00;
    al[1 + 4] = 0xAA;
    al[1 + 5] = 0x2A;
    d.tap[0] = 1;
    CHECK_EQ(wsdr_items(&d, al, sizeof al, &CB), 1);
    CHECK_EQ(G.pcm[0], 8);
    CHECK_EQ(G.pcm[1], -8);
    CHECK_EQ(G.pcm[2], 5504);
    CHECK_EQ(G.pcm[3], -5504);
    CHECK_EQ(G.pcm[4], 32256);
    CHECK_EQ(G.pcm[5], -32256);
    CHECK(!d.tap[0]);
    /* ...cut short: the rest zeros, as the page reads past a message. */
    memset(&G, 0, sizeof G);
    CHECK_EQ(wsdr_items(&d, al, 3, &CB), 1);
    CHECK(G.pcm[0] == 8 && G.pcm[1] == -8 && G.pcm[2] == 0 && G.pcm[127] == 0);

    /* AM sync's carrier: lock state and mHz. */
    const uint8_t car[] = { 0x85, 0x13, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0x86, 0x87, 1, 2, 3, 4, 5, 6 };
    wsdr_items(&d, car, sizeof car, &CB);
    CHECK_EQ(G.lock, 1);
    CHECK(G.carrier == ((uint64_t)0x3 << 40 | (uint64_t)0x45 << 32 | 0x6789ABCDu));
    CHECK_EQ(G.resyncs, 1);

    /* A coded block: a width tag, then 128 samples of the smallest code at
     * width 5 -- quotient 0 ('1'), mantissa 0000, sign 0: six bits each --
     * at step 40 with no integrator: the first sample half a step, 20. */
    wsdr_dec_reset(&d);
    memset(&G, 0, sizeof G);
    uint8_t blk[1 + 96 + 4];
    memset(blk, 0, sizeof blk);
    const uint8_t setup[] = { 0x82, 0x00, 0x28, 0x83, 0x10 };
    wsdr_items(&d, setup, sizeof setup, &CB);
    /* 128 x "100000" = 768 bits; the tag's low nibble carries the first 4. */
    uint8_t bits[800 / 8 + 1] = { 0 };
    for (int s = 0; s < 128; s++) bits[(s * 6) / 8] |= (uint8_t)(0x80 >> ((s * 6) % 8));
    blk[0] = (uint8_t)(0x90 | bits[0] >> 4);
    for (int i = 0; i < 96; i++) blk[1 + i] = (uint8_t)(bits[i] << 4 | bits[i + 1] >> 4);
    blk[97] = 0x84;                             /* the next item, right after the block */
    CHECK_EQ(wsdr_items(&d, blk, 98, &CB), 2);
    CHECK_EQ(d.width, 5);
    CHECK_EQ(G.pcm[0], 20);
    CHECK_EQ(G.silent, 1);                      /* the block ended where it should */
    CHECK_EQ(G.blocks, 2);

    /* Bytes the page passes by: 0x88-0x8F, 0xE0-0xEF. */
    memset(&G, 0, sizeof G);
    const uint8_t junk[] = { 0x88, 0x8F, 0xE0, 0xEF, 0x84 };
    CHECK_EQ(wsdr_items(&d, junk, sizeof junk, &CB), 1);
    CHECK_EQ(G.silent, 1);
}

/* tools/wsdr_codec.py's vectors: its encoder's mirror against the knob's
 * decoder, every message whole. */
static int vectors(const char *sp, const char *ep)
{
    CASE("vectors");
    FILE *f = fopen(sp, "rb"), *g = fopen(ep, "rb");
    if (!f || !g) {
        fprintf(stderr, "cannot open %s or %s\n", sp, ep);
        return 1;
    }
    static int16_t want[1 << 20];
    const size_t m = fread(want, 2, sizeof want / 2, g);
    fclose(g);
    wsdr_dec_t d;
    wsdr_dec_reset(&d);
    static int16_t all[1 << 20];
    size_t o = 0, msgs = 0, bytes = 0;
    static uint8_t buf[1 << 16];
    uint8_t h[4];
    while (fread(h, 1, 4, f) == 4) {
        const size_t n = h[0] | h[1] << 8 | h[2] << 16 | (size_t)h[3] << 24;
        if (n > sizeof buf || fread(buf, 1, n, f) != n) {
            CHECK(false);
            break;
        }
        memset(&G, 0, sizeof G);
        wsdr_items(&d, buf, n, &CB);
        if (o + G.n <= sizeof all / sizeof all[0]) memcpy(all + o, G.pcm, G.n * 2);
        o += G.n;
        msgs++;
        bytes += n;
    }
    fclose(f);
    CHECK_EQ(o, m);
    size_t at = 0;
    while (at < m && at < o && all[at] == want[at]) at++;
    CHECK_EQ(at, m);
    printf("vectors: %zu messages, %zu bytes, %zu samples, identical to %zu\n", msgs, bytes, o, at);
    return 0;
}

/* ------------------------------------------------------------ the bands */

/* bandinfo.js as the server writes it: one site of eight bands, one of a
 * single wide band with its band plan -- the shape of Maasbree's and
 * Twente's, numbers of this test's own. */
static const char MULTI[] =
    "var nbands=3;\n"
    "var ini_freq=3630.000000;\n"
    "var ini_mode='lsb';\n"
    "var chseq=2;\n"
    "var bandinfo= [\n"
    "  { centerfreq: 1895.000000,\n"
    "    samplerate: 192.000000,\n"
    "    tuningstep: 0.031250,\n"
    "    maxlinbw: 8.000000,\n"
    "    vfo: 1905.000000,\n"
    "    maxzoom: 4,\n"
    "    name: '160m',\n"
    "    scaleimgs: [\n"
    "      [\"tmp/1-b0z0i0.png\"],\n"
    "      [\"tmp/1-b0z1i0.png\",\"tmp/1-b0z1i1.png\"]\n"
    "    ]\n"
    "  }\n"
    ",  { centerfreq: 3660.000000,\n"
    "    samplerate: 384.000000,\n"
    "    tuningstep: 0.031250,\n"
    "    maxlinbw: 8.000000,\n"
    "    vfo: 3670.000000,\n"
    "    maxzoom: 5,\n"
    "    name: '80m',\n"
    "    scaleimgs: [ [\"tmp/{x}]',y.png\"] ]\n"
    "  }\n"
    ",  { centerfreq: 7100.000000, samplerate: 384.000000, tuningstep: 0.031250, maxlinbw: 8.000000,\n"
    "    vfo: 9999.000000, name: \"40\\\"m\", scaleimgs: [] }\n"
    "];\n"
    "var dxinfoavailable=0;\n"
    "var idletimeout=14400000;\n";

static const char WIDE[] =
    "var nbands=1;\n"
    "var ini_freq=-1.000000;\n"
    "var ini_mode='';\n"
    "var mw9kHzsteps=1;\n"
    "var bandinfo= [\n"
    "  { centerfreq: 14579.800, samplerate: 29159.600000, tuningstep: 0.006952,\n"
    "    maxlinbw: 7.119043, vfo: 14589.800000, maxzoom: 10, name: 'hf',\n"
    "    scaleimgs: [ [\"a\"], [\"b\",\"c\"] ] }\n"
    "];\n"
    "var freqbands=[];\n"
    "freqbands.push( { min:3500.000000, max:3800.000000 } )\n"
    "freqbands.push( { min:7000.000000, max:7200.000000 } )\n"
    "// a comment { [ (\n"
    "/* and another ] } */\n"
    "freqbands.push( { min:14000.000000, max:14350.000000 } )\n"
    "var idletimeout=0;\n";

static void read_info(const char *t, size_t piece, wsdr_info_t *out)
{
    wsdr_info_rd_t r;
    wsdr_info_begin(&r, out);
    const size_t n = strlen(t);
    for (size_t i = 0; i < n; i += piece)
        wsdr_info_feed(&r, (const uint8_t *)t + i, n - i < piece ? n - i : piece);
    wsdr_info_end(&r);
}

static void test_bands(void)
{
    CASE("bandinfo");
    static wsdr_info_t in;
    for (size_t piece = 1; piece <= sizeof MULTI; piece++) {
        read_info(MULTI, piece, &in);
        if (in.n_bands != 3) {
            CHECK_EQ(in.n_bands, 3);
            fprintf(stderr, "  in pieces of %zu\n", piece);
            break;
        }
    }
    CHECK_EQ(in.nbands, 3);
    CHECK_EQ(in.n_bands, 3);
    CHECK_EQ(in.ini_hz, 3630000);
    CHECK_STR(in.ini_mode, "lsb");
    CHECK_EQ(in.idle_ms, 14400000);
    CHECK_EQ(in.band[0].center_hz, 1895000);
    CHECK_EQ(in.band[0].span_hz, 192000);
    CHECK_EQ(in.band[0].vfo_hz, 1905000);
    CHECK(in.band[0].step_hz > 31.249 && in.band[0].step_hz < 31.251);
    CHECK_EQ(in.band[0].maxbw_hz, 8000);
    CHECK_STR(in.band[0].name, "160m");
    CHECK_STR(in.band[1].name, "80m");
    CHECK_STR(in.band[2].name, "40\"m");
    CHECK_EQ(in.band[2].vfo_hz, 7100000);       /* outside its band: its centre */
    CHECK_EQ(in.n_plan, 0);

    for (size_t piece = 1; piece <= sizeof WIDE; piece += piece < 40 ? 1 : 37) read_info(WIDE, piece, &in);
    CHECK_EQ(in.n_bands, 1);
    CHECK_EQ(in.ini_hz, -1);
    CHECK_STR(in.ini_mode, "");
    CHECK_EQ(in.idle_ms, 0);
    CHECK_EQ(in.band[0].center_hz, 14579800);
    CHECK_EQ(in.band[0].span_hz, 29159600);
    CHECK_EQ(in.band[0].maxbw_hz, 7119);
    CHECK(in.band[0].step_hz > 6.951 && in.band[0].step_hz < 6.953);
    CHECK_EQ(in.n_plan, 3);
    CHECK_EQ(in.plan[0].lo_hz, 3500000);
    CHECK_EQ(in.plan[2].hi_hz, 14350000);

    /* More bands than kept: the first WSDR_BANDS. */
    static char many[16384];
    size_t o = (size_t)snprintf(many, sizeof many, "var nbands=20;\nvar bandinfo= [\n");
    for (int i = 0; i < 20; i++)
        o += (size_t)snprintf(many + o, sizeof many - o, "%s{ centerfreq: %d.0, samplerate: 192.0, name: 'b%d' }\n",
                              i ? "," : "", 1000 + 500 * i, i);
    snprintf(many + o, sizeof many - o, "];\n");
    read_info(many, 7, &in);
    CHECK_EQ(in.nbands, 20);
    CHECK_EQ(in.n_bands, WSDR_BANDS);
    CHECK_STR(in.band[WSDR_BANDS - 1].name, "b15");

    /* Numbers past all reason: kept within bounds, nothing overflows. */
    read_info("var bandinfo= [ { centerfreq: -1e300, samplerate: 1e300, tuningstep: inf, maxlinbw: nan,\n"
              "vfo: 1e999, name: 'x' } ];\nvar ini_freq=1e400;\nvar idletimeout=-5;\n", 5, &in);
    CHECK_EQ(in.n_bands, 1);
    CHECK(in.band[0].center_hz == -1000000000000LL && in.band[0].span_hz == 1000000000000LL);
    CHECK(in.band[0].step_hz == 1 && in.band[0].maxbw_hz == 4000);
    CHECK_EQ(wsdr_band_of(&in, INT64_C(-1000000000000), -1), 0);
    CHECK_EQ(wsdr_band_of(&in, INT64_C(1000000000000), 0), -1);
    CHECK_EQ(in.idle_ms, 0);

    /* Not a bandinfo.js at all. */
    wsdr_info_rd_t r;
    wsdr_info_begin(&r, &in);
    wsdr_info_feed(&r, (const uint8_t *)"<html><body>404</body></html>", 29);
    CHECK(!wsdr_info_end(&r));

    CASE("range names");
    static const struct { int lo, hi; const char *name; bool ham; } RN[] = {
        { 3500, 3800, "80 m", true }, { 7000, 7200, "40 m", true }, { 5351, 5366, "60 m", true },
        { 26960, 27410, "CB", false }, { 5900, 6200, "49 m BC", false }, { 7200, 7450, "41 m BC", false },
        { 526, 1606, "MW", false }, { 148, 283, "LW", false }, { 135, 137, "2200 m", true },
        { 28000, 29700, "10 m", true }, { 50000, 52000, "50.0 MHz", false },
    };
    for (size_t i = 0; i < sizeof RN / sizeof RN[0]; i++) {
        const wsdr_range_t r = { RN[i].lo * 1000LL, RN[i].hi * 1000LL };
        char nm[24];
        bool ham = !RN[i].ham;
        wsdr_range_name(&r, nm, sizeof nm, &ham);
        CHECK_STR(nm, RN[i].name);
        CHECK(ham == RN[i].ham);
    }

    CASE("band_of");
    read_info(MULTI, 64, &in);
    CHECK_EQ(wsdr_band_of(&in, 3700000, -1), 1);
    CHECK_EQ(wsdr_band_of(&in, 3660000 + 192000, -1), 1);        /* its edge */
    CHECK_EQ(wsdr_band_of(&in, 3660000 + 192000 + 3000, -1), 1); /* within 4 kHz of it */
    CHECK_EQ(wsdr_band_of(&in, 3660000 + 192000 + 5000, -1), -1);
    CHECK_EQ(wsdr_band_of(&in, 14074000, -1), -1);
    CHECK_EQ(wsdr_band_of(&in, 7000000, 0), 2);
    /* Two that overlap: the one in use kept. */
    in.band[2].center_hz = 3700000;
    CHECK_EQ(wsdr_band_of(&in, 3700000, 2), 2);
    CHECK_EQ(wsdr_band_of(&in, 3700000, -1), 1);
}

/* ------------------------------------------------- the stream path */

static void test_v11(void)
{
    CASE("v11");
    static const char OLD[] = "x=new WebSocket(\"ws://\"+window.location.host+\"/~~stream?v=11\");";
    static const char NEW[] = "x=new WebSocket(\"ws://\"+window.location.host+\"/~~stream\");";
    for (size_t piece = 1; piece <= sizeof OLD; piece++) {
        wsdr_v11_t a = { 0 }, b = { 0 };
        for (size_t i = 0; i < strlen(OLD); i += piece)
            wsdr_v11_feed(&a, (const uint8_t *)OLD + i, strlen(OLD) - i < piece ? strlen(OLD) - i : piece);
        for (size_t i = 0; i < strlen(NEW); i += piece)
            wsdr_v11_feed(&b, (const uint8_t *)NEW + i, strlen(NEW) - i < piece ? strlen(NEW) - i : piece);
        if (!a.v11 || b.v11) {
            CHECK(a.v11 && !b.v11);
            break;
        }
    }
    wsdr_v11_t c = { 0 };
    wsdr_v11_feed(&c, (const uint8_t *)"/~~~stream?v=11", 15);
    CHECK(c.v11);
    wsdr_v11_t e = { 0 };
    wsdr_v11_feed(&e, (const uint8_t *)"~~stream?v=12 ~~stream?v=1", 26);
    CHECK(!e.v11);
}

/* ---------------------------------------------------------- the tuning */

static void test_tuning(void)
{
    CASE("modes");
    CHECK_STR(wsdr_mode(0)->name, "usb");
    CHECK(wsdr_mode_named("lsb")->lo == -2700 && wsdr_mode_named("lsb")->hi == -300);
    CHECK_EQ(wsdr_mode_named("cw")->mode, WSDR_M_SSB);
    CHECK_EQ(wsdr_mode_named("am")->mode, WSDR_M_AM);
    CHECK_EQ(wsdr_mode_named("sam")->mode, WSDR_M_AMSYNC);
    CHECK_EQ(wsdr_mode_named("nfm")->mode, WSDR_M_FM);
    CHECK(!wsdr_mode_named("wfm") && !wsdr_mode_named(NULL));
    int n = 0;
    while (wsdr_mode(n)) n++;
    CHECK_EQ(n, 6);

    CASE("param");
    wsdr_band_t mb = { .center_hz = 7100000, .span_hz = 384000, .step_hz = 31.25, .maxbw_hz = 8000 };
    wsdr_tune_t t = { .dial_hz = 7074000, .band = 3, .mode = WSDR_M_SSB, .lo = 300, .hi = 2700 };
    char cmd[200];
    CHECK(wsdr_param_cmd(cmd, sizeof cmd, &mb, &t, "ON6URE") > 0);
    CHECK_STR(cmd, "GET /~~param?f=7074.000&band=3&lo=0.3&hi=2.7&mode=0&name=ON6URE");
    /* CW: the station on the dial, its tone 750 Hz below the carrier. */
    t.cw = true;
    t.lo = -950;
    t.hi = -550;
    t.dial_hz = 7030000;
    wsdr_param_cmd(cmd, sizeof cmd, &mb, &t, "");
    CHECK_STR(cmd, "GET /~~param?f=7030.750&band=3&lo=-0.95&hi=-0.55&mode=0&name=");
    /* The carrier on the band's step: 7074010 is 31.25 Hz steps from 0. */
    t.cw = false;
    t.dial_hz = 7074010;
    CHECK_EQ(wsdr_carrier_hz(&mb, &t), 7074000);
    t.dial_hz = 7074020;
    CHECK_EQ(wsdr_carrier_hz(&mb, &t), 7074031);
    /* AM, a name to encode. */
    t.mode = WSDR_M_AM;
    t.lo = -4500;
    t.hi = 4500;
    t.dial_hz = 1000000;
    t.band = 0;
    wsdr_param_cmd(cmd, sizeof cmd, &mb, &t, "ON6URE/P knob");
    CHECK_STR(cmd, "GET /~~param?f=1000.000&band=0&lo=-4.5&hi=4.5&mode=1&name=ON6URE%2FP%20knob");
    CHECK_EQ(wsdr_param_cmd(cmd, 20, &mb, &t, "x"), -1);

    CASE("clamp");
    wsdr_band_t tw = { .maxbw_hz = 7119, .step_hz = 6.952 };
    wsdr_tune_t w = { .mode = WSDR_M_AM, .lo = -9000, .hi = 9000 };
    wsdr_clamp_pass(&tw, &w);
    CHECK(w.lo == -6763 && w.hi == 6763);
    w = (wsdr_tune_t){ .mode = WSDR_M_FM, .lo = -20000, .hi = 20000 };
    wsdr_clamp_pass(&tw, &w);
    CHECK(w.lo == -15000 && w.hi == 15000);
    w = (wsdr_tune_t){ .mode = WSDR_M_SSB, .lo = 2700, .hi = 300 };
    wsdr_clamp_pass(&tw, &w);
    CHECK(w.lo == 300 && w.hi == 2700);

    CASE("flags");
    CHECK(wsdr_flag_cmd(cmd, sizeof cmd, "mute", true) > 0);
    CHECK_STR(cmd, "GET /~~param?mute=1");
    wsdr_flag_cmd(cmd, sizeof cmd, "autonotch", false);
    CHECK_STR(cmd, "GET /~~param?autonotch=0");
}

/* ------------------------------------------------- ends, the site's name */

static void test_ends(void)
{
    CASE("ends");
    CHECK_STR(wsdr_end_word(WSDR_END_BUSY), "BUSY");
    CHECK_STR(wsdr_end_word(WSDR_END_NOT_WSDR), "NOT A WEBSDR");
    CHECK_STR(wsdr_end_word((wsdr_end_t)99), "NO ANSWER");
    CHECK_EQ(wsdr_http(101), WSDR_END_NONE);
    CHECK_EQ(wsdr_http(403), WSDR_END_REFUSED);
    CHECK_EQ(wsdr_http(404), WSDR_END_NOT_WSDR);
    CHECK_EQ(wsdr_http(503), WSDR_END_DOWN);
    CHECK_EQ(wsdr_http(302), WSDR_END_MOVED);
    CHECK_EQ(wsdr_http(0), WSDR_END_NO_ANSWER);
    CHECK_EQ(wsdr_retry_ms(WSDR_END_BUSY, 1), 300000);
    CHECK_EQ(wsdr_retry_ms(WSDR_END_IDLE, 1), 0);
    CHECK_EQ(wsdr_retry_ms(WSDR_END_NOT_WSDR, 3), 0);
    CHECK_EQ(wsdr_retry_ms(WSDR_END_NO_ROUTE, 1), 2000);
    CHECK_EQ(wsdr_retry_ms(WSDR_END_NO_ROUTE, 4), 16000);
    CHECK_EQ(wsdr_retry_ms(WSDR_END_NO_ROUTE, 40), 512000);

    CASE("title");
    static const char PAGE[] = "<!DOCTYPE html>\n<html><head><meta charset=utf-8>\n<TITLE lang=en>\n  Wide-band  WebSDR "
                               "&amp; Enschede &#39;NL&#39;\n</title><title>second</title></head>";
    for (size_t piece = 1; piece <= sizeof PAGE; piece++) {
        wsdr_title_t t = { 0 };
        for (size_t i = 0; i < strlen(PAGE); i += piece)
            wsdr_title_feed(&t, (const uint8_t *)PAGE + i, strlen(PAGE) - i < piece ? strlen(PAGE) - i : piece);
        if (strcmp(wsdr_title_end(&t), "Wide-band WebSDR & Enschede 'NL'")) {
            CHECK_STR(t.title, "Wide-band WebSDR & Enschede 'NL'");
            break;
        }
    }
    wsdr_title_t u = { 0 };
    wsdr_title_feed(&u, (const uint8_t *)"<html><body>no title</body>", 27);
    CHECK_STR(wsdr_title_end(&u), "");
    /* Cut at its room, a character never halved. */
    char longp[200];
    snprintf(longp, sizeof longp, "<title>%s</title>",
             "WebSDR \xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"
             "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"
             "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9!");
    wsdr_title_t v = { 0 };
    wsdr_title_feed(&v, (const uint8_t *)longp, strlen(longp));
    const char *tt = wsdr_title_end(&v);
    CHECK(strlen(tt) <= 63 && strlen(tt) >= 61);
    CHECK(((uint8_t)tt[strlen(tt) - 1] & 0xC0) == 0x80);        /* ends on a whole 'é' */
}

int main(int argc, char **argv)
{
    test_items();
    test_bands();
    test_v11();
    test_tuning();
    test_ends();
    if (argc == 3 && vectors(argv[1], argv[2])) return 1;
    printf("%s: %d checks, %d failed\n", __FILE__, t_run, t_fail);
    return t_fail ? 1 : 0;
}
