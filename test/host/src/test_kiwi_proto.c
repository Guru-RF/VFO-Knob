/* components/kiwi_proto/kiwi_proto.c: a receiver's /status, its clock, the
 * host:port key the marks are kept under, the rule that lifts a mark -- and a
 * session's plain-C parts: the tune command, what the MSGs mean, the SND
 * frames, the audio's rate, the names on the dial. */
#include <math.h>

#include "tiny.h"
#include "kiwi_proto.h"

static void test_asctime(void)
{
    CASE("asctime");
    uint32_t t = 0;
    CHECK(kiwi_asctime("Fri Oct  2 13:12:00 2026", &t));
    CHECK_EQ(t, 1790946720);
    /* The weekday is not checked: only the date says when. */
    CHECK(kiwi_asctime("Thu Oct  2 13:12:00 2026", &t));
    CHECK_EQ(t, 1790946720);
    CHECK(kiwi_asctime("Tue Feb 29 12:00:00 2028", &t));
    CHECK_EQ(t, 1835438400);
    CHECK(kiwi_asctime("Thu Jan  1 00:00:00 1970", &t));
    CHECK_EQ(t, 0);
    CHECK(!kiwi_asctime("Fri Foo  2 13:12:00 2026", &t));
    CHECK(!kiwi_asctime("Fri Oct 32 13:12:00 2026", &t));
    CHECK(!kiwi_asctime("Fri Oct  2 25:12:00 2026", &t));
    CHECK(!kiwi_asctime("tomorrow", &t));
    CHECK(!kiwi_asctime("", &t));
    CHECK(!kiwi_asctime(NULL, &t));
}

static void test_hp(void)
{
    /* Kept in NVS by every firmware: the value may never change. */
    CASE("host:port key");
    CHECK_EQ(kiwi_hp("81.83.21.23", 8077), 0xf759ad87u);
    CHECK_EQ(kiwi_hp("kiwi.on4cdj.be", 8073), 0x3fca91deu);
    CHECK_EQ(kiwi_hp("KIWI.on4cdj.BE", 8073), kiwi_hp("kiwi.on4cdj.be", 8073));
    CHECK(kiwi_hp("81.83.21.23", 8077) != kiwi_hp("81.83.21.23", 8076));
    CHECK(kiwi_hp("", 0) != 0);
    CHECK(kiwi_hp(NULL, 0) != 0);
}

static void test_unescape(void)
{
    CASE("unescape");
    char a[] = "0,Kicked%20by%20the%20admin";
    kiwi_unescape(a);
    CHECK_STR(a, "0,Kicked by the admin");
    char b[] = "60%2C81.83.21.23";
    kiwi_unescape(b);
    CHECK_STR(b, "60,81.83.21.23");
    char c[] = "100%zz%4";               /* not escapes: kept as they are */
    kiwi_unescape(c);
    CHECK_STR(c, "100%zz%4");
    char d[] = "a%00b";
    kiwi_unescape(d);
    CHECK_STR(d, "ab");
}

static const char WEB888[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n"
    "status=active\r\n"
    "offline=no\r\n"
    "name=RF.Guru Lombardsijde | EchoTracer\r\n"
    "sdr_hw=Web-888 v2026.0901 \xE2\x81\xA3 \xF0\x9F\x93\xA1 GPS \xE2\x81\xA3 \xE2\x8F\xB3 LIMITS\r\n"
    "op_email=\r\n"
    "bands=0-62000000\r\n"
    "freq_offset=0.000\r\n"
    "users=10\r\n"
    "users_max=13\r\n"
    "loc=Lombardsijde, Belgium\r\n"
    "sw_version=Web888_v2026.0901\r\n"
    "antenna=RF.Guru EchoTracer\r\n"
    "uptime=86400\r\n"
    "date=Fri Oct  2 13:12:00 2026\r\n";

static const char KIWISDR[] =
    "status=active\n"
    "offline=no\n"
    "name=ON4CDJ KiwiSDR\n"
    "sdr_hw=KiwiSDR 2 v1.902 \xE2\x81\xA3 \xF0\x9F\x93\xA1 GPS\n"
    "users=2\n"
    "users_max=8\n"
    "ext_api=0\n"
    "bands=0-30000000\n"
    "freq_offset=100000.000\n"
    "sw_version=KiwiSDR_v1.902\n"
    "uptime=5\n"
    "date=Thu Jan  1 00:00:05 1970\n";

static void test_status(void)
{
    kiwi_status_t st;
    CASE("status: a Web-888, headers and all");
    char a[sizeof WEB888];
    memcpy(a, WEB888, sizeof a);
    kiwi_status_parse(a, &st);
    CHECK(st.ok);
    CHECK_EQ(st.kind, KIWI_KIND_WEB888);
    CHECK(st.tlimits);
    CHECK(!st.offline);
    CHECK_STR(st.name, "RF.Guru Lombardsijde | EchoTracer");
    CHECK_STR(st.antenna, "RF.Guru EchoTracer");
    CHECK_STR(st.loc, "Lombardsijde, Belgium");
    CHECK_STR(st.sw, "Web888_v2026.0901");
    CHECK_EQ(st.users, 10);
    CHECK_EQ(st.users_max, 13);
    CHECK_EQ(st.ext_api, -1);            /* a KiwiSDR's word only */
    CHECK_EQ(st.bands_lo, 0);
    CHECK_EQ(st.bands_hi, 62000000);
    CHECK_EQ(st.uptime_s, 86400);
    CHECK_EQ(st.date_utc, 1790946720);
    CHECK(st.timed);

    CASE("status: a KiwiSDR, no limits, no clock yet");
    char b[sizeof KIWISDR];
    memcpy(b, KIWISDR, sizeof b);
    kiwi_status_parse(b, &st);
    CHECK(st.ok);
    CHECK_EQ(st.kind, KIWI_KIND_KIWISDR);
    CHECK(!st.tlimits);
    CHECK_EQ(st.ext_api, 0);
    CHECK_EQ((int)st.offset_khz, 100000);
    CHECK_EQ(st.uptime_s, 5);
    CHECK_EQ(st.date_utc, 0);            /* 1970: its clock is not set */
    CHECK(!st.timed);

    CASE("status: a field cut where a character ends");
    {
        char t[200];
        /* antenna[48]: 46 letters and a 2-byte character -- 48 bytes, no
         * room for its end: the character goes whole. */
        snprintf(t, sizeof t, "sdr_hw=x\nantenna=%s\xC3\xBC\n", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrst");
        kiwi_status_parse(t, &st);
        CHECK_EQ(strlen(st.antenna), 46);
        snprintf(t, sizeof t, "sdr_hw=x\nantenna=%s\xC3\xBC\n", "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqr");
        kiwi_status_parse(t, &st);
        CHECK_EQ(strlen(st.antenna), 46);                   /* 44 + 2: fits */
    }

    CASE("status: not a Kiwi");
    char c[] = "HTTP/1.1 200 OK\r\n\r\n<html><body>hello</body></html>";
    kiwi_status_parse(c, &st);
    CHECK(!st.ok);
    CHECK_EQ(st.users, -1);
    kiwi_status_parse(NULL, &st);
    CHECK(!st.ok);
    CHECK_EQ(st.kind, KIWI_KIND_UNKNOWN);
}

/* A /status of a receiver with limits that started at `boot` and says `date`. */
static kiwi_status_t said(uint8_t kind, uint32_t boot, uint32_t date)
{
    kiwi_status_t st;
    kiwi_status_parse(NULL, &st);
    st.ok = true;
    st.tlimits = true;
    st.kind = kind;
    st.date_utc = date;
    st.uptime_s = date - boot;
    st.timed = true;
    return st;
}

static void test_lifted(void)
{
    const uint32_t B = 1790000000u, T = B + 3600;
    kiwi_mark_t m = { .hp = 1, .strikes = 2, .kind = KIWI_KIND_WEB888, .rx_boot = B, .mark_utc = T };
    kiwi_status_t st;

    CASE("lifted: nothing proven");
    st = said(KIWI_KIND_WEB888, B, T + 7 * 86400);   /* a week on, the same boot */
    CHECK(!kiwi_mark_lifted(&m, &st));               /* a Web-888 never forgets */
    st = said(KIWI_KIND_WEB888, B + 60, T + 600);    /* a minute's drift of its clock */
    CHECK(!kiwi_mark_lifted(&m, &st));
    st.ok = false;
    CHECK(!kiwi_mark_lifted(&m, &st));
    CHECK(!kiwi_mark_lifted(NULL, &st));
    CHECK(!kiwi_mark_lifted(&m, NULL));

    CASE("lifted: it restarted");
    st = said(KIWI_KIND_WEB888, B + 3 * 3600, B + 4 * 3600);
    CHECK(kiwi_mark_lifted(&m, &st));

    CASE("lifted: no time limits now -- where it had them");
    st = said(KIWI_KIND_WEB888, B, T + 60);
    st.tlimits = false;
    CHECK(!kiwi_mark_lifted(&m, &st));               /* never seen: it refused without one */
    m.flags = KIWI_MARK_UNSURE;
    CHECK(!kiwi_mark_lifted(&m, &st));               /* kiwi_mark_check lets that one rest */
    m.flags = KIWI_MARK_HG;
    CHECK(kiwi_mark_lifted(&m, &st));                /* seen, and gone: taken away */
    m.flags = KIWI_MARK_HG | KIWI_MARK_REST;
    CHECK(kiwi_mark_lifted(&m, &st));                /* one at rest too */
    st.tlimits = true;
    CHECK(!kiwi_mark_lifted(&m, &st));
    m.flags = 0;

    CASE("lifted: a KiwiSDR, a day on");
    m.kind = KIWI_KIND_KIWISDR;
    st = said(KIWI_KIND_KIWISDR, B, T + 86400 - 1);
    CHECK(!kiwi_mark_lifted(&m, &st));
    st = said(KIWI_KIND_KIWISDR, B, T + 86400);
    CHECK(kiwi_mark_lifted(&m, &st));
    st = said(KIWI_KIND_UNKNOWN, B, T + 86400);      /* the mark knew what it was */
    CHECK(kiwi_mark_lifted(&m, &st));
    st = said(KIWI_KIND_WEB888, B, T + 86400);       /* it says otherwise now */
    CHECK(!kiwi_mark_lifted(&m, &st));

    CASE("lifted: unknown times prove nothing");
    kiwi_mark_t u = { .hp = 1, .strikes = 1, .kind = KIWI_KIND_KIWISDR };
    st = said(KIWI_KIND_KIWISDR, B + 30 * 86400, B + 31 * 86400);
    CHECK(!kiwi_mark_lifted(&u, &st));
    st = said(KIWI_KIND_KIWISDR, B, T);
    st.date_utc = 0;                                 /* no clock */
    st.timed = false;
    CHECK(!kiwi_mark_lifted(&m, &st));

    CASE("lifted: read in its first second");
    m.kind = KIWI_KIND_WEB888;
    st = said(KIWI_KIND_WEB888, B + 7200, B + 7200); /* uptime=0 */
    CHECK(kiwi_mark_lifted(&m, &st));

    /* Its count cleared -- the mark gone, strikes and all -- or only its
     * time limits gone, which proves nothing of the count it keeps. */
    CASE("cleared: a restart, a KiwiSDR's day; not the hourglass gone");
    m = (kiwi_mark_t){ .hp = 1, .strikes = 2, .kind = KIWI_KIND_WEB888, .rx_boot = B, .mark_utc = T };
    st = said(KIWI_KIND_WEB888, B + 3 * 3600, B + 4 * 3600);
    CHECK(kiwi_mark_cleared(&m, &st));               /* restarted */
    st = said(KIWI_KIND_WEB888, B, T + 60);
    st.tlimits = false;
    m.flags = KIWI_MARK_HG;
    CHECK(kiwi_mark_lifted(&m, &st));
    CHECK(!kiwi_mark_cleared(&m, &st));              /* lifted, its count kept */
    m.kind = KIWI_KIND_KIWISDR;
    st = said(KIWI_KIND_KIWISDR, B, T + 86400);
    CHECK(kiwi_mark_cleared(&m, &st));
    st.ok = false;
    CHECK(!kiwi_mark_cleared(&m, &st));
    CHECK(!kiwi_mark_cleared(NULL, &st));
    CHECK(!kiwi_mark_cleared(&m, NULL));

    CASE("lifted: a restart, though it shows no hourglass");
    m = (kiwi_mark_t){ .hp = 1, .strikes = 2, .kind = KIWI_KIND_WEB888, .rx_boot = B, .mark_utc = T };
    st = said(KIWI_KIND_WEB888, B + 3 * 3600, B + 4 * 3600);
    st.tlimits = false;                              /* it never showed one, and refused */
    CHECK(kiwi_mark_lifted(&m, &st));                /* its restart still clears the count */
    st = said(KIWI_KIND_WEB888, B, T + 7 * 86400);
    st.tlimits = false;
    CHECK(!kiwi_mark_lifted(&m, &st));               /* the same boot: nothing proven */
}

/* ------------------------------------------------------------ a session */

/* The tune for a receiver that centres CW on `cw`... */
static void tune_cw(const char *mode, int64_t hz, int32_t lo, int32_t hi, double off, int64_t bw, double rate,
                    int32_t cw, const char *want)
{
    kiwi_tune_t t = { .hz = hz, .lo = lo, .hi = hi };
    snprintf(t.mode, sizeof t.mode, "%s", mode);
    char out[128];
    CHECK(kiwi_tune_cmd(out, sizeof out, &t, off, bw, rate, cw) > 0);
    CHECK_STR(out, want);
}

/* ...and for one that has said nothing of it: a KiwiSDR's 500 Hz. */
static void tune(const char *mode, int64_t hz, int32_t lo, int32_t hi, double off, int64_t bw, double rate,
                 const char *want)
{
    tune_cw(mode, hz, lo, hi, off, bw, rate, KIWI_CW_PITCH, want);
}

/* Whether the receiver reaches the dial, what it says it covers -- and so is
 * told the dial as it is, where otherwise kiwi_tune_cmd keeps it at the
 * edge. Not CW, whose carrier is below the dial. */
static void reach(const char *mode, int64_t hz, double off, int64_t bw, bool want)
{
    kiwi_tune_t t = { .hz = hz };
    snprintf(t.mode, sizeof t.mode, "%s", mode);
    CHECK(kiwi_tune_reaches(&t, off, bw) == want);
    char out[128], f[40];
    CHECK(kiwi_tune_cmd(out, sizeof out, &t, off, bw, 12000, KIWI_CW_PITCH) > 0);
    const int n = snprintf(f, sizeof f, " freq=%.3f", (double)hz / 1000.0 - off);
    CHECK((strlen(out) >= (size_t)n && !strcmp(out + strlen(out) - n, f)) == want);
}

static void test_tune_cmd(void)
{
    CASE("tune: CW, the station on the dial at a 500 Hz tone");
    tune("cw", 7030000, -200, 200, 0, 30000000, 12000, "SET mod=cw low_cut=300 high_cut=700 freq=7029.500");
    tune("cwr", 7030000, -200, 200, 0, 30000000, 12000, "SET mod=cw low_cut=300 high_cut=700 freq=7029.500");
    tune("cw", 7030000, 0, 0, 0, 30000000, 12000, "SET mod=cw low_cut=300 high_cut=700 freq=7029.500");
    tune("cw", 7030000, -10, 10, 0, 30000000, 12000, "SET mod=cw low_cut=475 high_cut=525 freq=7029.500");
    CASE("tune: CW on the carrier, where an UberSDR's Kiwi input centres it");
    tune_cw("cw", 7030000, -200, 200, 0, 30000000, 12000, 0, "SET mod=cw low_cut=-200 high_cut=200 freq=7030.000");
    tune_cw("cwr", 7030000, -250, 250, 0, 30000000, 12000, 0, "SET mod=cw low_cut=-250 high_cut=250 freq=7030.000");
    tune_cw("cw", 7030000, 0, 0, 0, 30000000, 12000, 0, "SET mod=cw low_cut=-200 high_cut=200 freq=7030.000");
    CASE("tune: CW where its owner centres it: 400..800, a 600 Hz tone");
    tune_cw("cw", 7030000, -250, 250, 0, 30000000, 12000, 600, "SET mod=cw low_cut=350 high_cut=850 freq=7029.400");
    tune_cw("cwn", 7030000, -30, 30, 0, 30000000, 12000, 600, "SET mod=cwn low_cut=570 high_cut=630 freq=7029.400");
    tune_cw("cw", 7030000, -200, 200, 0, 30000000, 12000, -1000,
            "SET mod=cw low_cut=-1200 high_cut=-800 freq=7031.000");
    tune_cw("cw", 100030000, -200, 200, 100000, 30000000, 12000, 0,
            "SET mod=cw low_cut=-200 high_cut=200 freq=30.000");
    CASE("tune: the centre is CW's alone");
    tune_cw("usb", 14074000, 300, 2700, 0, 30000000, 12000, 0,
            "SET mod=usb low_cut=300 high_cut=2700 freq=14074.000");
    tune_cw("lsb", 7100000, -2700, -300, 0, 30000000, 12000, 600,
            "SET mod=lsb low_cut=-2700 high_cut=-300 freq=7100.000");
    tune_cw("am", 6000000, 0, 0, 0, 0, 0, 0, "SET mod=am low_cut=-4900 high_cut=4900 freq=6000.000");
    CASE("tune: a sideband, around the dial as given");
    tune("usb", 14074000, 300, 2700, 0, 30000000, 12000, "SET mod=usb low_cut=300 high_cut=2700 freq=14074.000");
    tune("lsb", 7100000, -2700, -300, 0, 30000000, 12000, "SET mod=lsb low_cut=-2700 high_cut=-300 freq=7100.000");
    CASE("tune: a radio's LSB, given the other way up");
    tune("lsb", 7100000, 100, 2800, 0, 30000000, 12000, "SET mod=lsb low_cut=-2800 high_cut=-100 freq=7100.000");
    tune("digl", 7100000, 100, 2800, 0, 30000000, 12000, "SET mod=lsb low_cut=-2800 high_cut=-100 freq=7100.000");
    CASE("tune: an offset: the baseband it is");
    tune("usb", 100100000, 300, 2700, 100000, 30000000, 12000,
         "SET mod=usb low_cut=300 high_cut=2700 freq=100.000");
    CASE("tune: kept where it tunes");
    tune("usb", 50000000, 300, 2700, 0, 30000000, 12000, "SET mod=usb low_cut=300 high_cut=2700 freq=30000.000");
    tune("usb", 5000, 300, 2700, 100000, 30000000, 12000, "SET mod=usb low_cut=300 high_cut=2700 freq=0.000");
    CASE("tune: whether it reaches the dial, or would be kept at its edge");
    reach("usb", 14074000, 0, 30000000, true);
    reach("usb", 30000000, 0, 30000000, true);           /* its very top */
    reach("usb", 30000001, 0, 30000000, false);
    reach("usb", 50150000, 0, 30000000, false);          /* 6 m: not on a KiwiSDR... */
    reach("usb", 50150000, 0, 62000000, true);           /* ...on a Web-888 */
    reach("usb", 50150000, 0, 0, true);                  /* a bandwidth of 0: anything over the offset */
    reach("lsb", 0, 0, 30000000, true);                  /* its very bottom */
    reach("usb", 100100000, 100000, 30000000, true);     /* a converter, 100 MHz up */
    reach("usb", 99999000, 100000, 30000000, false);
    reach("usb", 130000001, 100000, 30000000, false);
    CHECK(!kiwi_tune_reaches(NULL, 0, 30000000));
    CASE("tune: in CW the dial reaches, as the range it says it covers has it -- not its carrier");
    kiwi_tune_t t = { .hz = 30000400, .mode = "cw" };
    CHECK(!kiwi_tune_reaches(&t, 0, 30000000));          /* past its top, though its carrier is not */
    t.hz = 30000000;
    CHECK(kiwi_tune_reaches(&t, 0, 30000000));
    t.hz = 144000300;                                    /* just over a converter's bottom... */
    CHECK(kiwi_tune_reaches(&t, 144000, 30000000));
    tune("cw", 144000300, -200, 200, 144000, 30000000, 12000,   /* ...its carrier kept there */
         "SET mod=cw low_cut=300 high_cut=700 freq=0.000");
    CASE("tune: the passband within the audio's band");
    tune("nbfm", 29600000, -6000, 6000, 0, 30000000, 12000,
         "SET mod=nbfm low_cut=-5999 high_cut=5999 freq=29600.000");
    tune("nbfm", 29600000, -6000, 6000, 0, 30000000, 24000,
         "SET mod=nbfm low_cut=-6000 high_cut=6000 freq=29600.000");
    CASE("tune: a radio's names, the Kiwi's own");
    tune("fm", 29600000, 0, 0, 0, 0, 0, "SET mod=nbfm low_cut=-5999 high_cut=5999 freq=29600.000");
    tune("am", 6000000, 0, 0, 0, 0, 0, "SET mod=am low_cut=-4900 high_cut=4900 freq=6000.000");
    tune("sal", 6000000, -4900, 0, 0, 0, 0, "SET mod=sal low_cut=-4900 high_cut=0 freq=6000.000");
    CHECK_STR(kiwi_mode_of("cwu"), "cw");
    CHECK_STR(kiwi_mode_of("CWL"), "cw");
    CHECK_STR(kiwi_mode_of("digu"), "usb");
    CHECK_STR(kiwi_mode_of("rtty"), "usb");
    CHECK_STR(kiwi_mode_of("digl"), "lsb");
    CHECK_STR(kiwi_mode_of("wfm"), "nbfm");
    CHECK_STR(kiwi_mode_of("nfm"), "nbfm");
    CHECK_STR(kiwi_mode_of("SAM"), "sam");
    CHECK_STR(kiwi_mode_of("usn"), "usn");
    CHECK_STR(kiwi_mode_of("amw"), "am");
    CHECK_STR(kiwi_mode_of(NULL), "usb");
    CHECK(kiwi_mode_find("iq") == NULL);
    CHECK(kiwi_mode_find("drm") == NULL);
    CHECK_EQ(kiwi_mode_find("cw")->lo, -200);
}

/* ------------------------------------------------------ where it centres CW */

/* load_cfg as UberSDR's Kiwi input sends it (Go's escapes, upper case,
 * compact: tools/mock_ubersdr.py), and as the mocks have a KiwiSDR send it --
 * lower case, a space after each colon and comma, "pbw" in the object -- its
 * owner's CW at 300..700 and at 400..800. None captured from a receiver. */
static const char CFG_UBER[] =
    "MSG load_cfg=%7B%22passbands%22%3A%7B%22am%22%3A%7B%22lo%22%3A-4900%2C%22hi%22%3A4900%7D%2C%22lsb%22%3A"
    "%7B%22lo%22%3A-2400%2C%22hi%22%3A-300%7D%2C%22usb%22%3A%7B%22lo%22%3A300%2C%22hi%22%3A2400%7D%2C%22cw%22"
    "%3A%7B%22lo%22%3A-400%2C%22hi%22%3A400%7D%2C%22cwn%22%3A%7B%22lo%22%3A-250%2C%22hi%22%3A250%7D%2C%22nbfm"
    "%22%3A%7B%22lo%22%3A-6000%2C%22hi%22%3A6000%7D%7D%2C%22rx_grid%22%3A%22JO11%22%2C%22init%22%3A%7B%22freq"
    "%22%3A7020%2C%22mode%22%3A%22cw%22%2C%22zoom%22%3A0%7D%7D";
static const char CFG_KIWI[] =
    "MSG load_cfg=%7b%22passbands%22%3a%20%7b%22am%22%3a%20%7b%22lo%22%3a%20-4900%2c%20%22hi%22%3a%204900%7d"
    "%2c%20%22cw%22%3a%20%7b%22lo%22%3a%20300%2c%20%22hi%22%3a%20700%2c%20%22pbw%22%3a%20400%7d%2c%20%22cwn%22"
    "%3a%20%7b%22lo%22%3a%20470%2c%20%22hi%22%3a%20530%7d%2c%20%22usb%22%3a%20%7b%22lo%22%3a%20300%2c%20%22hi"
    "%22%3a%202700%7d%7d%2c%20%22ext_api_nchans%22%3a%204%2c%20%22init%22%3a%20%7b%22freq%22%3a%207020%2c%20"
    "%22mode%22%3a%20%22cw%22%7d%7d";
static const char CFG_KIWI600[] =
    "MSG load_cfg=%7b%22passbands%22%3a%20%7b%22am%22%3a%20%7b%22lo%22%3a%20-4900%2c%20%22hi%22%3a%204900%7d"
    "%2c%20%22cw%22%3a%20%7b%22lo%22%3a%20400%2c%20%22hi%22%3a%20800%2c%20%22pbw%22%3a%20400%7d%2c%20%22cwn%22"
    "%3a%20%7b%22lo%22%3a%20470%2c%20%22hi%22%3a%20530%7d%7d%7d";

/* What the reader makes of `msg`, fed `piece` bytes at a time (0: whole). */
static int32_t cw_of(const char *msg, size_t piece)
{
    kiwi_cw_t c;
    kiwi_cw_init(&c);
    const size_t n = strlen(msg);
    if (!piece) piece = n ? n : 1;
    for (size_t i = 0; i < n; i += piece)
        kiwi_cw_feed(&c, (const uint8_t *)msg + i, n - i < piece ? n - i : piece);
    return kiwi_cw_centre(&c, -9999);
}

/* The same, in every piece size: the frames and reads break it anywhere. */
static bool cw_every(const char *msg, int32_t want)
{
    for (size_t piece = 0; piece <= strlen(msg); piece++)
        if (cw_of(msg, piece) != want) {
            fprintf(stderr, "  in pieces of %zu: %ld, not %ld\n", piece, (long)cw_of(msg, piece), (long)want);
            return false;
        }
    return true;
}

static void test_cw_centre(void)
{
    CASE("CW's centre: an UberSDR's Kiwi input, on the carrier");
    CHECK_EQ(cw_of(CFG_UBER, 0), 0);
    CHECK(cw_every(CFG_UBER, 0));
    CASE("CW's centre: a KiwiSDR's, 300..700 as it comes");
    CHECK_EQ(cw_of(CFG_KIWI, 0), 500);
    CHECK(cw_every(CFG_KIWI, 500));
    CASE("CW's centre: one its owner set at 400..800");
    CHECK_EQ(cw_of(CFG_KIWI600, 0), 600);
    CHECK(cw_every(CFG_KIWI600, 600));
    CASE("CW's centre: JSON not escaped, and a broken escape before it");
    CHECK_EQ(cw_of("MSG load_cfg={\"passbands\": {\"cw\": {\"lo\": 250, \"hi\": 750}}}", 0), 500);
    CHECK(cw_every("MSG load_cfg=%zz%4%%7B%22cw%22%3A%7B%22lo%22%3A-100%2C%22hi%22%3A100%7D%7D", 0));
    CHECK(cw_every("MSG load_cfg=%22cw%22%3A%7B%22hi%22%3A700%2C%22lo%22%3A300%7D", 500));   /* either order */

    CASE("CW's centre: said nothing of it");
    CHECK_EQ(cw_of("MSG load_cfg=%7b%22ext_api_nchans%22%3a%204%2c%22rx_chans%22%3a%2013%7d", 0), -9999);
    CHECK_EQ(cw_of("MSG load_cfg=", 0), -9999);
    CHECK_EQ(cw_of("MSG load_cfg=%7b%22cw%22%3a%20%7b%22lo%22%3a%20300", 0), -9999);       /* cut short */
    CHECK_EQ(cw_of("", 0), -9999);
    CASE("CW's centre: only load_cfg's");
    CHECK(cw_every("MSG load_dxcfg=%7b%22cw%22%3a%7b%22lo%22%3a300%2c%22hi%22%3a700%7d%7d", -9999));
    CHECK(cw_every("MSG rx_chans=13 cw={\"lo\":300,\"hi\":700}", -9999));
    CHECK(cw_every("load_cfg=%7b%22cw%22%3a%7b%22lo%22%3a300%2c%22hi%22%3a700%7d%7d", -9999));
    CASE("CW's centre: CWN is not CW");
    CHECK(cw_every("MSG load_cfg=%7b%22cwn%22%3a%7b%22lo%22%3a470%2c%22hi%22%3a530%7d%7d", -9999));
    CHECK(cw_every("MSG load_cfg={\"cwn\":{\"lo\":470,\"hi\":530},\"cw\":{\"lo\":100,\"hi\":900}}", 500));
    CASE("CW's centre: the first \"cw\" object counts");
    CHECK_EQ(cw_of("MSG load_cfg={\"cw\":{\"lo\":-400,\"hi\":400},\"x\":{\"cw\":{\"lo\":300,\"hi\":700}}}", 0), 0);
    CASE("CW's centre: nothing sane is taken");
    CHECK_EQ(cw_of("MSG load_cfg={\"cw\":{\"lo\":700,\"hi\":300}}", 0), -9999);            /* the wrong way up */
    CHECK_EQ(cw_of("MSG load_cfg={\"cw\":{\"lo\":500,\"hi\":500}}", 0), -9999);
    CHECK_EQ(cw_of("MSG load_cfg={\"cw\":{\"lo\":3000,\"hi\":4000}}", 0), -9999);          /* 3500 Hz */
    CHECK_EQ(cw_of("MSG load_cfg={\"cw\":{\"lo\":-3000,\"hi\":-1200}}", 0), -9999);
    CHECK_EQ(cw_of("MSG load_cfg={\"cw\":{\"lo\":1000,\"hi\":2000}}", 0), 1500);           /* the highest */
    CHECK_EQ(cw_of("MSG load_cfg={\"cw\":{\"lo\":-1100,\"hi\":-900}}", 0), -1000);         /* the lowest */
    CHECK_EQ(cw_of("MSG load_cfg={\"cw\":{\"lo\":-99999999999999999999,\"hi\":99999999999999999999}}", 0),
             -9999);
    CHECK_EQ(cw_of("MSG load_cfg={\"cw\":{\"lo\":\"300\",\"hi\":700}}", 0), -9999);       /* no number */
    CHECK_EQ(cw_of("MSG load_cfg={\"cw\":{\"hi\":700}}", 0), -9999);
    CASE("CW's centre: an object longer than kept, a number cut");
    CHECK_EQ(cw_of("MSG load_cfg={\"cw\":{\"lo\":300,\"hi\":700,\"something_else_entirely_long\":1}}", 0), 500);
    CHECK_EQ(cw_of("MSG load_cfg={\"cw\":{\"something_else_entirely_long_too\":1,\"lo\":300,\"hi\":700}}", 0),
             -9999);
    /* 47 characters kept, the last "hi":4 of "hi":400: no -198 Hz from it. */
    CHECK_EQ(strlen("\"aaaaaaaaaaaaaaaaaaaaaaaaaa\":1,\"lo\":-400,\"hi\":4"), 47);
    CHECK_EQ(cw_of("MSG load_cfg={\"cw\":{\"aaaaaaaaaaaaaaaaaaaaaaaaaa\":1,\"lo\":-400,\"hi\":400}}", 0), -9999);

    CASE("CW's centre: a session's, until it says");
    kiwi_said_t s;
    kiwi_said_init(&s);
    CHECK_EQ(s.cw_hz, KIWI_CW_PITCH);
    kiwi_cw_t c;
    kiwi_cw_init(&c);
    CHECK_EQ(kiwi_cw_centre(&c, s.cw_hz), KIWI_CW_PITCH);
    kiwi_cw_feed(&c, (const uint8_t *)CFG_UBER, 40);
    CHECK_EQ(kiwi_cw_centre(&c, s.cw_hz), KIWI_CW_PITCH);                    /* not that far yet */
    kiwi_cw_feed(&c, (const uint8_t *)CFG_UBER + 40, strlen(CFG_UBER) - 40);
    CHECK_EQ(kiwi_cw_centre(&c, s.cw_hz), 0);
    kiwi_cw_feed(&c, (const uint8_t *)CFG_KIWI600, strlen(CFG_KIWI600));    /* read: nothing more taken */
    CHECK_EQ(kiwi_cw_centre(&c, s.cw_hz), 0);
    kiwi_cw_feed(NULL, (const uint8_t *)CFG_UBER, 4);
    kiwi_cw_feed(&c, NULL, 4);
    CHECK_EQ(kiwi_cw_centre(NULL, 7), 7);
}

static void test_settings(void)
{
    char out[96];
    CASE("AGC: the Kiwi's own presets");
    kiwi_agc_cmd(out, sizeof out, KIWI_AGC_MED, false);
    CHECK_STR(out, "SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50");
    kiwi_agc_cmd(out, sizeof out, KIWI_AGC_FAST, true);
    CHECK_STR(out, "SET agc=1 hang=0 thresh=-130 slope=6 decay=250 manGain=50");
    kiwi_agc_cmd(out, sizeof out, KIWI_AGC_SLOW, false);
    CHECK_STR(out, "SET agc=1 hang=0 thresh=-100 slope=6 decay=3000 manGain=50");
    CASE("squelch");
    kiwi_squelch_cmd(out, sizeof out, 0, false);
    CHECK_STR(out, "SET squelch=0 param=0.50");
    kiwi_squelch_cmd(out, sizeof out, 100, true);
    CHECK_STR(out, "SET squelch=99 param=0.00");
    kiwi_squelch_cmd(out, sizeof out, 50, false);
    CHECK_STR(out, "SET squelch=20 param=0.50");
    CASE("noise filter");
    CHECK(kiwi_nr_cmd(out, sizeof out, KIWI_NR_OFF, 0));
    CHECK_STR(out, "SET nr algo=0");
    CHECK(!kiwi_nr_cmd(out, sizeof out, KIWI_NR_OFF, 1));
    int n = 0;
    while (kiwi_nr_cmd(out, sizeof out, KIWI_NR_LMS, n)) n++;
    CHECK_EQ(n, 11);
    kiwi_nr_cmd(out, sizeof out, KIWI_NR_LMS, 7);
    CHECK_STR(out, "SET nr type=1 param=2 pval=0.99915");
    kiwi_nr_cmd(out, sizeof out, KIWI_NR_WDSP, 9);
    CHECK_STR(out, "SET nr type=0 en=1");
    kiwi_nr_cmd(out, sizeof out, KIWI_NR_SPEC, 10);
    CHECK_STR(out, "SET nr type=1 en=0");
}

static kiwi_end_t said1(kiwi_said_t *s, const char *msg) { return kiwi_said(s, msg, false); }

static void test_said(void)
{
    kiwi_said_t s;
    char v[16];
    CASE("MSG values");
    CHECK(kiwi_msg_val("center_freq=15000000 bandwidth=30000000", "bandwidth", v, sizeof v));
    CHECK_STR(v, "30000000");
    CHECK(!kiwi_msg_val("chan_no_pwd_true=2", "chan_no_pwd", v, sizeof v));
    CHECK(kiwi_msg_val("down", "down", v, sizeof v));
    CHECK_STR(v, "");
    CHECK(!kiwi_msg_val("updown=1", "down", v, sizeof v));

    CASE("said: a login, and what follows it");
    kiwi_said_init(&s);
    CHECK_EQ(said1(&s, "rx_chans=13"), KIWI_END_NONE);
    CHECK_EQ(said1(&s, "chan_no_pwd=0"), KIWI_END_NONE);
    CHECK_EQ(said1(&s, "badp=0"), KIWI_END_NONE);
    CHECK(s.logged_in);
    said1(&s, "center_freq=31000000 bandwidth=62000000 adc_clk_nom=122880000");
    said1(&s, "audio_init=0 audio_rate=12000 sample_rate=12001.135");
    said1(&s, "version_maj=2026 version_min=901");
    said1(&s, "freq_offset=100000.000");
    CHECK_EQ(s.bw_hz, 62000000);
    CHECK_EQ(s.audio_rate, 12000);
    CHECK_EQ((int)(s.rate * 1000), 12001135);
    CHECK_EQ(s.version_maj, 2026);
    CHECK_EQ((int)s.offset_khz, 100000);
    CHECK_EQ(s.rx_chans, 13);

    CASE("said: the refusals");
    kiwi_said_init(&s);
    CHECK_EQ(said1(&s, "badp=1"), KIWI_END_PASSWORD);           /* no word of free channels */
    kiwi_said_init(&s);
    said1(&s, "chan_no_pwd=3");
    CHECK_EQ(said1(&s, "badp=1"), KIWI_END_PWD_FULL);           /* no password, its free ones taken */
    kiwi_said_init(&s);
    said1(&s, "chan_no_pwd=3");
    CHECK_EQ(kiwi_said(&s, "badp=1", true), KIWI_END_PASSWORD); /* a password, wrong */
    kiwi_said_init(&s);
    CHECK_EQ(said1(&s, "badp=2"), KIWI_END_TRY_LATER);
    CHECK_EQ(said1(&s, "badp=3"), KIWI_END_REFUSED);
    CHECK_EQ(said1(&s, "badp=4"), KIWI_END_TRY_LATER);
    CHECK_EQ(said1(&s, "badp=5"), KIWI_END_DUP_IP);
    CHECK_EQ(said1(&s, "badp=6"), KIWI_END_UPDATING);
    CHECK_EQ(said1(&s, "badp=7"), KIWI_END_TRY_LATER);
    CHECK_EQ(said1(&s, "redirect=http%3a%2f%2fother"), KIWI_END_FULL);
    CHECK_EQ(said1(&s, "reason_disabled= down=1"), KIWI_END_DOWN);

    CASE("said: too_busy, before and after the login");
    kiwi_said_init(&s);
    CHECK_EQ(said1(&s, "too_busy=8"), KIWI_END_FULL);
    kiwi_said_init(&s);
    said1(&s, "rx_chans=8");
    said1(&s, "badp=0");
    CHECK_EQ(said1(&s, "too_busy=0"), KIWI_END_NO_APPS);
    CHECK_EQ(said1(&s, "too_busy=4"), KIWI_END_APPS_FULL);
    CHECK_EQ(said1(&s, "too_busy=8"), KIWI_END_FULL);

    CASE("said: the day limit, at the login and in it");
    kiwi_said_init(&s);
    CHECK_EQ(said1(&s, "ip_limit=60%2c81.83.21.23"), KIWI_END_DAY_LIMIT);
    CHECK(s.limit_at_login);
    kiwi_said_init(&s);
    said1(&s, "badp=0");
    CHECK_EQ(said1(&s, "ip_limit=60%2c81.83.21.23"), KIWI_END_DAY_LIMIT);
    CHECK(!s.limit_at_login);

    CASE("said: time up, kicked");
    kiwi_said_init(&s);
    said1(&s, "badp=0");
    CHECK_EQ(said1(&s, "inactivity_timeout=30"), KIWI_END_IDLE);
    CHECK_EQ(said1(&s, "kiwi_kick=0,Kicked%20by%20the%20admin"), KIWI_END_KICKED);
    CHECK_STR(s.kick, "Kicked by the admin");
    CHECK_EQ(said1(&s, "kiwi_kick=Gone%20for%20now"), KIWI_END_KICKED);
    CHECK_STR(s.kick, "Gone for now");

    CASE("quiet: no answer to the login");
    kiwi_said_init(&s);
    CHECK_EQ(kiwi_quiet(&s, KIWI_KIND_KIWISDR, 4, false), KIWI_END_SILENT);
    CHECK_EQ(kiwi_quiet(&s, KIWI_KIND_UNKNOWN, -1, false), KIWI_END_SILENT);
    CHECK_EQ(kiwi_quiet(&s, KIWI_KIND_KIWISDR, 0, false), KIWI_END_NO_APPS);
    CHECK_EQ(kiwi_quiet(&s, KIWI_KIND_WEB888, 4, false), KIWI_END_NO_ANSWER);
    CHECK_EQ(kiwi_quiet(&s, KIWI_KIND_KIWISDR, 4, true), KIWI_END_NO_ANSWER);
    said1(&s, "rx_chans=8");
    CHECK_EQ(kiwi_quiet(&s, KIWI_KIND_KIWISDR, 4, false), KIWI_END_NO_ANSWER);
    said1(&s, "badp=0");
    CHECK_EQ(kiwi_quiet(&s, KIWI_KIND_KIWISDR, 4, true), KIWI_END_CLOSED);
    CHECK_EQ(kiwi_quiet(&s, KIWI_KIND_KIWISDR, 4, false), KIWI_END_QUIET);

    CASE("HTTP");
    CHECK_EQ(kiwi_http(101), KIWI_END_NONE);
    CHECK_EQ(kiwi_http(0), KIWI_END_NO_ANSWER);
    CHECK_EQ(kiwi_http(400), KIWI_END_NOT_KIWI);
    CHECK_EQ(kiwi_http(403), KIWI_END_REFUSED);
    CHECK_EQ(kiwi_http(404), KIWI_END_NOT_KIWI);
    CHECK_EQ(kiwi_http(429), KIWI_END_FULL);
    CHECK_EQ(kiwi_http(500), KIWI_END_DOWN);
    CHECK_EQ(kiwi_http(503), KIWI_END_FULL);
    /* A redirect, once not followed, is said as one -- never "not a kiwi". */
    CHECK_EQ(kiwi_http(301), KIWI_END_MOVED);
    CHECK_EQ(kiwi_http(302), KIWI_END_MOVED);
    CHECK_EQ(kiwi_http(303), KIWI_END_MOVED);
    CHECK_EQ(kiwi_http(307), KIWI_END_MOVED);
    CHECK_EQ(kiwi_http(308), KIWI_END_MOVED);
    CHECK_EQ(kiwi_http(304), KIWI_END_NOT_KIWI);
    CHECK(kiwi_redirect_code(301) && kiwi_redirect_code(302) && kiwi_redirect_code(307) && kiwi_redirect_code(308));
    CHECK(!kiwi_redirect_code(303) && !kiwi_redirect_code(300) && !kiwi_redirect_code(200));

    CASE("the face's words fit");
    for (int e = KIWI_END_NONE; e <= KIWI_END_LAST; e++) {
        CHECK(strlen(kiwi_end_word((kiwi_end_t)e)) <= 15);
        CHECK(strlen(kiwi_end_note((kiwi_end_t)e)) <= 11);
        CHECK(e <= KIWI_END_WANT || kiwi_end_word((kiwi_end_t)e)[0]);       /* every end says why */
    }
    CHECK_STR(kiwi_end_word(KIWI_END_SILENT), "APPS FULL");
    CHECK_STR(kiwi_end_word(KIWI_END_PWD_FULL), "RECEIVER FULL");
    CHECK_STR(kiwi_end_word(KIWI_END_NO_FLASH), "MEMORY FULL");
    CHECK_STR(kiwi_end_note(KIWI_END_NO_FLASH), "memory full");
    CHECK_STR(kiwi_end_word(KIWI_END_MOVED), "MOVED");
    CHECK_STR(kiwi_end_note(KIWI_END_MOVED), "moved");
    CHECK_STR(kiwi_end_word(KIWI_END_CERT), "CERTIFICATE?");
    CHECK_STR(kiwi_end_note(KIWI_END_CERT), "certificate");
    CHECK_STR(kiwi_end_word((kiwi_end_t)(KIWI_END_LAST + 1)), "NO ANSWER");    /* past the table */
    /* Kept through a crash by their numbers (kiwi_boot_hold_put): new ones
     * only ever after the last. */
    CHECK_EQ(KIWI_END_NO_FLASH, 24);
    CHECK_EQ(KIWI_END_MOVED, 25);
    CHECK_EQ(KIWI_END_CERT, 26);
}

static void test_addresses(void)
{
    char h[64];
    uint16_t port = 0;
    bool tls = true;

    CASE("an address as the page takes it");
    CHECK(kiwi_url("kiwi.example.org", 8073, h, sizeof h, &port, &tls));
    CHECK_STR(h, "kiwi.example.org");
    CHECK_EQ(port, 8073);
    CHECK(!tls);
    CHECK(kiwi_url("81.83.21.23:8077", 8073, h, sizeof h, &port, &tls));
    CHECK_STR(h, "81.83.21.23");
    CHECK_EQ(port, 8077);
    CHECK(!tls);
    CHECK(kiwi_url("  https://kiwisdr.on3rvh.be/", 8073, h, sizeof h, &port, &tls));
    CHECK_STR(h, "kiwisdr.on3rvh.be");
    CHECK_EQ(port, 443);
    CHECK(tls);
    CHECK(kiwi_url("HTTPS://n0bqv.proxy.kiwisdr.com:8443/kiwi/?f=7074usb", 8073, h, sizeof h, &port, &tls));
    CHECK_STR(h, "n0bqv.proxy.kiwisdr.com");
    CHECK_EQ(port, 8443);
    CHECK(tls);
    CHECK(kiwi_url("http://n0bqv.proxy.kiwisdr.com/", 8073, h, sizeof h, &port, &tls));
    CHECK_STR(h, "n0bqv.proxy.kiwisdr.com");
    CHECK_EQ(port, 80);
    CHECK(!tls);
    CHECK(kiwi_url("http://kiwi.example.org:8073", 8073, h, sizeof h, &port, &tls));
    CHECK_EQ(port, 8073);
    CHECK(!tls);
    CHECK(kiwi_url("kiwi.example.org?f=7", 8073, h, sizeof h, &port, &tls));
    CHECK_STR(h, "kiwi.example.org");
    CHECK(!kiwi_url("", 8073, h, sizeof h, &port, &tls));
    CHECK(!kiwi_url("https://", 8073, h, sizeof h, &port, &tls));
    CHECK(!kiwi_url("https://:443/", 8073, h, sizeof h, &port, &tls));
    CHECK(!kiwi_url("host:0", 8073, h, sizeof h, &port, &tls));
    CHECK(!kiwi_url("host:65536", 8073, h, sizeof h, &port, &tls));
    CHECK(!kiwi_url("host:x", 8073, h, sizeof h, &port, &tls));
    CHECK(!kiwi_url(NULL, 8073, h, sizeof h, &port, &tls));
    char small[8];
    CHECK(!kiwi_url("https://kiwi.example.org", 8073, small, sizeof small, &port, &tls));    /* too long */
    /* A Location wants a scheme: a path alone is none of these. */
    CHECK(!kiwi_url("/status", 0, h, sizeof h, &port, &tls));
    CHECK(!kiwi_url("kiwi.example.org", 0, h, sizeof h, &port, &tls));

    CASE("the Host a front routes on");
    kiwi_host_hdr(h, sizeof h, "kiwisdr.on3rvh.be", 443, true);
    CHECK_STR(h, "kiwisdr.on3rvh.be");
    kiwi_host_hdr(h, sizeof h, "kiwisdr.on3rvh.be", 8443, true);
    CHECK_STR(h, "kiwisdr.on3rvh.be:8443");
    kiwi_host_hdr(h, sizeof h, "n0bqv.proxy.kiwisdr.com", 80, false);
    CHECK_STR(h, "n0bqv.proxy.kiwisdr.com");
    kiwi_host_hdr(h, sizeof h, "81.83.21.23", 8077, false);
    CHECK_STR(h, "81.83.21.23:8077");
    kiwi_host_hdr(h, sizeof h, "kiwi.example.org", 443, false);          /* 443 in the clear: said */
    CHECK_STR(h, "kiwi.example.org:443");

    CASE("a header in an answer's head");
    const char head[] = "HTTP/1.1 307 Temporary Redirect\r\nDate: Tue, 06 Oct 2026 08:00:00 GMT\r\n"
                        "location:   https://n0bqv.proxy.kiwisdr.com/  \r\nServer: x\r\n\r\nLocation: nope";
    CHECK(kiwi_http_header(head, "Location", h, sizeof h));
    CHECK_STR(h, "https://n0bqv.proxy.kiwisdr.com/");
    CHECK(kiwi_http_header(head, "server", h, sizeof h));
    CHECK_STR(h, "x");
    CHECK(!kiwi_http_header(head, "Content-Length", h, sizeof h));
    CHECK(!kiwi_http_header("HTTP/1.1 301 Moved\nLocation: /x\n\nbody", "Content-Type", h, sizeof h));
    CHECK(kiwi_http_header("HTTP/1.1 301 Moved\nLocation: /x\n\nbody", "Location", h, sizeof h));
    CHECK_STR(h, "/x");
    CHECK(!kiwi_http_header("HTTP/1.1 301 Moved\r\nLocationX: /x\r\n\r\n", "Location", h, sizeof h));
    CHECK(!kiwi_http_header("Location: /x", "Location", h, sizeof h));   /* the status line is no header */
    char tiny[4];
    CHECK(kiwi_http_header(head, "Location", tiny, sizeof tiny));
    CHECK_STR(tiny, "htt");

    CASE("a redirect: followed only to https:// on its own host, from the clear");
    char to[64];
    port = 0;
    CHECK(kiwi_redirect("https://n0bqv.proxy.kiwisdr.com/", "n0bqv.proxy.kiwisdr.com", false, &port, to, sizeof to));
    CHECK_EQ(port, 443);
    CHECK_STR(to, "");
    CHECK(kiwi_redirect("https://KiwiSDR.on3rvh.BE/status", "kiwisdr.on3rvh.be", false, &port, to, sizeof to));
    CHECK_EQ(port, 443);
    CHECK(kiwi_redirect("https://127.0.0.1:8443/12345/SND", "127.0.0.1", false, &port, to, sizeof to));
    CHECK_EQ(port, 8443);
    port = 7;
    CHECK(!kiwi_redirect("https://elsewhere.example/", "kiwisdr.on3rvh.be", false, &port, to, sizeof to));
    CHECK_STR(to, "elsewhere.example");
    CHECK_EQ(port, 7);                                                   /* untouched */
    CHECK(!kiwi_redirect("http://kiwisdr.on3rvh.be:8073/", "kiwisdr.on3rvh.be", false, &port, to, sizeof to));
    CHECK_STR(to, "kiwisdr.on3rvh.be");                                  /* in the clear: not followed */
    CHECK(!kiwi_redirect("/kiwi/", "kiwisdr.on3rvh.be", false, &port, to, sizeof to));
    CHECK_STR(to, "kiwisdr.on3rvh.be");                                  /* a path alone: its own host */
    CHECK(!kiwi_redirect("https://kiwisdr.on3rvh.be/", "kiwisdr.on3rvh.be", true, &port, to, sizeof to));
    CHECK_STR(to, "kiwisdr.on3rvh.be");                                  /* over TLS already: never a second */
    CHECK(!kiwi_redirect("https://localhost:8443/", "127.0.0.1", false, &port, to, sizeof to));
    CHECK_STR(to, "localhost");                                          /* another name is another host */
    CASE("a redirect not followed: where it points, said as far as it can be");
    CHECK(!kiwi_redirect("", "h.example", false, &port, to, sizeof to));
    CHECK_STR(to, "");                                                   /* no Location: nowhere said */
    CHECK(!kiwi_redirect(NULL, "h.example", false, &port, to, sizeof to));
    CHECK_STR(to, "");
    CHECK(!kiwi_redirect("  ", "h.example", false, &port, to, sizeof to));
    CHECK_STR(to, "");
    CHECK(!kiwi_redirect("//other.example/kiwi/", "h.example", false, &port, to, sizeof to));
    CHECK_STR(to, "other.example");                                      /* no scheme: its host all the same */
    CHECK(!kiwi_redirect("//h.example/", "h.example", false, &port, to, sizeof to));
    CHECK_STR(to, "h.example");                                          /* ...never followed without https:// */
    CHECK(!kiwi_redirect("ftp://files.example:21/x", "h.example", false, &port, to, sizeof to));
    CHECK_STR(to, "files.example");                                      /* another scheme: its host */
    CHECK(!kiwi_redirect("kiwi/", "h.example", false, &port, to, sizeof to));
    CHECK_STR(to, "h.example");                                          /* a relative path: its own host */
    {
        /* A host too long to follow: the start of it, never the receiver's own. */
        char loc[160], lng[100];
        memset(lng, 'a', 80);
        memcpy(lng + 80, ".example.org", 13);
        snprintf(loc, sizeof loc, "https://%s/", lng);
        port = 9;
        CHECK(!kiwi_redirect(loc, lng, false, &port, to, sizeof to));
        CHECK_EQ(strlen(to), sizeof to - 1);
        CHECK(!strncmp(to, lng, sizeof to - 1));
        CHECK_EQ(port, 9);
        char small[8];
        CHECK(!kiwi_redirect("https://elsewhere.example/", "h.example", false, &port, small, sizeof small));
        CHECK_STR(small, "elsewhe");
    }
}

static void test_audio(void)
{
    CASE("S-meter: each its own bias");
    CHECK_EQ((int)(kiwi_dbm(540, false) * 10), -730);
    CHECK_EQ((int)(kiwi_dbm(670, true) * 10), -730);

    CASE("SND frames");
    const uint8_t f[] = { 'S', 'N', 'D', 0x12, 1, 0, 0, 0, 0x02, 0x1C, 0xAA, 0xBB };
    kiwi_snd_t s;
    CHECK(kiwi_snd(f, sizeof f, &s));
    CHECK_EQ(s.flags, 0x12);
    CHECK_EQ(s.seq, 1);
    CHECK_EQ(s.smeter, 540);
    CHECK_EQ(s.off, 10);
    CHECK(!kiwi_snd(f, 9, &s));
    CHECK(!kiwi_snd((const uint8_t *)"MSG x=1  ", 10, &s));

    CASE("the rate: what a Kiwi has, nothing else");
    kiwi_dsp_t d;
    kiwi_dsp_init(&d);
    CHECK(kiwi_dsp_reset(&d, 12001.135));
    CHECK(kiwi_dsp_reset(&d, 48000));
    CHECK(!kiwi_dsp_reset(&d, 3999));
    CHECK(!kiwi_dsp_reset(&d, 96000));
    CHECK(!kiwi_dsp_reset(&d, 0.0 / 0.0));

    CASE("the resampler: 12 kHz to 24 kHz, frame after frame");
    static int16_t in[2048], out[KIWI_OUT_MAX];
    for (int i = 0; i < 2048; i++) in[i] = (int16_t)(i * 7);
    kiwi_dsp_init(&d);
    kiwi_dsp_reset(&d, 12000);
    long total = 0;
    for (int k = 0; k < 10; k++) {
        const int o = kiwi_resample(&d, in, 2048, out, KIWI_OUT_MAX);
        CHECK(o >= 4095 && o <= 4097);
        total += o;
    }
    CHECK_EQ(total, 40960);
    kiwi_dsp_reset(&d, 20250);
    total = 0;
    for (int k = 0; k < 10; k++) total += kiwi_resample(&d, in, 2048, out, KIWI_OUT_MAX);
    CHECK(total >= 24272 && total <= 24274);         /* 20480 * 24000 / 20250 */
    kiwi_dsp_reset(&d, 4000);
    CHECK(kiwi_resample(&d, in, 1024, out, KIWI_OUT_MAX) <= 6146);

    CASE("the resampler keeps its rate: no drift of its own, minutes on end");
    /* A float's place drifted -20 ppm at a KiwiSDR's 12001.135 Hz and -49 at
     * 4 kHz -- half a sample a second, which the ring has to swallow. The
     * fixed point's whole error is the step's last bit: an hour's is no more
     * than these few minutes'. */
    static const struct { double rate; int piece; long n; } RUN[] = {
        { 12001.135, 2048, 2000 }, { 12001.135, 1024, 4000 }, { 4000.0, 1024, 2800 },
        { 4000.0, 512, 5600 }, { 20250.0, 2048, 2000 }, { 48000.0, 1024, 2000 },
    };
    for (size_t r = 0; r < sizeof RUN / sizeof RUN[0]; r++) {
        kiwi_dsp_init(&d);
        kiwi_dsp_reset(&d, RUN[r].rate);
        long long got = 0;
        for (long k = 0; k < RUN[r].n; k++) got += kiwi_resample(&d, in, RUN[r].piece, out, KIWI_OUT_MAX);
        const double want = (double)RUN[r].n * RUN[r].piece * KIWI_OUT_HZ / RUN[r].rate;
        CHECK(fabs((double)got - want) <= 2.0);
    }

    CASE("ADPCM: silence stays silence");
    static const uint8_t zero[64];
    static int16_t pcm[128];
    kiwi_dsp_init(&d);
    CHECK_EQ(kiwi_adpcm(&d, zero, sizeof zero, pcm), 128);
}

/* A tone of `hz` at `rate` through the resampler, half a second in pieces
 * of 1024 as kiwi_snd_audio() hands them over, the trim set: its gain, and
 * what else came out with it (dB under the tone), from a least-squares fit
 * at the pitch it should have at 24 kHz -- a trimmed clock's, a hair higher. */
static void tone_through(double rate, double hz, float trim, double *gain_db, double *rest_db)
{
    static int16_t in[1024], out[KIWI_OUT_MAX];
    static double y[16000];
    kiwi_dsp_t d;
    kiwi_dsp_init(&d);
    kiwi_dsp_reset(&d, rate);
    kiwi_dsp_trim(&d, trim);
    size_t n = 0;
    long at = 0;
    while (at < (long)(rate / 2)) {
        for (int i = 0; i < 1024; i++, at++) in[i] = (int16_t)lrint(10000 * sin(2 * M_PI * hz / rate * at));
        const int o = kiwi_resample(&d, in, 1024, out, KIWI_OUT_MAX);
        for (int i = 0; i < o && n < sizeof y / sizeof y[0]; i++) y[n++] = out[i];
    }
    const double w = 2 * M_PI * hz * d.trim / KIWI_OUT_HZ;
    double cc = 0, ss = 0, cs = 0, yc = 0, ys = 0;
    const size_t skip = 400;                       /* the filters filling */
    for (size_t k = skip; k < n; k++) {
        const double c = cos(w * k), s = sin(w * k);
        cc += c * c; ss += s * s; cs += c * s; yc += y[k] * c; ys += y[k] * s;
    }
    const double det = cc * ss - cs * cs, a = (yc * ss - ys * cs) / det, b = (ys * cc - yc * cs) / det;
    double rest = 0;
    for (size_t k = skip; k < n; k++) {
        const double e = y[k] - a * cos(w * k) - b * sin(w * k);
        rest += e * e;
    }
    const double amp = sqrt(a * a + b * b);
    *gain_db = 20 * log10(amp / 10000);
    *rest_db = 10 * log10(rest / (n - skip) / (amp * amp / 2));
}

static void test_resampler(void)
{
    double g, r;
    CASE("the resampler: flat to 5 kHz at 12 kHz in, whatever the trim");
    static const float TRIMS[] = { 1.0f, 1.0013f, 0.998f, 1.002f };
    /* ...and closer where the half-band's edge is worst: 4.75-4.8 kHz, its
     * image at 7.2 (+0.07 dB, 42 dB down) */
    static const double EDGE[] = { 4600, 4700, 4750, 4800, 4850, 4900 };
    for (size_t t = 0; t < sizeof TRIMS / sizeof TRIMS[0]; t++) {
        for (size_t k = 0; k < 11 + sizeof EDGE / sizeof EDGE[0]; k++) {
            const double hz = k < 11 ? 300 + 470.0 * k : EDGE[k - 11];
            tone_through(12000, hz, TRIMS[t], &g, &r);
            CHECK(fabs(g) <= 0.5);
            CHECK(r <= -40.0);                     /* images and the interpolator's own */
            if (fabs(g) > 0.5 || r > -40.0)
                fprintf(stderr, "  12 kHz, trim %.4f, %.0f Hz: gain %.2f dB, rest %.1f dB\n", TRIMS[t], hz, g, r);
        }
    }

    CASE("the resampler: 1 kHz stays 1 kHz, its images 40 dB down, at every rate a Kiwi has");
    static const double RATES[] = { 4000, 8000, 12000, 12001.135, 16000, 20250, 24000, 36000, 48000, 50000 };
    for (size_t k = 0; k < sizeof RATES / sizeof RATES[0]; k++) {
        for (size_t t = 0; t < 2; t++) {
            tone_through(RATES[k], 1000, TRIMS[t], &g, &r);
            CHECK(fabs(g) <= 0.2);
            CHECK(r <= -45.0);
            if (fabs(g) > 0.2 || r > -45.0)
                fprintf(stderr, "  %.0f Hz in, trim %.4f, 1 kHz: gain %.2f dB, rest %.1f dB\n", RATES[k], TRIMS[t], g, r);
        }
        /* ...and the top of a voice's passband, where a rate gives room */
        if (RATES[k] >= 8000) {
            tone_through(RATES[k], 3000, 1.0013f, &g, &r);
            CHECK(fabs(g) <= 0.2);
            CHECK(r <= -40.0);
            if (fabs(g) > 0.2 || r > -40.0)
                fprintf(stderr, "  %.0f Hz in, 3 kHz: gain %.2f dB, rest %.1f dB\n", RATES[k], g, r);
        }
    }

    CASE("the resampler: the trim, within 0.2 %, and its length");
    static int16_t in[2048], out[KIWI_OUT_MAX];
    memset(in, 0, sizeof in);
    kiwi_dsp_t d;
    kiwi_dsp_init(&d);
    kiwi_dsp_trim(&d, 1.01f);
    CHECK(d.trim == 1.002f);
    kiwi_dsp_trim(&d, 0.5f);
    CHECK(d.trim == 0.998f);
    kiwi_dsp_trim(&d, 0.0f / 0.0f);
    CHECK(d.trim == 0.998f);
    /* 2048 at 12 kHz: 4096 out, 4096 / 1.002 with the trim at its top */
    kiwi_dsp_init(&d);
    long total = 0;
    for (int k = 0; k < 100; k++) total += kiwi_resample(&d, in, 2048, out, KIWI_OUT_MAX);
    CHECK_EQ(total, 409600);
    kiwi_dsp_trim(&d, 1.002f);
    total = 0;
    for (int k = 0; k < 100; k++) total += kiwi_resample(&d, in, 2048, out, KIWI_OUT_MAX);
    CHECK(fabs((double)total - 409600 / 1.002) <= 2.0);
    kiwi_dsp_reset(&d, 12000);                    /* a new rate keeps the trim */
    CHECK(d.trim == 1.002f);

    CASE("the resampler: the stages each rate takes");
    kiwi_dsp_reset(&d, 12000);
    CHECK(d.hb == 2 && !d.lp);
    kiwi_dsp_reset(&d, 4000);
    CHECK(d.hb == 3 && !d.lp);
    kiwi_dsp_reset(&d, 20250);
    CHECK(d.hb == 1 && !d.lp);
    kiwi_dsp_reset(&d, 24000);
    CHECK(d.hb == 1 && !d.lp);
    kiwi_dsp_reset(&d, 48000);
    CHECK(d.hb == 0 && d.lp);

    CASE("SND: the samples a frame carries");
    kiwi_snd_t f = { .flags = KIWI_SND_ADPCM, .off = 10 };
    CHECK_EQ(kiwi_snd_len(&f, 1034), 2048);
    f.flags = 0;
    CHECK_EQ(kiwi_snd_len(&f, 4106), 2048);
    f.flags = KIWI_SND_STEREO;
    CHECK_EQ(kiwi_snd_len(&f, 4106), 0);
    f.flags = 0;
    f.off = 20;
    CHECK_EQ(kiwi_snd_len(&f, 10), 0);
}

/* ------------------------------------------------------------ decoding */

/* kiwiclient's decoder (kiwi/client.py, ImaAdpcmDecoder), line for line: the
 * one every Kiwi app is measured against. */
static const int REF_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34,
    37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767 };
static const int REF_ADJ[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct { int index, prev; } ref_t;

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

static int16_t ref_sample(ref_t *r, int code)
{
    const int step = REF_STEP[r->index];
    r->index = clampi(r->index + REF_ADJ[code], 0, 88);
    int difference = step >> 3;
    if (code & 1) difference += step >> 2;
    if (code & 2) difference += step >> 1;
    if (code & 4) difference += step;
    if (code & 8) difference = -difference;
    r->prev = clampi(r->prev + difference, -32768, 32767);
    return (int16_t)r->prev;
}

static size_t ref_decode(ref_t *r, const uint8_t *in, size_t n, int16_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        out[o++] = ref_sample(r, in[i] & 0x0F);
        out[o++] = ref_sample(r, in[i] >> 4);
    }
    return o;
}

/* The receiver's side, as tools/mock_kiwi.py encodes (the standard IMA
 * encoder): two samples a byte, the low nibble first. */
typedef struct { int pred, idx; } enc_t;

static int enc_code(enc_t *e, int x)
{
    const int step = REF_STEP[e->idx];
    int diff = x - e->pred, c = 0, d = step >> 3;
    if (diff < 0) { c = 8; diff = -diff; }
    if (diff >= step)      { c |= 4; diff -= step;      d += step; }
    if (diff >= step >> 1) { c |= 2; diff -= step >> 1; d += step >> 1; }
    if (diff >= step >> 2) { c |= 1; d += step >> 2; }
    e->pred = clampi(c & 8 ? e->pred - d : e->pred + d, -32768, 32767);
    e->idx = clampi(e->idx + REF_ADJ[c], 0, 88);
    return c;
}

/* A tone, `hz` at `rate`, from sample `at`, encoded: n samples into n/2 bytes. */
static void enc_tone(enc_t *e, double hz, double rate, long at, size_t n, uint8_t *out, int16_t *pcm)
{
    for (size_t i = 0; i < n; i += 2) {
        const int a = (int)(8000 * sin(2 * M_PI * hz * (double)(at + (long)i) / rate));
        const int b = (int)(8000 * sin(2 * M_PI * hz * (double)(at + (long)i + 1) / rate));
        if (pcm) { pcm[i] = (int16_t)a; pcm[i + 1] = (int16_t)b; }
        const int lo = enc_code(e, a);          /* in order: the state runs on */
        const int hi = enc_code(e, b);
        out[i / 2] = (uint8_t)(lo | hi << 4);
    }
}

/* An SND frame: header, then data. */
static size_t snd_frame(uint8_t *f, uint8_t flags, uint32_t seq, uint16_t sm, const uint8_t *data, size_t n)
{
    memcpy(f, "SND", 3);
    f[3] = flags;
    f[4] = (uint8_t)seq; f[5] = (uint8_t)(seq >> 8); f[6] = (uint8_t)(seq >> 16); f[7] = (uint8_t)(seq >> 24);
    f[8] = (uint8_t)(sm >> 8); f[9] = (uint8_t)sm;
    memcpy(f + 10, data, n);
    return n + 10;
}

typedef struct { int16_t *buf; size_t n, cap; int calls; size_t total; } sink_t;

static void sink(void *ctx, const int16_t *pcm, size_t n)
{
    sink_t *s = ctx;
    s->calls++;
    s->total += n;
    for (size_t i = 0; i < n && s->n < s->cap; i++) s->buf[s->n++] = pcm[i];
}

static uint32_t rnd_state = 0x2468ACEu;
static uint32_t rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

#define FRAMES 6
#define FRAME_B 1024                    /* ADPCM: 2048 samples, as a Kiwi sends */

static void test_decode(void)
{
    static uint8_t bytes[8192], frame[10 + 8192];
    static int16_t want[16384], got[16384], pcm[KIWI_PCM_MAX], out[KIWI_OUT_MAX];
    kiwi_dsp_t d;

    CASE("ADPCM: kiwiclient's decoder, sample for sample, call after call");
    for (size_t i = 0; i < sizeof bytes; i++) bytes[i] = (uint8_t)rnd();
    ref_t r = { 0, 0 };
    kiwi_dsp_init(&d);
    size_t at = 0, bad = 0;
    while (at < sizeof bytes) {
        /* Pieces of every length, the state carried across them. */
        size_t n = 1 + rnd() % 700;
        if (n > sizeof bytes - at) n = sizeof bytes - at;
        const size_t wn = ref_decode(&r, bytes + at, n, want);
        const int gn = kiwi_adpcm(&d, bytes + at, n, got);
        CHECK_EQ(gn, (long long)wn);
        for (size_t i = 0; i < wn; i++) bad += got[i] != want[i];
        at += n;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(d.pred, r.prev);
    CHECK_EQ(d.idx, r.index);

    CASE("ADPCM: a tone through the receiver's encoder and back");
    enc_t e = { 0, 0 };
    enc_tone(&e, 1000, 12000, 0, 4096, bytes, want);
    kiwi_dsp_init(&d);
    CHECK_EQ(kiwi_adpcm(&d, bytes, 2048, got), 4096);
    int worst = 0;
    for (int i = 256; i < 4096; i++) {          /* once the step has grown to the tone */
        const int err = abs(got[i] - want[i]);
        if (err > worst) worst = err;
    }
    CHECK(worst < 800);                          /* 10 % of the tone's 8000 */

    CASE("SND: squelched frames between open ones -- the same audio, but for its silence");
    /* Six frames of a tone; then the same six with the third and fourth
     * squelched. Every frame is decoded either way, so the open ones after
     * the silence are exactly what they were. */
    static int16_t straight[FRAMES * 4200], quiet[FRAMES * 4200];
    size_t ends_a[FRAMES], ends_b[FRAMES];
    sink_t sa = { .buf = straight, .cap = FRAMES * 4200 }, sb = { .buf = quiet, .cap = FRAMES * 4200 };
    for (int run = 0; run < 2; run++) {
        enc_t en = { 0, 0 };
        kiwi_dsp_init(&d);
        kiwi_dsp_reset(&d, 12000);
        for (int k = 0; k < FRAMES; k++) {
            uint8_t data[FRAME_B];
            enc_tone(&en, 1000, 12000, (long)k * FRAME_B * 2, FRAME_B * 2, data, NULL);
            const uint8_t flags = KIWI_SND_ADPCM | (run && (k == 2 || k == 3) ? KIWI_SND_SQUELCH : 0);
            const size_t fl = snd_frame(frame, flags, (uint32_t)k, 540, data, sizeof data);
            kiwi_snd_t f;
            CHECK(kiwi_snd(frame, fl, &f));
            CHECK_EQ(kiwi_snd_audio(&d, &f, frame, fl, true, pcm, out, sink, run ? &sb : &sa), FRAME_B * 2);
            (run ? ends_b : ends_a)[k] = (run ? sb : sa).n;
        }
    }
    CHECK_EQ(sa.n, sb.n);
    CHECK(sa.n >= FRAMES * 4096 - 1 && sa.n <= FRAMES * 4096 + 1);
    size_t diff = 0, loud = 0;
    for (size_t i = 0; i < sa.n && i < sb.n; i++) {
        const bool hushed = i >= ends_a[1] && i < ends_a[3];
        if (hushed) loud += quiet[i] != 0;
        else        diff += quiet[i] != straight[i];
    }
    CHECK_EQ(ends_a[3], ends_b[3]);
    CHECK_EQ(loud, 0);
    CHECK_EQ(diff, 0);

    CASE("SND: before the first tune, decoded and not played");
    {
        enc_t en = { 0, 0 };
        uint8_t data[FRAME_B];
        enc_tone(&en, 1000, 12000, 0, FRAME_B * 2, data, NULL);
        const size_t fl = snd_frame(frame, KIWI_SND_ADPCM, 0, 540, data, sizeof data);
        kiwi_snd_t f;
        kiwi_snd(frame, fl, &f);
        sink_t s0 = { .buf = got, .cap = sizeof got / sizeof got[0] };
        kiwi_dsp_init(&d);
        CHECK_EQ(kiwi_snd_audio(&d, &f, frame, fl, false, pcm, out, sink, &s0), FRAME_B * 2);
        CHECK_EQ(s0.calls, 0);
        ref_t rr = { 0, 0 };
        ref_decode(&rr, data, sizeof data, want);
        CHECK_EQ(d.pred, rr.prev);                /* ...and the decoder moved on with it */
        CHECK_EQ(d.idx, rr.index);
    }

    CASE("SND: a frame longer than one buffer, decoded whole");
    {
        static uint8_t big[10 + 5000];
        static uint8_t data[5000];
        for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)rnd();
        const size_t fl = snd_frame(big, KIWI_SND_ADPCM, 0, 540, data, sizeof data);
        kiwi_snd_t f;
        kiwi_snd(big, fl, &f);
        kiwi_dsp_init(&d);
        CHECK_EQ(kiwi_snd_audio(&d, &f, big, fl, false, pcm, out, NULL, NULL), 10000);
        ref_t rr = { 0, 0 };
        static int16_t all[10000];
        ref_decode(&rr, data, sizeof data, all);
        CHECK_EQ(d.pred, rr.prev);
        CHECK_EQ(d.idx, rr.index);
    }

    CASE("SND: 16-bit PCM, either byte order; a stereo frame let go");
    {
        const uint8_t be[] = { 0x01, 0x02, 0xFF, 0xFE, 0x7F };       /* an odd byte left over */
        const size_t fl = snd_frame(frame, 0, 0, 540, be, sizeof be);
        kiwi_snd_t f;
        kiwi_snd(frame, fl, &f);
        kiwi_dsp_init(&d);
        CHECK_EQ(kiwi_snd_audio(&d, &f, frame, fl, false, pcm, out, NULL, NULL), 2);
        CHECK_EQ(pcm[0], 0x0102);
        CHECK_EQ(pcm[1], -2);
        const size_t fl2 = snd_frame(frame, KIWI_SND_LE, 0, 540, be, 4);
        kiwi_snd(frame, fl2, &f);
        CHECK_EQ(kiwi_snd_audio(&d, &f, frame, fl2, false, pcm, out, NULL, NULL), 2);
        CHECK_EQ(pcm[0], 0x0201);
        CHECK_EQ(pcm[1], -257);
        uint8_t st[40] = { 'S', 'N', 'D', KIWI_SND_STEREO | KIWI_SND_ADPCM };
        kiwi_snd(st, sizeof st, &f);
        sink_t s0 = { .buf = got, .cap = 16 };
        CHECK_EQ(kiwi_snd_audio(&d, &f, st, sizeof st, true, pcm, out, sink, &s0), 0);
        CHECK_EQ(s0.calls, 0);
    }

    CASE("SND: at every rate a Kiwi has, its length at 24 kHz");
    {
        /* Four frames of 2048 samples: 8192 * 24000 / rate out, give or take
         * one -- the lowest rate in pieces, each within KIWI_OUT_MAX. */
        static const double RATES[] = { 4000, 12000, 20250, 24000, 48000, 50000 };
        uint8_t data[FRAME_B];
        memset(data, 0x11, sizeof data);
        for (size_t k = 0; k < sizeof RATES / sizeof RATES[0]; k++) {
            kiwi_dsp_init(&d);
            CHECK(kiwi_dsp_reset(&d, RATES[k]));
            sink_t s0 = { .buf = got };
            for (int i = 0; i < 4; i++) {
                const size_t fl = snd_frame(frame, KIWI_SND_ADPCM, (uint32_t)i, 540, data, sizeof data);
                kiwi_snd_t f;
                kiwi_snd(frame, fl, &f);
                kiwi_snd_audio(&d, &f, frame, fl, true, pcm, out, sink, &s0);
            }
            /* Within a few samples: the linear resampler's position is a
             * float, and at 4 kHz it drifts by two in 49152 (40 ppm). */
            const double want_n = 8192.0 * KIWI_OUT_HZ / RATES[k];
            CHECK(fabs((double)s0.total - want_n) <= 4.0);
        }
    }
}

static void test_names(void)
{
    char out[16];
    CASE("labels");
    kiwi_label(out, sizeof out, "", "", "81.83.21.23", 8077, true);
    CHECK_STR(out, "..83.21.23:8077");
    kiwi_label(out, sizeof out, "", "RF.Guru EchoTracer", "81.83.21.23", 8077, true);
    CHECK_STR(out, "EchoTracer");
    kiwi_label(out, sizeof out, "Tower", "RF.Guru EchoTracer", "81.83.21.23", 8077, true);
    CHECK_STR(out, "Tower");
    kiwi_label(out, sizeof out, "", "", "kiwi.local", 8073, false);
    CHECK_STR(out, "kiwi.local:8073");                      /* it fits: whole, shared or not */
    kiwi_label(out, sizeof out, "", "", "kiwi.local", 8073, true);
    CHECK_STR(out, "kiwi.local:8073");
    kiwi_label(out, sizeof out, "", "RF.Guru ", "h", 1, false);
    CHECK_STR(out, "RF.Guru");                              /* all there is, its space left off */
    CASE("labels: an antenna of spaces says nothing; spaces round one left off");
    kiwi_label(out, sizeof out, "", "   ", "h", 1, false);
    CHECK_STR(out, "h:1");
    kiwi_label(out, sizeof out, "", "  Dipole  ", "h", 1, false);
    CHECK_STR(out, "Dipole");
    kiwi_label(out, sizeof out, "", "RF.Guru  OctaLoop ", "h", 1, false);
    CHECK_STR(out, "OctaLoop");
    CASE("labels: four receivers on one address, told apart by their ports");
    {
        char l[4][16];
        for (int i = 0; i < 4; i++)
            kiwi_label(l[i], sizeof l[i], "", "", "81.83.21.23", (uint16_t)(8074 + i), true);
        CHECK_STR(l[0], "..83.21.23:8074");
        CHECK_STR(l[3], "..83.21.23:8077");
        for (int i = 0; i < 4; i++)
            for (int k = i + 1; k < 4; k++) CHECK(strcmp(l[i], l[k]) != 0);
        kiwi_label(out, sizeof out, "", "", "192.0.2.123", 8074, true);
        CHECK_STR(out, "..0.2.123:8074");                  /* cut at a dot: two of them, not three */
    }
    CASE("labels: receivers on one domain, each alone on its host, told apart by its start");
    {
        char a[16], b[16];
        kiwi_label(a, sizeof a, "", "", "kiwi1.example.net", 8073, false);
        kiwi_label(b, sizeof b, "", "", "kiwi2.example.net", 8073, false);
        CHECK_STR(a, "kiwi1.example.n");
        CHECK(strcmp(a, b) != 0);
        kiwi_label(a, sizeof a, "", "", "abc.proxy.kiwisdr.com", 8073, false);
        kiwi_label(b, sizeof b, "", "", "xyz.proxy.kiwisdr.com", 8073, false);
        CHECK(strcmp(a, b) != 0);
        kiwi_label(a, sizeof a, "", "", "kiwi.on4cdj.be", 8073, false);
        CHECK_STR(a, "kiwi.on4cdj.be");
        kiwi_label(a, sizeof a, "", "", "81.83.21.23", 8077, false);
        CHECK_STR(a, "81.83.21.23");                        /* alone on its address: no port needed */
    }
    CASE("labels: the whole address where there is room");
    {
        char big[72];
        kiwi_label(big, sizeof big, "", "", "kiwi1.example.net", 8073, false);
        CHECK_STR(big, "kiwi1.example.net:8073");
        kiwi_label(big, sizeof big, "", "", "81.83.21.23", 8077, true);
        CHECK_STR(big, "81.83.21.23:8077");
        kiwi_label(big, sizeof big, "", "RF.Guru TerraBooster", "81.83.21.23", 8075, true);
        CHECK_STR(big, "TerraBooster");
    }
    CASE("labels: an https:// one on 443 by its name alone");
    {
        char big[72];
        CHECK_EQ(kiwi_label_port(443, true), 0);
        CHECK_EQ(kiwi_label_port(443, false), 443);                  /* 443 in the clear: said */
        CHECK_EQ(kiwi_label_port(8443, true), 8443);
        kiwi_label(out, sizeof out, "", "", "localhost", kiwi_label_port(443, true), false);
        CHECK_STR(out, "localhost");
        kiwi_label(big, sizeof big, "", "", "kiwisdr.on3rvh.be", kiwi_label_port(443, true), false);
        CHECK_STR(big, "kiwisdr.on3rvh.be");
        kiwi_label(out, sizeof out, "", "", "kiwisdr.on3rvh.be", 0, false);
        CHECK_STR(out, "kiwisdr.on3rvh.");                         /* too long: its start, as ever */
        kiwi_label(out, sizeof out, "", "", "n0bqv.proxy.kiwisdr.com", 0, true);
        CHECK_STR(out, "..y.kiwisdr.com");                          /* shared: its end, with no port */
    }
    CASE("labels: a name cut where a character ends");
    kiwi_label(out, sizeof out, "", "ABCDEFGHIJKLMN\xC3\xBC", "h", 1, false);     /* 14 + a 2-byte u-umlaut */
    CHECK_STR(out, "ABCDEFGHIJKLMN");
    kiwi_label(out, sizeof out, "", "ABCDEFGHIJKLM\xC3\xBC", "h", 1, false);      /* 13 + 2: fits */
    CHECK_STR(out, "ABCDEFGHIJKLM\xC3\xBC");

    CASE("the name a /status suggests");
    {
        kiwi_status_t st;
        char nm[16];
        kiwi_status_parse(NULL, &st);
        kiwi_status_name(nm, sizeof nm, &st);
        CHECK_STR(nm, "");                                  /* no Kiwi's: nothing */
        st.ok = true;
        strcpy(st.name, "RF.Guru Lombardsijde | EchoTracer");
        strcpy(st.antenna, "RF.Guru EchoTracer");
        kiwi_status_name(nm, sizeof nm, &st);
        CHECK_STR(nm, "EchoTracer");
        strcpy(st.antenna, "RF.Guru OctaLoop Mini ");
        kiwi_status_name(nm, sizeof nm, &st);
        CHECK_STR(nm, "OctaLoop Mini");                     /* the spaces at its end left out */
        strcpy(st.antenna, "   ");
        strcpy(st.name, "MOCK | the knob's mock Web-888");
        kiwi_status_name(nm, sizeof nm, &st);
        CHECK_STR(nm, "MOCK");                              /* no antenna: its name's first part */
        strcpy(st.name, "RF.Guru Lombardsijde | EchoTracer");
        kiwi_status_name(nm, sizeof nm, &st);
        CHECK_STR(nm, "Lombardsijde");                      /* ..."RF.Guru " left off there too */
        strcpy(st.name, "  RF.Guru  ");
        kiwi_status_name(nm, sizeof nm, &st);
        CHECK_STR(nm, "RF.Guru");                           /* all there is */
        strcpy(st.antenna, "");
        strcpy(st.name, "ON4CDJ KiwiSDR, Beveren");
        kiwi_status_name(nm, sizeof nm, &st);
        CHECK_STR(nm, "ON4CDJ KiwiSDR,");                   /* 15 at most */
        /* An "a" grave, then a no-break space across the 15th byte. */
        strcpy(st.antenna, "Mini-Whip \xC3\xA0 1\xC2\xA0m roof");
        kiwi_status_name(nm, sizeof nm, &st);
        CHECK_STR(nm, "Mini-Whip \xC3\xA0 1");
        strcpy(st.antenna, "Mini-Whip \xC3\xA0 10 m roof");
        kiwi_status_name(nm, sizeof nm, &st);
        CHECK_STR(nm, "Mini-Whip \xC3\xA0 10");
        strcpy(st.antenna, "ABCDEFGHIJKLMN\xE2\x8F\xB3");          /* 14 + a 3-byte hourglass */
        kiwi_status_name(nm, sizeof nm, &st);
        CHECK_STR(nm, "ABCDEFGHIJKLMN");
        strcpy(st.antenna, "");
        strcpy(st.name, "");
        kiwi_status_name(nm, sizeof nm, &st);
        CHECK_STR(nm, "");
    }

    CASE("who the owner sees: SET ident_user, URL-encoded");
    {
        char c[128];
        CHECK_EQ(kiwi_ident_cmd(c, sizeof c, ""), 23);
        CHECK_STR(c, "SET ident_user=VFO-Knob");
        kiwi_ident_cmd(c, sizeof c, NULL);
        CHECK_STR(c, "SET ident_user=VFO-Knob");
        kiwi_ident_cmd(c, sizeof c, "ON6URE");
        CHECK_STR(c, "SET ident_user=ON6URE");
        kiwi_ident_cmd(c, sizeof c, "ON6URE / knob");
        CHECK_STR(c, "SET ident_user=ON6URE%20%2F%20knob");
        kiwi_ident_cmd(c, sizeof c, "Jo\xC3\xABl (ON6URE)!");
        CHECK_STR(c, "SET ident_user=Jo%C3%ABl%20(ON6URE)!");
        kiwi_ident_cmd(c, sizeof c, "a=b&c%d+e~f_g.h*i'j");
        CHECK_STR(c, "SET ident_user=a%3Db%26c%25d%2Be~f_g.h*i'j");
        CHECK_EQ(kiwi_ident_cmd(c, 20, "ON6URE / knob"), -1);           /* no room: nothing half */
        CHECK_EQ(kiwi_ident_cmd(c, 16, ""), -1);
        /* The longest kept, every byte escaped: well within one frame. */
        char who[KIWI_IDENT_MAX];
        memset(who, 0xE9, sizeof who - 1);
        who[sizeof who - 1] = 0;
        CHECK_EQ(kiwi_ident_cmd(c, sizeof c, who), 15 + 3 * (KIWI_IDENT_MAX - 1));
    }

    CASE("sidebands, IARU Region 1");
    CHECK_STR(kiwi_sideband(7100000), "lsb");
    CHECK_STR(kiwi_sideband(1840000), "lsb");
    CHECK_STR(kiwi_sideband(5355000), "usb");
    CHECK_STR(kiwi_sideband(14200000), "usb");
    CHECK_STR(kiwi_sideband(50200000), "usb");
    CHECK(kiwi_sideband(6000000) == NULL);
    CHECK(kiwi_sideband(9500000) == NULL);
}

T_MAIN(
    test_asctime();
    test_hp();
    test_unescape();
    test_status();
    test_lifted();
    test_tune_cmd();
    test_cw_centre();
    test_settings();
    test_said();
    test_addresses();
    test_audio();
    test_resampler();
    test_decode();
    test_names();
)
