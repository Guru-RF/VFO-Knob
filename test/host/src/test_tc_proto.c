/* The tailchaser firmware's API parsing (components/tailchaser_client/
 * tc_proto.c) on the PC: a node's status as TailChaser writes it (src/
 * status.c) -- a transmit site with its receive site inside, a simplex node,
 * a transmit site whose receive site is not answering -- the block's state,
 * errors, the S-meter, HTTP answers whole and cut short, and the addresses
 * the page takes.
 *
 * With a file as its argument it parses that too and prints what it found:
 * a status saved from a real node, kept out of the repository. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tc_proto.h"

static int s_fail;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);               \
            s_fail++;                                                         \
        }                                                                     \
    } while (0)
#define CHECK_STR(a, b)                                                       \
    do {                                                                      \
        if (strcmp((a), (b))) {                                               \
            printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
            s_fail++;                                                         \
        }                                                                     \
    } while (0)

/* A transmit site of a split repeater, as a node in the field answers
 * (shortened: the counters left out), its receive site inside. */
static const char TX_SITE[] =
    "{\"version\":\"dev\",\"board\":\"Raspberry Pi Compute Module 4 Rev 1.1\",\"uptime_s\":525,"
    "\"now\":1795172655,\"block\":{\"rx\":false,\"tx\":true},\"role\":\"transmitter\","
    "\"title\":\"repeater transmitter\",\"has\":{\"rx\":false,\"tx\":true,\"reflector\":true,"
    "\"uplink\":false,\"rxlink\":true,\"repeater\":true},"
    "\"receiver\":{\"connected\":true,\"peer\":\"192.0.2.7:5210\",\"codec\":\"S16\",\"squelch\":true,"
    "\"siglev\":0.0},"
    "\"radio\":{\"freq_mhz\":438.8,\"freq_tx_mhz\":438.8,\"bandwidth\":\"narrow\",\"ctcss_rx\":\"none\","
    "\"ctcss_tx\":\"131.8\",\"squelch\":1,\"type\":\"sa818\"},"
    "\"link\":{\"connected\":true,\"audio\":true,\"host\":\"reflector.example.org\",\"port\":5300,"
    "\"callsign\":\"ON0TST\",\"tg\":206,\"home_tg\":8,\"talker\":\"ON4ABC\"},"
    "\"tx\":{\"keyed\":true,\"why\":\"talkgroup audio\",\"keyed_ms\":4200,\"timeout_s\":600},"
    "\"ident\":{\"callsign\":\"ON0TST\",\"interval_min\":15},"
    "\"gate\":{\"mode\":\"on\",\"cos\":false,\"open\":false,\"state\":\"idle\",\"level_db\":-120.0,"
    "\"rssi\":-1,\"rssi_lo\":79,\"rssi_hi\":100},"
    "\"history\":[],"
    "\"net_history\":[{\"t\":1795172600,\"ms\":12400,\"call\":\"ON4ABC\",\"tg\":206,\"outcome\":\"sent\"},"
    "{\"t\":1795172500,\"ms\":3000,\"call\":\"ON4OLD\",\"tg\":8,\"outcome\":\"sent\"}],"
    "\"remote_receiver\":{\"version\":\"dev\",\"uptime_s\":561,\"block\":{\"rx\":false,\"tx\":false},"
    "\"role\":\"receiver\",\"has\":{\"rx\":true,\"tx\":false,\"reflector\":false,\"uplink\":true},"
    "\"radio\":{\"freq_mhz\":431.2,\"freq_tx_mhz\":431.2,\"ctcss_rx\":\"131.8\",\"ctcss_tx\":\"none\"},"
    "\"gate\":{\"cos\":true,\"open\":true,\"state\":\"open\",\"level_db\":-18.5,\"rssi\":90,"
    "\"rssi_lo\":79,\"rssi_hi\":100},"
    "\"history\":[{\"t\":1795172640,\"ms\":2500,\"pass\":\"passed\",\"would\":\"\",\"like\":\"speech\","
    "\"analysed\":true,\"level_db\":-20.1,\"rssi\":88,\"tg\":206,\"sent\":true},"
    "{\"t\":1795172000,\"ms\":900,\"pass\":\"blocked\",\"like\":\"noise\"}],"
    "\"net_history\":[]}}\n";

/* A simplex node: its own receiver, nobody on the talkgroup. */
static const char SIMPLEX[] =
    "{\"version\":\"1.4.0\",\"uptime_s\":86400,\"now\":1795000000,\"block\":{\"rx\":true,\"tx\":false},"
    "\"role\":\"simplex\",\"has\":{\"rx\":true,\"tx\":true,\"reflector\":true,\"rxlink\":false,"
    "\"repeater\":false},"
    "\"radio\":{\"freq_mhz\":145.2875,\"freq_tx_mhz\":145.2875,\"ctcss_rx\":\"79.7\",\"ctcss_tx\":\"79.7\"},"
    "\"link\":{\"connected\":false,\"host\":\"reflector.example.org\",\"callsign\":\"ON0SPX\",\"tg\":0,"
    "\"home_tg\":8,\"talker\":\"\"},"
    "\"tx\":{\"keyed\":false,\"why\":\"\",\"keyed_ms\":0},"
    "\"ident\":{\"callsign\":\"\"},"
    "\"gate\":{\"cos\":true,\"open\":false,\"state\":\"listening\",\"level_db\":-41.0,\"rssi\":100,"
    "\"rssi_lo\":79,\"rssi_hi\":100},"
    "\"history\":[{\"t\":1795000000,\"ms\":700,\"pass\":\"blocked\",\"like\":\"noise\"}],"
    "\"net_history\":[]}";

/* A transmit site whose receive site does not answer: remote_receiver null,
 * the RemoteTrx link's squelch alone. */
static const char TX_ALONE[] =
    "{\"role\":\"transmitter\",\"has\":{\"rx\":false,\"tx\":true,\"rxlink\":true},"
    "\"block\":{\"rx\":false,\"tx\":false},"
    "\"receiver\":{\"connected\":false,\"squelch\":false},"
    "\"radio\":{\"freq_mhz\":438.8,\"freq_tx_mhz\":0,\"ctcss_tx\":\"none\"},"
    "\"ident\":{\"callsign\":\"ON0TST\"},\"link\":{\"connected\":true,\"tg\":8},"
    "\"remote_receiver\":null}";

static void test_tx_site(void)
{
    tc_status_t st;
    char why[40];
    CHECK(tc_parse_status(TX_SITE, sizeof TX_SITE - 1, &st, why, sizeof why));
    CHECK_STR(st.role, "transmitter");
    CHECK_STR(st.callsign, "ON0TST");
    CHECK_STR(st.version, "dev");
    CHECK(st.uptime_s == 525 && st.now == 1795172655);
    CHECK(!st.has_rx && st.has_tx && st.has_reflector && st.has_repeater && st.has_rxlink);
    CHECK(!st.block_rx && st.block_tx);
    /* what it sends on, and -- from the receive site -- what it hears on */
    CHECK(fabs(st.tx_mhz - 438.8) < 1e-9);
    CHECK(fabs(st.rx_mhz - 431.2) < 1e-9);
    CHECK_STR(st.ctcss_tx, "131.8");
    CHECK_STR(st.ctcss_rx, "131.8");
    CHECK(st.tx_keyed && st.tx_keyed_ms == 4200);
    CHECK_STR(st.tx_why, "talkgroup audio");
    /* the receiver is the receive site's gate, not the transmit site's */
    CHECK(st.rx_known && st.cos && st.open);
    CHECK_STR(st.gate, "open");
    CHECK(st.rssi == 90 && st.rssi_lo == 79 && st.rssi_hi == 100);
    CHECK(fabs(st.level_db + 18.5) < 1e-4);
    CHECK(st.rx_site_up);
    CHECK(st.link_up && st.tg == 206 && st.home_tg == 8);
    CHECK_STR(st.talker, "ON4ABC");
    CHECK_STR(st.reflector, "reflector.example.org");
    /* newest first: [0] is the last */
    CHECK_STR(st.net_call, "ON4ABC");
    CHECK(st.net_t == 1795172600 && st.net_ms == 12400);
    CHECK(st.local_t == 1795172640 && st.local_ms == 2500);
    CHECK_STR(st.local_pass, "passed");
}

static void test_simplex(void)
{
    tc_status_t st;
    CHECK(tc_parse_status(SIMPLEX, strlen(SIMPLEX), &st, NULL, 0));
    CHECK_STR(st.role, "simplex");
    /* no callsign of its own: the link's */
    CHECK_STR(st.callsign, "ON0SPX");
    CHECK(st.block_rx && !st.block_tx);
    CHECK(fabs(st.tx_mhz - 145.2875) < 1e-9 && fabs(st.rx_mhz - 145.2875) < 1e-9);
    CHECK_STR(st.ctcss_rx, "79.7");
    CHECK(st.rx_known && st.cos && !st.open);
    CHECK(st.rssi == 100);
    CHECK(!st.rx_site_up);              /* not a transmit site */
    CHECK(!st.link_up && st.tg == 0 && st.home_tg == 8);
    CHECK_STR(st.talker, "");
    CHECK_STR(st.net_call, "");
    CHECK_STR(st.local_pass, "blocked");
    CHECK(!st.tx_keyed);
}

static void test_tx_alone(void)
{
    tc_status_t st;
    CHECK(tc_parse_status(TX_ALONE, strlen(TX_ALONE), &st, NULL, 0));
    /* freq_tx_mhz 0: the radio's frequency */
    CHECK(fabs(st.tx_mhz - 438.8) < 1e-9);
    CHECK(st.rx_mhz == 0);
    CHECK(st.rx_known && !st.cos);
    CHECK(st.rssi == -1);
    CHECK(!st.rx_site_up);
    CHECK_STR(st.ctcss_tx, "none");
    CHECK(st.link_up && st.tg == 8);
}

static void test_not_status(void)
{
    tc_status_t st;
    char why[40];
    CHECK(!tc_parse_status("<html>", 6, &st, why, sizeof why));
    CHECK_STR(why, "not JSON");
    CHECK(!tc_parse_status("{\"ok\":false,\"error\":\"unauthorized\"}", 35, &st, why, sizeof why));
    CHECK_STR(why, "not a TailChaser status");
    /* cut short: not parsed */
    CHECK(!tc_parse_status(TX_SITE, 400, &st, why, sizeof why));
    /* odd strings: printable ASCII only, cut to fit */
    const char odd[] = "{\"role\":\"simplex\",\"ident\":{\"callsign\":\"ON\\u00e90A\\nBCDEFGHIJKLMNOPQRSTUV\"}}";
    CHECK(tc_parse_status(odd, sizeof odd - 1, &st, NULL, 0));
    CHECK_STR(st.callsign, "ON0ABCDEFGHIJKL");
    CHECK(strlen(st.callsign) == sizeof st.callsign - 1);
}

static void test_block_and_error(void)
{
    bool rx = true, tx = true;
    CHECK(tc_parse_block("{\"rx\":false,\"tx\":true}", 22, &rx, &tx));
    CHECK(!rx && tx);
    CHECK(!tc_parse_block("{\"rx\":1}", 8, &rx, &tx));
    CHECK(!tc_parse_block("nope", 4, &rx, &tx));
    char e[40];
    tc_parse_error("{\"ok\":false,\"error\":\"this key may only read\"}", 45, e, sizeof e);
    CHECK_STR(e, "this key may only read");
    tc_parse_error("garbage", 7, e, sizeof e);
    CHECK_STR(e, "");
}

static void test_smeter(void)
{
    char s[12];
    /* lo 79 = S1, hi 100 = S9+40: twelve steps over 21, TailChaser's own */
    CHECK(tc_smeter(-1, 79, 100, s, sizeof s) == -1);
    CHECK_STR(s, "");
    CHECK(tc_smeter(50, 100, 100, s, sizeof s) == -1);      /* not calibrated */
    CHECK(tc_smeter(79, 79, 100, s, sizeof s) == 0);
    CHECK_STR(s, "S1");
    CHECK(tc_smeter(60, 79, 100, s, sizeof s) == 0);        /* below S1 */
    CHECK_STR(s, "S1");
    CHECK(tc_smeter(100, 79, 100, s, sizeof s) == 88);
    CHECK_STR(s, "S9+40");
    CHECK(tc_smeter(104, 79, 100, s, sizeof s) == 108);     /* above: on to S9+60 */
    CHECK_STR(s, "S9+60");
    CHECK(tc_smeter(130, 79, 100, s, sizeof s) == 108);
    CHECK_STR(s, "S9+60");
    /* S9 is step 9: 79 + 8 * 21 / 12 = 93 */
    tc_smeter(93, 79, 100, s, sizeof s);
    CHECK_STR(s, "S9");
    tc_smeter(86, 79, 100, s, sizeof s);                    /* step 5 */
    CHECK_STR(s, "S5");
    tc_smeter(95, 79, 100, s, sizeof s);                    /* 10.14: S9+10 */
    CHECK_STR(s, "S9+10");
    tc_smeter(97, 79, 100, s, sizeof s);                    /* 11.29: S9+20 */
    CHECK_STR(s, "S9+20");
    /* the mock's transmit site: 103 of 30..130 is step 9.76, as its page */
    tc_smeter(103, 30, 130, s, sizeof s);
    CHECK_STR(s, "S9+10");
    /* each step up never reads lower */
    int last = -1;
    for (int r = 60; r <= 130; r++) {
        const int d = tc_smeter(r, 79, 100, NULL, 0);
        CHECK(d >= last);
        last = d;
    }
}

static void test_http(void)
{
    int code;
    const char *body;
    size_t bl;
    const char ok[] = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: 5\r\n"
                      "Connection: close\r\n\r\n{\"a\"}";
    CHECK(tc_http_answer(ok, sizeof ok - 1, &code, &body, &bl));
    CHECK(code == 200 && bl == 5 && !memcmp(body, "{\"a\"}", 5));
    /* cut short of its length */
    CHECK(!tc_http_answer(ok, sizeof ok - 3, &code, &body, &bl));
    /* headers not finished */
    CHECK(!tc_http_answer(ok, 30, &code, &body, &bl));
    /* more than it said: the length is what counts */
    const char more[] = "HTTP/1.1 401 Unauthorized\r\ncontent-length: 2\r\n\r\n{}trailing";
    CHECK(tc_http_answer(more, sizeof more - 1, &code, &body, &bl));
    CHECK(code == 401 && bl == 2);
    /* no length: all of it */
    const char nolen[] = "HTTP/1.0 403 Forbidden\r\n\r\n{\"ok\":false}";
    CHECK(tc_http_answer(nolen, sizeof nolen - 1, &code, &body, &bl));
    CHECK(code == 403 && bl == 12);
    CHECK(!tc_http_answer("SSH-2.0-OpenSSH_9.6\r\n\r\n", 23, &code, &body, &bl));
}

static void test_addr(void)
{
    tc_addr_t a;
    CHECK(tc_parse_addr("on0ora-tx", &a));
    CHECK_STR(a.host, "on0ora-tx");
    CHECK(a.port == 8080);
    CHECK(tc_parse_addr("  http://192.0.2.10:8081/admin", &a));
    CHECK_STR(a.host, "192.0.2.10");
    CHECK(a.port == 8081);
    CHECK(tc_parse_addr("node.tail0000.ts.net:80", &a));
    CHECK_STR(a.host, "node.tail0000.ts.net");
    CHECK(a.port == 80);
    CHECK(tc_parse_addr("rep_1 ", &a));
    CHECK_STR(a.host, "rep_1");
    CHECK(!tc_parse_addr("", &a));
    CHECK(!tc_parse_addr("   ", &a));
    CHECK(!tc_parse_addr("host:0", &a));
    CHECK(!tc_parse_addr("host:70000", &a));
    CHECK(!tc_parse_addr("host:80x", &a));
    CHECK(!tc_parse_addr("ho\"st", &a));
    CHECK(!tc_parse_addr("<script>", &a));
    CHECK(!tc_parse_addr("https://host", &a));                  /* no TLS: "https" is no host */
    char longhost[100];
    memset(longhost, 'a', sizeof longhost - 1);
    longhost[sizeof longhost - 1] = 0;
    CHECK(!tc_parse_addr(longhost, &a));
}

static void test_smeter_dbm(void)
{
    CHECK(tc_smeter_dbm(-1, 79, 100) == -127.0f);
    CHECK(tc_smeter_dbm(50, 100, 100) == -127.0f);
    CHECK(fabsf(tc_smeter_dbm(79, 79, 100) + 121.0f) < 0.01f);            /* S1 */
    CHECK(fabsf(tc_smeter_dbm(100, 79, 100) + 33.0f) < 0.01f);            /* S9+40 */
    CHECK(fabsf(tc_smeter_dbm(93, 79, 100) + 73.0f) < 0.01f);             /* S9 */
    CHECK(fabsf(tc_smeter_dbm(0, 79, 100) + 127.0f) < 0.01f);             /* far below: S0, no lower */
    CHECK(fabsf(tc_smeter_dbm(78, 79, 100) - (-121.0f - 6.0f * 12.0f / 21.0f)) < 0.01f);   /* under S1 */
    CHECK(fabsf(tc_smeter_dbm(130, 79, 100) + 13.0f) < 0.01f);            /* S9+60, no further */
    CHECK(fabsf(tc_smeter_dbm(80, 30, 130) - (-121.0f + 6.0f * 6.0f)) < 0.01f);   /* step 7: S7 */
    CHECK(fabsf(tc_smeter_dbm(103, 30, 130) - (-73.0f + 7.6f)) < 0.01f);  /* step 9.76 */
    /* A transmit site's own gate (today's TailChaser): its signal, 0..100,
     * over the receive site's raw reading. */
    static const char T[] =
        "{\"role\":\"transmitter\",\"has\":{\"rx\":false,\"tx\":true,\"rxlink\":true},\"monitor\":true,"
        "\"gate\":{\"rssi\":64,\"rssi_lo\":0,\"rssi_hi\":100},"
        "\"remote_receiver\":{\"gate\":{\"cos\":true,\"open\":true,\"rssi\":103,\"rssi_lo\":30,\"rssi_hi\":130}}}";
    tc_status_t st;
    char why[48];
    CHECK(tc_parse_status(T, sizeof T - 1, &st, why, sizeof why));
    CHECK(st.rssi == 64 && st.rssi_lo == 0 && st.rssi_hi == 100 && st.cos && st.open && st.monitor);
    static const char U[] =
        "{\"role\":\"transmitter\",\"has\":{\"rx\":false,\"tx\":true,\"rxlink\":true},"
        "\"gate\":{\"rssi\":-1,\"rssi_lo\":0,\"rssi_hi\":100},"
        "\"remote_receiver\":{\"gate\":{\"cos\":true,\"rssi\":103,\"rssi_lo\":30,\"rssi_hi\":130}}}";
    CHECK(tc_parse_status(U, sizeof U - 1, &st, why, sizeof why));
    CHECK(st.rssi == 103 && st.rssi_lo == 30 && !st.monitor);        /* none of its own: the receive site's */
}

static void test_ws(void)
{
    tc_ws_frame_t f;
    /* binary, 3 bytes */
    const uint8_t a[] = { 0x82, 0x03, 1, 2, 3, 0x89 };
    CHECK(tc_ws_frame(a, 1, &f) == 0);
    CHECK(tc_ws_frame(a, 4, &f) == 0);
    CHECK(tc_ws_frame(a, sizeof a, &f) == 5 && f.op == 2 && f.fin && f.off == 2 && f.len == 3);
    /* 3200 bytes: a 16-bit length */
    static uint8_t b[4 + 3200];
    b[0] = 0x82; b[1] = 126; b[2] = 3200 >> 8; b[3] = 3200 & 255;
    CHECK(tc_ws_frame(b, 4 + 3199, &f) == 0);
    CHECK(tc_ws_frame(b, sizeof b, &f) == 3204 && f.off == 4 && f.len == 3200);
    /* a 64-bit length */
    static uint8_t c[10 + 70000];
    c[0] = 0x02; c[1] = 127;
    memset(c + 2, 0, 8);
    c[7] = 0; c[8] = 70000 >> 8 & 255; c[9] = 70000 & 255;
    c[7] = 1;                                   /* 65536 + 4464 */
    c[8] = (70000 - 65536) >> 8; c[9] = (70000 - 65536) & 255;
    CHECK(tc_ws_frame(c, sizeof c, &f) == 10 + 70000 && !f.fin && f.op == 2 && f.len == 70000);
    /* what a server may not send */
    const uint8_t masked[] = { 0x82, 0x81, 0, 0, 0, 0, 7 };
    CHECK(tc_ws_frame(masked, sizeof masked, &f) == -1);
    const uint8_t rsv[] = { 0xC2, 0x00 };
    CHECK(tc_ws_frame(rsv, sizeof rsv, &f) == -1);
    const uint8_t op3[] = { 0x83, 0x00 };
    CHECK(tc_ws_frame(op3, sizeof op3, &f) == -1);
    const uint8_t bigping[] = { 0x89, 126, 0, 200 };
    CHECK(tc_ws_frame(bigping, sizeof bigping, &f) == -1);
    const uint8_t piecemeal_close[] = { 0x08, 0x00 };
    CHECK(tc_ws_frame(piecemeal_close, sizeof piecemeal_close, &f) == -1);
    const uint8_t huge[] = { 0x82, 127, 0, 0, 0, 0, 2, 0, 0, 0 };
    CHECK(tc_ws_frame(huge, sizeof huge, &f) == -1);
    /* a pong, masked */
    const uint8_t mask[4] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t out[16];
    CHECK(tc_ws_client_frame(10, (const uint8_t *)"hi", 2, mask, out, sizeof out) == 8);
    CHECK(out[0] == 0x8A && out[1] == 0x82 && out[6] == ('h' ^ 0x11) && out[7] == ('i' ^ 0x22));
    CHECK(tc_ws_client_frame(10, (const uint8_t *)"hi", 2, mask, out, 7) == 0);
    /* the handshake's answer */
    int code;
    static const char ok[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n\x82\x00";
    CHECK(tc_ws_answer(ok, 20, &code) == 0);
    CHECK(tc_ws_answer(ok, sizeof ok - 1, &code) == sizeof ok - 3 && code == 101);
    static const char off[] = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    CHECK(tc_ws_answer(off, sizeof off - 1, &code) == sizeof off - 1 && code == 404);
    static const char junk[] = "SSH-2.0-OpenSSH\r\n\r\n";
    CHECK(tc_ws_answer(junk, sizeof junk - 1, &code) == sizeof junk - 1 && code == 0);
    /* a level */
    int16_t pcm[4] = { 0, 0, 0, 0 };
    CHECK(tc_pcm_peak_db(pcm, 4) == -120.0f);
    pcm[2] = -32768;
    CHECK(fabsf(tc_pcm_peak_db(pcm, 4)) < 0.01f);
    pcm[2] = 8192;
    CHECK(fabsf(tc_pcm_peak_db(pcm, 4) + 12.04f) < 0.01f);
}

static void test_names(void)
{
    char n[TC_NAME];
    tc_addr_t a;
    /* A settings line: the address, a tab, the name. */
    CHECK(tc_parse_addr("on0ora-simplex:8080\tON0ORA-S", &a));
    CHECK_STR(a.host, "on0ora-simplex");
    CHECK(a.port == 8080);
    tc_line_name("on0ora-simplex:8080\tON0ORA-S", n, sizeof n);
    CHECK_STR(n, "ON0ORA-S");
    tc_line_name("on0ora-tx", n, sizeof n);                      /* a line from before names */
    CHECK_STR(n, "");
    tc_clean_name("  ON0ORA   simplex \t", n, sizeof n);
    CHECK_STR(n, "ON0ORA simplex");
    tc_clean_name("A\x01" "B\xc3\xa9" "C", n, sizeof n);              /* control and non-ASCII dropped */
    CHECK_STR(n, "ABC");
    tc_clean_name("0123456789abcdefXYZ", n, sizeof n);           /* 16 at most */
    CHECK_STR(n, "0123456789abcdef");
    tc_clean_name("0123456789abcde fgh", n, sizeof n);           /* no space left hanging at the cut */
    CHECK_STR(n, "0123456789abcde");
}

static void test_mhz(void)
{
    char s[16];
    tc_mhz(438.8, s, sizeof s);
    CHECK_STR(s, "438.800");
    tc_mhz(145.2875, s, sizeof s);
    CHECK_STR(s, "145.2875");
    tc_mhz(431.0249999, s, sizeof s);
    CHECK_STR(s, "431.025");
    tc_mhz(1296.5, s, sizeof s);
    CHECK_STR(s, "1296.500");
    tc_mhz(0, s, sizeof s);
    CHECK_STR(s, "");
}

/* A status saved from a real node: what it parses to. */
static int show_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    static char buf[65536];
    const size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    tc_status_t st;
    char why[40], sm[12];
    if (!tc_parse_status(buf, n, &st, why, sizeof why)) {
        printf("%s: %s\n", path, why);
        return 1;
    }
    tc_smeter(st.rssi, st.rssi_lo, st.rssi_hi, sm, sizeof sm);
    printf("%s: %s %s, version %s, up %u s\n", path, st.role, st.callsign, st.version, (unsigned)st.uptime_s);
    printf("  tx %.4f MHz T%s, rx %.4f MHz T%s; blocks rx %d tx %d\n", st.tx_mhz, st.ctcss_tx, st.rx_mhz,
           st.ctcss_rx, st.block_rx, st.block_tx);
    printf("  keyed %d (%s) %u ms; rx known %d cos %d open %d gate %s rssi %d [%d..%d] %s level %.1f\n",
           st.tx_keyed, st.tx_why, (unsigned)st.tx_keyed_ms, st.rx_known, st.cos, st.open, st.gate, st.rssi,
           st.rssi_lo, st.rssi_hi, sm, st.level_db);
    printf("  rx site up %d; link %d tg %u home %u talker '%s' on %s; last net %s, last local %s\n",
           st.rx_site_up, st.link_up, (unsigned)st.tg, (unsigned)st.home_tg, st.talker, st.reflector,
           st.net_call, st.local_pass);
    return 0;
}

int main(int argc, char **argv)
{
    test_tx_site();
    test_simplex();
    test_tx_alone();
    test_not_status();
    test_block_and_error();
    test_smeter();
    test_http();
    test_addr();
    test_names();
    test_smeter_dbm();
    test_ws();
    test_mhz();
    int rc = 0;
    for (int i = 1; i < argc; i++) rc |= show_file(argv[i]);
    if (s_fail) {
        printf("%d failed\n", s_fail);
        return 1;
    }
    printf("tc_proto: all passed\n");
    return rc;
}
