/* TailChaser's callsign tag (components/vfo_core/src/tag.c): the format against
 * TailChaser's own vectors.json (tag_vectors.h), and the decoder on tags made
 * here -- clean, under noise, through clocks that differ, after loud bursts
 * like speech, one talker after another -- and on noise alone, where it must
 * never read anything. With TAG_WAV_DIR naming the folder of TailChaser's
 * recordings (they are not in the repository), those too, the one off the air
 * among them. */
#include "tiny.h"
#include "tag.h"
#include "tag_hist.h"
#include "tag_vectors.h"
#include "wav_load.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- making a tag ------------------------------------------------------------------ */

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static double urand(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (double)(rng >> 11) / (double)(1ull << 53);
}
static double gauss(void)
{
    const double u = urand() + 1e-300, v = urand();
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

/* n samples of the tag at 16 kHz with RMS rms, from sample `from` of its frames;
 * `ppm`: the repeater's clock against the decoder's. Shaped as the repeater
 * shapes it (a low-pass at TAG_SHAPE_HZ at baseband), as a ring. */
static void tag_wave(float *out, size_t n, const char *call, bool local, double from, double ppm, double rms)
{
    enum { TAPS = 129, PER = TAG_FRAME_CHIPS * (TAG_FS / TAG_CHIP_RATE) };
    static int8_t chips[TAG_FRAME_CHIPS];
    static bool have;
    if (!have) {
        for (int i = 0; i < TAG_FRAME_CHIPS; i++) chips[i] = (int8_t)tag_chip(i);
        have = true;
    }
    int8_t sg[TAG_NSYM];
    CHECK(tag_frame_signs(call, local, sg));
    float g[TAPS];
    double gs = 0;
    for (int j = 0; j < TAPS; j++) {
        const double t = j - (TAPS - 1) / 2.0, wc = 2 * TAG_SHAPE_HZ / TAG_FS;
        const double s = t == 0 ? wc : sin(M_PI * wc * t) / (M_PI * t);
        const double w = 0.42 - 0.5 * cos(2 * M_PI * j / (TAPS - 1)) + 0.08 * cos(4 * M_PI * j / (TAPS - 1));
        g[j] = (float)(s * w);
        gs += s * w;
    }
    /* One frame, shaped as a ring, then read out at the repeater's clock. */
    static float frame[PER];
    for (int i = 0; i < PER; i++) {
        double acc = 0;
        for (int j = 0; j < TAPS; j++) {
            const int at = ((i - j + (TAPS - 1) / 2) % PER + PER) % PER;
            const int c = at / (TAG_FS / TAG_CHIP_RATE);
            acc += g[j] / gs * chips[c] * sg[c / TAG_CPS];
        }
        frame[i] = (float)(acc * cos(2 * M_PI * TAG_FC_HZ * i / TAG_FS));
    }
    double p = 0;
    for (int i = 0; i < PER; i++) p += (double)frame[i] * frame[i];
    const double k = rms / sqrt(p / PER);
    for (size_t i = 0; i < n; i++) {
        double t = fmod(from + (double)i * (1 + ppm * 1e-6), PER);
        const int a = (int)t;
        const double f = t - a;
        out[i] = (float)(k * ((1 - f) * frame[a] + f * frame[(a + 1) % PER]));
    }
}

typedef struct {
    int n;
    tag_reading_t r[64];
    double first_s;
} heard_t;

/* Feed x to a fresh decoder in 20 ms pieces, as the knob does. */
static void run(tag_dec_t *d, const float *x, size_t n, heard_t *h)
{
    tag_dec_init(d);
    memset(h, 0, sizeof *h);
    h->first_s = -1;
    for (size_t at = 0; at < n; at += 320) {
        const size_t k = n - at < 320 ? n - at : 320;
        tag_dec_feed_f(d, x + at, k);
        tag_reading_t rd;
        while (tag_dec_take(d, &rd)) {
            if (h->first_s < 0) h->first_s = (double)rd.at / TAG_FS;
            if (h->n < 64) h->r[h->n++] = rd;
        }
    }
}

/* Every reading is this one, and there is at least one. */
static bool only(const heard_t *h, const char *call, bool local)
{
    if (!h->n) return false;
    for (int i = 0; i < h->n; i++)
        if (strcmp(h->r[i].call, call) || h->r[i].local != local) return false;
    return true;
}

static void show(const char *what, const tag_dec_t *d, const heard_t *h)
{
    tag_stats_t st;
    tag_dec_stats(d, &st);
    printf("  %-44s ", what);
    if (h->n)
        printf("%s %s first at %.2f s (sync %.1f dB, %u corrected, %u frame%s), %d readings",
               h->r[0].call, h->r[0].local ? "local" : "network", h->first_s, h->r[0].sync_db,
               h->r[0].corrected, h->r[0].frames, h->r[0].frames > 1 ? "s" : "", h->n);
    else
        printf("nothing");
    printf("; tries %u, CRC bad %u, sync now %.1f dB, band %.0f dBFS\n",
           st.tries, st.crc_bad, st.sync_db, st.band_dbfs);
}

/* --- TailChaser's recordings ------------------------------------------------------ */

/* --- the tests ------------------------------------------------------------------- */

static void test_format(void)
{
    CASE("the chips, the sync bits, the carrier, the alphabet, the interleaver");
    int chips_ok = 0;
    for (int i = 0; i < TAG_FRAME_CHIPS; i++) chips_ok += tag_chip(i) == (TV_CHIPS[i] == '+' ? 1 : -1);
    CHECK_EQ(chips_ok, TAG_FRAME_CHIPS);
    CHECK_EQ(strlen(TV_CHIPS), TAG_FRAME_CHIPS);
    for (int k = 1; k < TAG_NSYNC; k++) CHECK_EQ((TAG_SYNC_WORD >> k) & 1, TV_SYNC[k] - '0');
    CHECK(fabs(TAG_FC_HZ - TV_FC) < 1e-6);
    CHECK_STR(TAG_ALPHABET, TV_ALPHABET);
    for (int i = 0; i < TAG_NBITS; i++) CHECK_EQ(tag_perm(i), TV_PERM[i]);

    CASE("three messages: the bits, the code, the symbols' signs, and back");
    for (size_t m = 0; m < sizeof TV_MSG / sizeof TV_MSG[0]; m++) {
        uint8_t p[TAG_PAYLOAD_BITS], c[TAG_NBITS];
        int8_t sg[TAG_NSYM];
        char s[TAG_NBITS + 1];
        CHECK(tag_pack(TV_MSG[m].call, TV_MSG[m].local, p));
        for (int i = 0; i < TAG_PAYLOAD_BITS; i++) s[i] = (char)('0' + p[i]);
        s[TAG_PAYLOAD_BITS] = 0;
        CHECK_STR(s, TV_MSG[m].payload);
        tag_conv(p, c);
        for (int i = 0; i < TAG_NBITS; i++) s[i] = (char)('0' + c[i]);
        s[TAG_NBITS] = 0;
        CHECK_STR(s, TV_MSG[m].coded);
        CHECK(tag_frame_signs(TV_MSG[m].call, TV_MSG[m].local, sg));
        char sgs[TAG_NSYM + 1];
        for (int i = 0; i < TAG_NSYM; i++) sgs[i] = sg[i] > 0 ? '+' : '-';
        sgs[TAG_NSYM] = 0;
        CHECK_STR(sgs, TV_MSG[m].signs);

        /* through the Viterbi, clean and with 12 of the 148 bits turned round */
        float soft[TAG_NBITS];
        for (int i = 0; i < TAG_NBITS; i++) soft[i] = c[i] ? 1.0f : -1.0f;
        uint8_t back[TAG_PAYLOAD_BITS];
        char call[TAG_CALL_LEN + 1];
        bool local = !TV_MSG[m].local;
        tag_viterbi(soft, back);
        CHECK(tag_unpack(back, call, &local));
        CHECK_STR(call, TV_MSG[m].call);
        CHECK_EQ(local, TV_MSG[m].local);
        for (int i = 5; i < TAG_NBITS; i += 12) soft[i] = -soft[i];
        tag_viterbi(soft, back);
        CHECK(tag_unpack(back, call, &local));
        CHECK_STR(call, TV_MSG[m].call);
    }

    CASE("callsigns: what may be one, and a CRC or a zero bit wrong is nothing");
    uint8_t p[TAG_PAYLOAD_BITS];
    char call[TAG_CALL_LEN + 1];
    bool local;
    CHECK(tag_call_ok("ON4ABC-XY"));
    CHECK(tag_call_ok("PA/ON6URE"));
    CHECK(!tag_call_ok(""));
    CHECK(!tag_call_ok("ON4ABC-XYZ"));                       /* ten */
    CHECK(!tag_call_ok("on6ure"));
    CHECK(!tag_call_ok("ON6 URE"));
    CHECK(!tag_pack("on6ure", false, p));
    CHECK(tag_pack("ON0ORA", true, p));
    CHECK(tag_unpack(p, call, &local));
    CHECK_STR(call, "ON0ORA");
    CHECK(local);
    p[60] ^= 1;
    CHECK(!tag_unpack(p, call, &local));
    p[60] ^= 1;
    p[50] = 1;                                               /* a zero bit, and its CRC made right */
    const uint16_t c = tag_crc16(p, TAG_CRC_FROM);
    for (int i = 0; i < 16; i++) p[TAG_CRC_FROM + i] = (uint8_t)((c >> (15 - i)) & 1);
    CHECK(!tag_unpack(p, call, &local));

    CASE("callsigns: as a voice says them");
    char say[160];
    CHECK(tag_say_text("ON6URE", false, say, sizeof say) > 0);
    CHECK_STR(say, "O, N, six, U, R, E");
    CHECK(tag_say_text("ON6URE", true, say, sizeof say) > 0);
    CHECK_STR(say, "Oscar November six Uniform Romeo Echo");
    CHECK(tag_say_text("EA8/ON0ORA-L", true, say, sizeof say) == 0);     /* ten: no callsign */
    CHECK(tag_say_text("PA/ON0OR-L", false, say, sizeof say) == 0);
    CHECK(tag_say_text("PA/ON0OR", true, say, sizeof say) > 0);
    CHECK_STR(say, "Papa Alfa stroke Oscar November zero Oscar Romeo");
    CHECK(tag_say_text("ON0ORA-L", false, say, sizeof say) > 0);
    CHECK_STR(say, "O, N, zero, O, R, A, dash, L");
    CHECK(tag_say_text("ON6URE", true, say, 12) == 0 && say[0] == 0);    /* too small: nothing */
    CHECK(tag_say_text("on6ure", false, say, sizeof say) == 0);

    CASE("callsigns: what looks like one is believed at once");
    static const char *const likely[] = { "ON6URE", "ON0ORA", "2E0ABC", "3DA0RS", "W1AW", "ON0ORA-L",
                                          "ON4ABC-1", "PA/ON6URE", "ON6URE/P", "F/ON6URE", "EA8/ON6UR", "ON6URE/MM",
                                          "ON6UR/P-L", "ON6URE/5", "ON6UR/QRP", "9M2AB" };
    for (size_t i = 0; i < sizeof likely / sizeof likely[0]; i++) CHECK(tag_call_likely(likely[i]));
    static const char *const odd[] = { "UWYVJD6UE", "ECHOLINK", "ON6URE1", "6URE", "ONURE", "ON-6URE",
                                       "ON6URE-", "ON4ABC-XY", "A/B/C/ON6", "ON6URE//P", "NRBS/ZB1E", "5/DZZ0RBM",
                                       "QM04ET-VX", "ON6URE/QRPP", "SNL6P/S1D", "Y1F4TYG-S", "NFW9P/QAV", "on6ure", "" };
    for (size_t i = 0; i < sizeof odd / sizeof odd[0]; i++) CHECK(!tag_call_likely(odd[i]));
    /* What a CRC passed by chance reads as: nine of the 40, the padding at
     * the end. About one in 90 of those looks like a callsign (2026-11-23). */
    uint32_t r = 12345;
    int ok = 0, looks = 0;
    for (int i = 0; i < 200000; i++) {
        char sx[TAG_CALL_LEN + 1];
        for (int j = 0; j < TAG_CALL_LEN; j++) {
            r = r * 1664525u + 1013904223u;
            sx[j] = TAG_ALPHABET[(r >> 8) % 39];
        }
        sx[TAG_CALL_LEN] = 0;
        int n = TAG_CALL_LEN;
        while (n > 0 && sx[n - 1] == ' ') sx[--n] = 0;
        if (!tag_call_ok(sx)) continue;
        ok++;
        if (tag_call_likely(sx)) looks++;
    }
    printf("    %d of %d random callsigns look like one\n", looks, ok);
    CHECK(looks * 50 < ok);
}

#define SECS(s) ((size_t)((s) * TAG_FS))

static void add_noise(float *x, size_t n, double rms)
{
    for (size_t i = 0; i < n; i++) x[i] += (float)(rms * gauss());
}

static void test_made(tag_dec_t *d)
{
    heard_t h;
    const size_t n = SECS(12);
    float *x = calloc(n, sizeof *x);

    CASE("made here: clean, from anywhere in its frame");
    for (int i = 0; i < 4; i++) {
        tag_wave(x, n, "ON4ABC-XY", false, urand() * 41984, 0, 0.05);
        run(d, x, n, &h);
        CHECK(only(&h, "ON4ABC-XY", false));
        CHECK(h.first_s > 0 && h.first_s < 6.5);
        if (!i) show("clean", d, &h);
    }

    CASE("made here: 6 dB and 10 dB under white noise");
    tag_wave(x, n, "ON0ORA", true, 777, 0, 0.05);
    add_noise(x, n, 0.1);
    run(d, x, n, &h);
    show("6 dB under noise", d, &h);
    CHECK(only(&h, "ON0ORA", true));
    tag_wave(x, n, "ON0ORA", true, 9000, 0, 0.05);
    add_noise(x, n, 0.158);
    run(d, x, n, &h);
    show("10 dB under noise", d, &h);
    CHECK(only(&h, "ON0ORA", true));

    CASE("made here: the repeater's clock 100 ppm either way");
    for (int s = -1; s <= 1; s += 2) {
        tag_wave(x, n, "ON6URE", false, 1234, 100.0 * s, 0.05);
        add_noise(x, n, 0.05);
        run(d, x, n, &h);
        show(s < 0 ? "-100 ppm, 0 dB under noise" : "+100 ppm, 0 dB under noise", d, &h);
        CHECK(only(&h, "ON6URE", false));
    }

    CASE("made here: 8 s of loud bursts 38 dB over it, then a 3.5 s tail");
    {
        const size_t m = SECS(11.5);
        float *y = calloc(m, sizeof *y);
        tag_wave(y, m, "ON4ABC-XY", false, 20000, 30, 0.01);
        for (size_t i = 0; i < SECS(8); i++) {
            /* syllables: noise switched on and off, 4 a second */
            const double env = fmod((double)i / TAG_FS * 4, 1.0) < 0.7 ? 1.0 : 0.05;
            y[i] += (float)(0.8 * env * gauss());
        }
        add_noise(y, m, 0.002);
        run(d, y, m, &h);
        show("bursts, then a 3.5 s tail", d, &h);
        CHECK(only(&h, "ON4ABC-XY", false));
        free(y);
    }

    CASE("made here: one talker, then another straight after");
    {
        const size_t m = SECS(16);
        float *y = calloc(m, sizeof *y);
        tag_wave(y, SECS(8), "ON4ABC", false, 0, 0, 0.05);
        tag_wave(y + SECS(8), m - SECS(8), "ON0ORA", true, 0, 0, 0.05);
        add_noise(y, m, 0.03);
        run(d, y, m, &h);
        show("ON4ABC then ON0ORA", d, &h);
        int a = 0, b = 0, other = 0;
        for (int i = 0; i < h.n; i++) {
            if (!strcmp(h.r[i].call, "ON4ABC") && !h.r[i].local) a++;
            else if (!strcmp(h.r[i].call, "ON0ORA") && h.r[i].local) b++;
            else other++;
        }
        CHECK(a > 0);
        CHECK(b > 0);
        CHECK_EQ(other, 0);
        bool order = true;                                   /* never the first again once the second is read */
        for (int i = 1; i < h.n; i++)
            if (!strcmp(h.r[i].call, "ON4ABC") && !strcmp(h.r[i - 1].call, "ON0ORA")) order = false;
        CHECK(order);
        free(y);
    }
    free(x);
}

static void test_nothing(tag_dec_t *d)
{
    const char *e = getenv("TAG_NOISE_S");
    const double secs = e ? atof(e) : 60;
    CASE("noise alone, and bursts alone: never a callsign");
    const size_t n = SECS(secs);
    float *x = malloc(n * sizeof *x);
    heard_t h;
    for (size_t i = 0; i < n; i++) x[i] = (float)(0.1 * gauss());
    run(d, x, n, &h);
    show("white noise", d, &h);
    CHECK_EQ(h.n, 0);
    for (size_t i = 0; i < n; i++) {
        const double env = fmod((double)i / TAG_FS * 3.3, 1.0) < 0.6 ? 1.0 : 0.02;
        x[i] = (float)(0.5 * env * gauss() + 0.2 * env * sin(2 * M_PI * 1650 * i / TAG_FS));
    }
    run(d, x, n, &h);
    show("bursts and a tone on the carrier", d, &h);
    CHECK_EQ(h.n, 0);
    free(x);
}

static void test_recordings(tag_dec_t *d)
{
    const char *dir = getenv("TAG_WAV_DIR");
    if (!dir) {
        printf("  TailChaser's recordings: TAG_WAV_DIR not set, left out\n");
        return;
    }
    static const struct { const char *file, *call; bool local; } R[] = {
        { "clean-ON6URE-network-16k.wav", "ON6URE", false },
        { "clean-ON0ORA-local-16k.wav", "ON0ORA", true },
        { "noisy-ON6URE-network-16k.wav", "ON6URE", false },
        { "speech-plus-tag-38dB-ON6URE-16k.wav", "ON6URE", false },
        { "offair-websdr-ON6URE-12k.wav", "ON6URE", false },
    };
    CASE("TailChaser's recordings, the one off the air among them");
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, R[i].file);
        size_t n = 0;
        float *x = load_wav(path, &n);
        CHECK(x != NULL);
        if (!x) continue;
        heard_t h;
        run(d, x, n, &h);
        show(R[i].file, d, &h);
        CHECK(only(&h, R[i].call, R[i].local));
        free(x);
    }
}


static void test_hist(void)
{
    static tag_hist_t h;
    const int64_t T0 = 1795000000;                           /* a wall time, in seconds */

    CASE("history: one entry an over; the over lasts while its tag is on the air");
    tag_hist_init(&h);
    CHECK_EQ(tag_hist_count(&h), 0);
    CHECK(tag_hist_at(&h, 0) == NULL);
    CHECK_EQ(tag_hist_reading(&h, "ON4ABC", false, true, 1000, T0 + 1), TAG_HIST_NEW);
    CHECK_EQ(tag_hist_reading(&h, "ON4ABC", false, true, 4000, T0 + 4), TAG_HIST_SAME);
    tag_hist_holding(&h, 12000, T0 + 12);                    /* under speech, no reading: still on the air */
    tag_hist_holding(&h, 20000, T0 + 20);
    CHECK_EQ(tag_hist_reading(&h, "ON4ABC", false, true, 29000, T0 + 29), TAG_HIST_SAME);
    CHECK_EQ(tag_hist_count(&h), 1);
    CHECK_EQ(tag_hist_at(&h, 0)->reads, 3);
    CHECK_EQ(tag_hist_at(&h, 0)->first_unix, T0 + 1);
    CHECK_EQ(tag_hist_at(&h, 0)->last_unix, T0 + 29);

    CASE("history: gone over 10 s, another callsign, or the local flag: a new entry");
    CHECK_EQ(tag_hist_reading(&h, "ON4ABC", false, true, 40000, T0 + 40), TAG_HIST_NEW);
    CHECK_EQ(tag_hist_reading(&h, "ON0ORA", true, true, 41000, T0 + 41), TAG_HIST_NEW);
    CHECK_EQ(tag_hist_reading(&h, "ON0ORA", false, true, 42000, T0 + 42), TAG_HIST_NEW);
    CHECK_EQ(tag_hist_count(&h), 4);
    CHECK_STR(tag_hist_at(&h, 0)->call, "ON0ORA");
    CHECK(!tag_hist_at(&h, 0)->local);
    CHECK(tag_hist_at(&h, 1)->local);
    CHECK_STR(tag_hist_at(&h, 3)->call, "ON4ABC");
    tag_hist_holding(&h, 60000, T0 + 60);                    /* too late: gone already */
    CHECK_EQ(tag_hist_at(&h, 0)->last_unix, T0 + 42);

    CASE("history: a reading that is not sure waits for a second one");
    CHECK_EQ(tag_hist_reading(&h, "ON9ZZZ", false, false, 100000, T0 + 100), TAG_HIST_WAIT);
    CHECK_EQ(tag_hist_count(&h), 4);
    CHECK_EQ(tag_hist_reading(&h, "ON8YYY", false, false, 101000, T0 + 101), TAG_HIST_WAIT);    /* another: it waits instead */
    CHECK_EQ(tag_hist_reading(&h, "ON9ZZZ", false, false, 102000, T0 + 102), TAG_HIST_WAIT);
    CHECK_EQ(tag_hist_reading(&h, "ON9ZZZ", false, true, 110000, T0 + 110), TAG_HIST_NEW);      /* the tail: sure */
    CHECK_EQ(tag_hist_at(&h, 0)->first_unix, T0 + 102);     /* heard from the first reading on */
    CHECK_EQ(tag_hist_at(&h, 0)->reads, 2);
    CHECK_EQ(tag_hist_reading(&h, "ON9ZZZ", false, false, 112000, T0 + 112), TAG_HIST_SAME);    /* the over on the air */
    CHECK_EQ(tag_hist_reading(&h, "ON7XXX", false, false, 120000, T0 + 120), TAG_HIST_WAIT);
    CHECK_EQ(tag_hist_reading(&h, "ON7XXX", false, false, 151000, T0 + 151), TAG_HIST_WAIT);    /* 31 s on: too late */
    CHECK_EQ(tag_hist_reading(&h, "ON7XXX", false, false, 160000, T0 + 160), TAG_HIST_NEW);     /* two, 9 s apart */
    CHECK_EQ(tag_hist_count(&h), 6);

    CASE("history: no wall time yet, then NTP");
    tag_hist_init(&h);
    tag_hist_reading(&h, "ON4ABC", false, true, 5000, 0);
    tag_hist_reading(&h, "ON0ORA", true, true, 65000, 0);
    tag_hist_clock(&h, 125000, T0);
    CHECK_EQ(tag_hist_at(&h, 0)->first_unix, T0 - 60);
    CHECK_EQ(tag_hist_at(&h, 1)->first_unix, T0 - 120);

    CASE("history: the newest 100 kept");
    tag_hist_init(&h);
    for (int i = 0; i < 105; i++) {
        char c[10];
        snprintf(c, sizeof c, "ON%dABC", i % 10);
        tag_hist_reading(&h, c, i & 1, true, 100000LL * i, T0 + 100 * i);
    }
    CHECK_EQ(tag_hist_count(&h), TAG_HIST_N);
    CHECK_EQ(tag_hist_at(&h, 0)->first_unix, T0 + 100 * 104);
    CHECK_EQ(tag_hist_at(&h, TAG_HIST_N - 1)->first_unix, T0 + 100 * 5);
    CHECK(tag_hist_at(&h, TAG_HIST_N) == NULL);

    CASE("history: to the card and back; what was heard before a restart is its own");
    static char buf[8192];
    const size_t n = tag_hist_save(&h, buf, sizeof buf);
    CHECK(n > 0 && n < sizeof buf);
    CHECK_EQ(strlen(buf), n);
    static tag_hist_t g;
    CHECK_EQ(tag_hist_load(&g, buf), TAG_HIST_N);
    CHECK_STR(tag_hist_at(&g, 0)->call, tag_hist_at(&h, 0)->call);
    CHECK_EQ(tag_hist_at(&g, 0)->local, tag_hist_at(&h, 0)->local);
    CHECK_EQ(tag_hist_at(&g, 0)->last_unix, tag_hist_at(&h, 0)->last_unix);
    CHECK_EQ(tag_hist_at(&g, 99)->first_unix, tag_hist_at(&h, 99)->first_unix);
    CHECK(!tag_hist_at(&g, 0)->now_start);
    CHECK_EQ(tag_hist_reading(&g, tag_hist_at(&g, 0)->call, tag_hist_at(&g, 0)->local, true, 1000, T0 + 99999), TAG_HIST_NEW);
    char small[60];
    const size_t k = tag_hist_save(&h, small, sizeof small);
    CHECK(k > 0 && k < sizeof small && small[k - 1] == '\n');       /* whole lines only */
    CHECK_EQ(tag_hist_load(&g, "garbage\n1 2 3 X ON4ABC\n5 4 1 N ON4ABC\n7 8 1 L on4abc\n9 9 2 L ON0ORA\n"), 1);
    CHECK_STR(tag_hist_at(&g, 0)->call, "ON0ORA");
    CHECK_EQ(tag_hist_at(&g, 0)->reads, 2);
}

int main(void)
{
    tag_dec_t *d = malloc(tag_dec_size());
    test_format();
    test_hist();
    const clock_t t0 = clock();
    test_made(d);
    test_recordings(d);
    test_nothing(d);
    printf("  decoder: %.1f s of the computer's time\n", (double)(clock() - t0) / CLOCKS_PER_SEC);
    free(d);
    printf("%s: %d checks, %d failed\n", __FILE__, t_run, t_fail);
    return t_fail ? 1 : 0;
}
