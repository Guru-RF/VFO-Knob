/* The Elecraft K4's remote protocol (components/k4_client/k4_parse.c): the
 * envelope every byte of it travels in, the audio packet, and the CAT text.
 * The layouts are the K4 Programmer's Reference rev. D12's, and the frame
 * format K4remote documents. */
#include "tiny.h"
#include "k4_parse.h"

typedef struct { uint8_t p[16][K4_FRAME_MAX]; size_t n[16]; int count; } sink_t;

static void on_frame(void *ctx, const uint8_t *p, size_t n)
{
    sink_t *s = ctx;
    if (s->count < 16) {
        memcpy(s->p[s->count], p, n);
        s->n[s->count] = n;
    }
    s->count++;
}

static void test_envelope(void)
{
    CASE("a CAT command out, and back");
    uint8_t w[64];
    const size_t n = k4_encode_cat(w, sizeof w, "FA00014074000;");
    CHECK_EQ(n, 8 + 3 + 14 + 4);
    CHECK(w[0] == 0xFE && w[3] == 0xFB && w[n - 4] == 0xFB && w[n - 1] == 0xFE);
    CHECK_EQ(w[7], 17);                       /* the payload: 3 + 14 */
    CHECK(w[8] == 0 && w[9] == 0 && w[10] == 0);
    CHECK_EQ(k4_encode_cat(w, 20, "FA00014074000;"), 0);   /* does not fit */

    k4_framer_t f;
    sink_t s;
    memset(&s, 0, sizeof s);
    k4_framer_init(&f);
    k4_framer_push(&f, w, n, on_frame, &s);
    CHECK_EQ(s.count, 1);
    CHECK_EQ(s.n[0], 17);
    size_t pos = 0;
    char cmd[32];
    CHECK(k4_cat_next(s.p[0], s.n[0], &pos, cmd, sizeof cmd));
    CHECK_STR(cmd, "FA00014074000");
    CHECK(!k4_cat_next(s.p[0], s.n[0], &pos, cmd, sizeof cmd));

    CASE("the same, a byte at a time, and two frames in one read");
    memset(&s, 0, sizeof s);
    k4_framer_init(&f);
    for (size_t i = 0; i < n; i++) k4_framer_push(&f, w + i, 1, on_frame, &s);
    CHECK_EQ(s.count, 1);
    uint8_t two[128];
    memcpy(two, w, n);
    memcpy(two + n, w, n);
    memset(&s, 0, sizeof s);
    k4_framer_push(&f, two, 2 * n, on_frame, &s);
    CHECK_EQ(s.count, 2);
    CHECK_EQ(f.bad, 0);

    CASE("garbage before a frame, and a frame whose end is wrong");
    uint8_t g[200];
    size_t o = 0;
    memset(g, 0x55, 10); o = 10;
    g[o++] = 0xFE; g[o++] = 0xFE;                  /* a false start, then the real one */
    memcpy(g + o, w, n); o += n;
    memcpy(g + o, w, n - 1); o += n - 1; g[o++] = 0x00;   /* last end byte wrong */
    memcpy(g + o, w, n); o += n;
    memset(&s, 0, sizeof s);
    k4_framer_init(&f);
    k4_framer_push(&f, g, o, on_frame, &s);
    CHECK_EQ(s.count, 2);                          /* the good ones; the bad one dropped */
    CHECK_EQ(f.bad, 1);

    CASE("a frame too long to keep is skipped whole, and the next one is read");
    uint8_t big[K4_FRAME_MAX + 64 + 8 + 4 + 64];
    const uint32_t blen = K4_FRAME_MAX + 64;
    o = 0;
    big[o++] = 0xFE; big[o++] = 0xFD; big[o++] = 0xFC; big[o++] = 0xFB;
    big[o++] = (uint8_t)(blen >> 24); big[o++] = (uint8_t)(blen >> 16);
    big[o++] = (uint8_t)(blen >> 8);  big[o++] = (uint8_t)blen;
    memset(big + o, 0x02, blen); o += blen;        /* a panadapter's bytes */
    big[o++] = 0xFB; big[o++] = 0xFC; big[o++] = 0xFD; big[o++] = 0xFE;
    memcpy(big + o, w, n); o += n;
    memset(&s, 0, sizeof s);
    k4_framer_init(&f);
    k4_framer_push(&f, big, o, on_frame, &s);
    CHECK_EQ(s.count, 1);
    CHECK_EQ(f.skipped, 1);

    CASE("an absurd length is corruption, not a reason to swallow the stream");
    uint8_t c[64];
    o = 0;
    c[o++] = 0xFE; c[o++] = 0xFD; c[o++] = 0xFC; c[o++] = 0xFB;
    c[o++] = 0xFF; c[o++] = 0xFF; c[o++] = 0xFF; c[o++] = 0xFF;
    memcpy(c + o, w, n); o += n;
    memset(&s, 0, sizeof s);
    k4_framer_init(&f);
    k4_framer_push(&f, c, o, on_frame, &s);
    CHECK_EQ(s.count, 1);
    CHECK_EQ(f.bad, 1);

    CASE("an empty frame");
    const uint8_t e[] = { 0xFE, 0xFD, 0xFC, 0xFB, 0, 0, 0, 0, 0xFB, 0xFC, 0xFD, 0xFE };
    memset(&s, 0, sizeof s);
    k4_framer_init(&f);
    k4_framer_push(&f, e, sizeof e, on_frame, &s);
    CHECK_EQ(s.count, 1);
    CHECK_EQ(s.n[0], 0);
}

static void test_audio(void)
{
    CASE("audio, out and back");
    const uint8_t opus[] = { 1, 2, 3, 4, 5 };
    uint8_t w[64];
    const size_t n = k4_encode_audio(w, sizeof w, 7, K4_EM_OPUS_INT, 240, opus, sizeof opus);
    CHECK_EQ(n, 8 + 7 + 5 + 4);
    k4_framer_t f;
    sink_t s;
    memset(&s, 0, sizeof s);
    k4_framer_init(&f);
    k4_framer_push(&f, w, n, on_frame, &s);
    CHECK_EQ(s.count, 1);
    k4_audio_t a;
    CHECK(k4_audio_parse(s.p[0], s.n[0], &a));
    CHECK_EQ(a.seq, 7);
    CHECK_EQ(a.mode, K4_EM_OPUS_INT);
    CHECK_EQ(a.samples, 240);
    CHECK_EQ(a.n, 5);
    CHECK(!memcmp(a.data, opus, 5));
    CHECK_EQ(s.p[0][4], 240 & 0xFF);              /* the sample count is little-endian */
    CHECK_EQ(s.p[0][5], 0);

    CASE("what is not audio, or too short");
    CHECK(!k4_audio_parse(s.p[0], 6, &a));
    const uint8_t cat[] = { 0, 0, 0, 'F', 'A' };
    CHECK(!k4_audio_parse(cat, sizeof cat, &a));
    CHECK_EQ(k4_encode_audio(w, 10, 0, 3, 240, opus, sizeof opus), 0);
}

/* An IF response as the programmer's reference lays it out:
 * IF[f*11][5 spaces][+yyyy][r][x][space][00][t][m][0][s][p][b][d][1][space]; */
static void make_if(char *out, const char *f, char sign, int rit, int rit_on, int tx, char mode)
{
    sprintf(out, "IF%s     %c%04d%c0 00%c%c0000001 ", f, sign, rit, rit_on ? '1' : '0',
            tx ? '1' : '0', mode);
}

static void test_status(void)
{
    k4_status_t st;
    char b[64];

    CASE("IF: frequency, RIT, transmitting, mode");
    memset(&st, 0, sizeof st);
    make_if(b, "00014074000", '-', 150, 1, 0, '2');
    CHECK(k4_apply(&st, b));
    CHECK(st.have_f && st.f_hz == 14074000);
    CHECK(st.have_rit && st.rit_hz == -150 && st.rit_on);
    CHECK(st.have_tx && !st.tx);
    CHECK_STR(st.mode, "usb");
    make_if(b, "00007100000", '+', 0, 0, 1, '1');
    CHECK(k4_apply(&st, b));
    CHECK(st.tx && st.f_hz == 7100000 && !st.rit_on);
    CHECK_STR(st.mode, "lsb");
    CHECK(!k4_apply(&st, "IF0001407"));            /* too short to read */

    CASE("FA, MD, BW");
    CHECK(k4_apply(&st, "FA00003573000"));
    CHECK_EQ(st.f_hz, 3573000);
    CHECK(k4_apply(&st, "MD6"));
    CHECK_STR(st.mode, "digu");
    CHECK(k4_apply(&st, "MD9"));
    CHECK_STR(st.mode, "digl");
    CHECK(k4_apply(&st, "MD7"));
    CHECK_STR(st.mode, "cwr");
    CHECK(!k4_apply(&st, "MD8"));                  /* not a mode */
    CHECK_STR(st.mode, "cwr");                     /* and nothing was changed */
    CHECK(k4_apply(&st, "BW0240"));
    CHECK_EQ(st.bw_hz, 2400);
    CHECK(!k4_apply(&st, "BW02x0"));
    CHECK(!k4_apply(&st, "FA99999999999999999999999999999"));   /* not a number to overflow on */

    CASE("the sub receiver's answers are not ours");
    const int f_before = (int)st.f_hz;
    CHECK(!k4_apply(&st, "MD$3"));
    CHECK(!k4_apply(&st, "BW$0050"));
    CHECK(!k4_apply(&st, "SM$12"));
    CHECK(!k4_apply(&st, "SMH$-090"));
    CHECK_EQ(st.f_hz, f_before);

    CASE("the S-meter");
    CHECK(k4_apply(&st, "SMH-073"));
    CHECK(st.have_smeter && st.smeter_dbm == -73.0f);
    CHECK(k4_apply(&st, "SMH+012"));
    CHECK(st.smeter_dbm == 12.0f);
    CHECK(k4_apply(&st, "SM21"));
    CHECK_EQ(st.bars, 21);
    CHECK(!k4_apply(&st, "SM"));

    CASE("TX meter: QRO is watts, QRP is tenths");
    memset(&st, 0, sizeof st);
    CHECK(k4_apply(&st, "TM003000050012"));        /* no PC heard: tenths */
    CHECK(st.alc == 3.0f && st.fwd_w == 5.0f && st.swr == 1.2f && st.cmp_db == 0);
    CHECK(k4_apply(&st, "PC050H"));
    CHECK(st.have_pc && st.pc == 50 && st.pc_range == 'H');
    CHECK(k4_apply(&st, "TM003000050012"));
    CHECK(st.fwd_w == 50.0f);
    CHECK(k4_apply(&st, "PC020L"));
    CHECK(k4_apply(&st, "TM000000015010"));
    CHECK(st.fwd_w == 1.5f && st.swr == 1.0f);
    CHECK(!k4_apply(&st, "TM00300005"));           /* short */

    CASE("transmit state");
    CHECK(k4_apply(&st, "TQ1"));
    CHECK(st.tx);
    CHECK(k4_apply(&st, "RX"));
    CHECK(!st.tx);
    CHECK(!k4_apply(&st, "TQ2"));

    CASE("AGC, RF gain, RIT");
    CHECK(k4_apply(&st, "GT2"));
    CHECK_STR(k4_agc_name(st.agc), "fast");
    CHECK(k4_apply(&st, "RG-12"));
    CHECK_EQ(st.rg_db, -12);
    CHECK(k4_apply(&st, "RO-0100"));
    CHECK_EQ(st.rit_hz, -100);
    CHECK(k4_apply(&st, "RT1"));
    CHECK(st.rit_on && st.have_rit && st.rit_hz == -100);   /* the offset stands, now it counts */

    CASE("passband centre, and the DSP");
    CHECK(k4_apply(&st, "IS0150"));
    CHECK_EQ(st.is_hz, 1500);
    CHECK(k4_apply(&st, "IS 0060"));              /* as a real K4 sent it: a space first */
    CHECK_EQ(st.is_hz, 600);
    CHECK(k4_apply(&st, "IS  0150"));
    CHECK_EQ(st.is_hz, 1500);
    CHECK(!k4_apply(&st, "IS "));
    CHECK(!k4_apply(&st, "IS x"));
    CHECK(k4_apply(&st, "NB1"));                  /* the alternate form, as a real K4 sent it */
    CHECK(st.have_nb && st.nb_on);
    CHECK(k4_apply(&st, "NB0"));
    CHECK(!st.nb_on);
    CHECK(!k4_apply(&st, "NB2"));
    CHECK(k4_apply(&st, "NB051"));
    CHECK(st.have_nb && st.nb_on);
    CHECK(k4_apply(&st, "NB050"));
    CHECK(!st.nb_on);
    CHECK(!k4_apply(&st, "NB05"));
    CHECK(k4_apply(&st, "NR041"));
    CHECK(st.have_nr && st.nr_on && st.nr_level == 4);
    CHECK(k4_apply(&st, "NR990"));
    CHECK(!st.nr_on && st.nr_level == 10);       /* clamped, not trusted */
    CHECK(k4_apply(&st, "NA1"));
    CHECK(st.have_na && st.na_on);
    CHECK(k4_apply(&st, "AP12"));
    CHECK(st.have_ap && st.ap_on && st.ap_width == 2);
    CHECK(!k4_apply(&st, "AP19"));
    CHECK(!k4_apply(&st, "NB$051"));

    CASE("ID, PONG, and errors");
    CHECK(k4_apply(&st, "ID017"));
    CHECK_STR(st.id, "017");
    CHECK(k4_apply(&st, "PONG"));
    CHECK(st.pong);
    CHECK(!k4_apply(&st, "ER1"));                  /* only the mode's echo */
    CHECK_EQ(st.err, 0);
    CHECK(k4_apply(&st, "ER14:TX inhibited"));
    CHECK_EQ(st.err, 14);
    CHECK_STR(st.err_text, "TX inhibited");
    CHECK(!k4_apply(&st, "ZZ99"));
}

static void test_cat_text(void)
{
    CASE("several commands in one payload, a stray ';', and a cut one");
    const uint8_t p[] = { 0, 0, 0, 'F','A','0','1',';',';','M','D','2',';','S','M','1' };
    size_t pos = 0;
    char c[16];
    CHECK(k4_cat_next(p, sizeof p, &pos, c, sizeof c));
    CHECK_STR(c, "FA01");
    CHECK(k4_cat_next(p, sizeof p, &pos, c, sizeof c));
    CHECK_STR(c, "MD2");
    CHECK(!k4_cat_next(p, sizeof p, &pos, c, sizeof c));   /* SM1 has no ';': not a command yet */

    CASE("a command longer than the room is cut, not split in two");
    const uint8_t q[] = { 0, 0, 0, 'A','B','C','D','E','F','G','H',';','X',';' };
    pos = 0;
    char s4[4];
    CHECK(k4_cat_next(q, sizeof q, &pos, s4, sizeof s4));
    CHECK_STR(s4, "ABC");
    CHECK(k4_cat_next(q, sizeof q, &pos, s4, sizeof s4));
    CHECK_STR(s4, "X");

    CASE("not CAT");
    const uint8_t a[] = { 1, 1, 0, 'F', 'A', ';' };
    pos = 0;
    CHECK(!k4_cat_next(a, sizeof a, &pos, c, sizeof c));
}

static void test_commands(void)
{
    char b[32];
    CASE("commands");
    CHECK(k4_cmd_freq(b, sizeof b, 14074000));
    CHECK_STR(b, "FA00014074000;");
    CHECK(k4_cmd_freq(b, sizeof b, -5));
    CHECK_STR(b, "FA00000000000;");
    CHECK_EQ(k4_cmd_freq(b, 5, 1), 0);
    CHECK(k4_cmd_mode(b, sizeof b, '3'));
    CHECK_STR(b, "MD3;");
    CHECK_EQ(k4_cmd_mode(b, sizeof b, 'x'), 0);
    CHECK(k4_cmd_bw(b, sizeof b, 2400));
    CHECK_STR(b, "BW0240;");
    CHECK(k4_cmd_bw(b, sizeof b, 1000000));
    CHECK_STR(b, "BW9999;");
    CHECK(k4_cmd_rit(b, sizeof b, 100));
    CHECK_STR(b, "RO+0100;");
    CHECK(k4_cmd_rit(b, sizeof b, -20000));
    CHECK_STR(b, "RO-9999;");
    CHECK(k4_cmd_rg(b, sizeof b, -12));
    CHECK_STR(b, "RG-12;");
    CHECK(k4_cmd_rg(b, sizeof b, 99));
    CHECK_STR(b, "RG-60;");

    CASE("mode names round-trip");
    static const char *names[] = { "lsb", "usb", "cw", "fm", "am", "digu", "cwr", "digl" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char d = k4_mode_digit(names[i]);
        CHECK(d != 0);
        CHECK(k4_mode_name(d) && !strcmp(k4_mode_name(d), names[i]));
    }
    CHECK_EQ(k4_mode_digit("rtty"), 0);
    CHECK_EQ(k4_agc_digit("fast"), 2);
    CHECK_EQ(k4_agc_digit("mid"), -1);

    CASE("hex");
    const uint8_t d[] = { 0x00, 0xAB, 0xFF };
    char h[8];
    k4_hex(h, d, 3);
    CHECK_STR(h, "00abff");
}

/* Whatever comes off the network: no crash, no overrun, and the framer still
 * finds a real frame after it. */
static void test_fuzz(void)
{
    CASE("random bytes");
    uint8_t w[64];
    const size_t n = k4_encode_cat(w, sizeof w, "FA00014074000;");
    k4_framer_t f;
    k4_framer_init(&f);
    sink_t s;
    uint32_t x = 12345;
    for (int round = 0; round < 2000; round++) {
        uint8_t junk[300];
        for (size_t i = 0; i < sizeof junk; i++) {
            x = x * 1664525u + 1013904223u;
            /* Biased towards the marker's bytes, to get deep into the states. */
            const uint32_t r = x >> 24;
            junk[i] = r < 40 ? 0xFE : r < 80 ? 0xFD : r < 100 ? 0xFB : (uint8_t)r;
        }
        memset(&s, 0, sizeof s);
        k4_framer_push(&f, junk, sizeof junk, on_frame, &s);
        for (int i = 0; i < s.count && i < 16; i++) {
            size_t pos = 0;
            char c[40];
            k4_status_t st;
            memset(&st, 0, sizeof st);
            while (k4_cat_next(s.p[i], s.n[i], &pos, c, sizeof c)) k4_apply(&st, c);
        }
    }
    CHECK(1);

    CASE("a real frame after the junk");
    /* Enough clean bytes to leave whatever state the junk ended in. */
    uint8_t quiet[K4_FRAME_MAX + 32];
    memset(quiet, 0x11, sizeof quiet);
    k4_framer_push(&f, quiet, sizeof quiet, on_frame, &s);
    memset(&s, 0, sizeof s);
    k4_framer_push(&f, w, n, on_frame, &s);
    CHECK_EQ(s.count, 1);

    CASE("random text into the status parser");
    k4_status_t st;
    memset(&st, 0, sizeof st);
    static const char *pre[] = { "IF", "FA", "MD", "BW", "SM", "SMH", "TM", "TQ", "GT", "RG", "RO",
                                 "RT", "PC", "ID", "ER", "PONG", "IS", "NB", "NR", "NA", "AP" };
    for (int round = 0; round < 20000; round++) {
        char c[48];
        x = x * 1664525u + 1013904223u;
        strcpy(c, pre[(x >> 20) % (sizeof pre / sizeof pre[0])]);
        size_t l = strlen(c);
        const size_t more = (x >> 8) % 40;
        for (size_t i = 0; i < more; i++) {
            x = x * 1664525u + 1013904223u;
            const uint32_t r = (x >> 24) % 14;
            c[l++] = r < 10 ? (char)('0' + r) : r == 10 ? '-' : r == 11 ? '+' : r == 12 ? ':' : '$';
        }
        c[l] = 0;
        k4_apply(&st, c);
    }
    CHECK(1);
}

T_MAIN({
    test_envelope();
    test_audio();
    test_status();
    test_cat_text();
    test_commands();
    test_fuzz();
})
