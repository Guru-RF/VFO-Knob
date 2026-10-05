/* The FlexRadio API's parsing (components/flex_client/flex_parse.c): the
 * discovery broadcast, the memory statuses, the slice's antenna lists. The
 * lines are what a FLEX-6600 on SmartSDR 4.2.20 sent the knob's probes (the
 * slice), and what FlexLib and AetherSDR read (memories, discovery). */
#include "tiny.h"
#include "flex_parse.h"

/* A discovery packet as a radio sends it: VITA-49 extension data with a
 * stream id and a class id -- OUI 0x001C2D, packet class 0xFFFF -- both
 * timestamps, then the text, NUL-padded to whole words. */
static size_t vita(uint8_t *b, const char *text, uint32_t oui, uint16_t pcc, int trailer)
{
    size_t n = strlen(text), pad = (4 - n % 4) % 4;
    const size_t words = 7 + (n + pad) / 4 + (trailer ? 1 : 0);
    const uint32_t w0 = 0x38000000u | (trailer ? 0x04000000u : 0) | 0x00D00000u | (uint32_t)words;
    const uint32_t w[5] = { w0, 0x00000800u, oui, 0x534C0000u | pcc, 0 };
    for (int i = 0; i < 5; i++) {
        b[i * 4] = (uint8_t)(w[i] >> 24); b[i * 4 + 1] = (uint8_t)(w[i] >> 16);
        b[i * 4 + 2] = (uint8_t)(w[i] >> 8); b[i * 4 + 3] = (uint8_t)w[i];
    }
    memset(b + 20, 0, 8);
    memcpy(b + 28, text, n);
    memset(b + 28 + n, 0, pad);
    size_t len = 28 + n + pad;
    if (trailer) { memset(b + len, 0xAB, 4); len += 4; }
    return len;
}

static void test_discovery(void)
{
    CASE("discovery, VITA-49");
    uint8_t b[1500];
    flex_disc_t r;
    const char *t = "discovery_protocol_version=3.0.0.2 model=FLEX-6600 serial=1919-1212-6600-0001 "
                    "version=4.2.20.41343 nickname=Lombards\x7fijde callsign=ON6URE ip=172.16.32.179 "
                    "port=4992 status=In_Use inuse_ip=172.16.31.9 inuse_host=thinkstation "
                    "max_licensed_version=v4 gui_client_stations=thinkstation,Maestro "
                    "gui_client_hosts=thinkstation,maestro gui_client_handles=0x31CE0037,0x1234";
    size_t n = vita(b, t, 0x001C2D, 0xFFFF, 0);
    CHECK(flex_disc_parse(b, n, &r));
    CHECK_STR(r.model, "FLEX-6600");
    CHECK_STR(r.serial, "1919-1212-6600-0001");
    CHECK_STR(r.nickname, "Lombards ijde");
    CHECK_STR(r.callsign, "ON6URE");
    CHECK_STR(r.ip, "172.16.32.179");
    CHECK_EQ(r.port, 4992);
    CHECK_STR(r.version, "4.2.20.41343");
    CHECK_STR(r.status, "In_Use");
    CHECK_STR(r.who, "thinkstation,Maestro");      /* the stations, before the host */

    CASE("discovery, with a trailer");
    n = vita(b, "model=FLEX-8600 serial=8600-1 ip=10.0.0.5 port=4993 inuse_host=shack", 0x001C2D, 0xFFFF, 1);
    CHECK(flex_disc_parse(b, n, &r));
    CHECK_STR(r.model, "FLEX-8600");
    CHECK_EQ(r.port, 4993);
    CHECK_STR(r.who, "shack");
    CHECK_STR(r.ip, "10.0.0.5");

    CASE("discovery, no address: the sender's, then");
    n = vita(b, "model=FLEX-6400 serial=6400-9", 0x001C2D, 0xFFFF, 0);
    CHECK(flex_disc_parse(b, n, &r));
    CHECK_STR(r.ip, "");
    CHECK_EQ(r.port, 4992);

    CASE("discovery, a bad address is none");
    n = vita(b, "serial=1 ip=172.16.300.1", 0x001C2D, 0xFFFF, 0);
    CHECK(flex_disc_parse(b, n, &r));
    CHECK_STR(r.ip, "");
    n = vita(b, "serial=1 ip=1.2.3", 0x001C2D, 0xFFFF, 0);
    CHECK(flex_disc_parse(b, n, &r) && !r.ip[0]);
    n = vita(b, "serial=1 ip=1.2.3.4.5", 0x001C2D, 0xFFFF, 0);
    CHECK(flex_disc_parse(b, n, &r) && !r.ip[0]);

    CASE("discovery, not a radio's");
    n = vita(b, "model=FLEX-6600 serial=1 ip=1.2.3.4", 0x00ABCD, 0xFFFF, 0);
    CHECK(!flex_disc_parse(b, n, &r));             /* another OUI */
    n = vita(b, "model=FLEX-6600 serial=1 ip=1.2.3.4", 0x001C2D, 0x8005, 0);
    CHECK(!flex_disc_parse(b, n, &r));             /* the radio's Opus, not its discovery */
    n = vita(b, "model=FLEX-6600 ip=1.2.3.4", 0x001C2D, 0xFFFF, 0);
    CHECK(!flex_disc_parse(b, n, &r));             /* no serial */
    CHECK(!flex_disc_parse(b, 10, &r));            /* cut short */

    CASE("discovery, bare text (a gateway's)");
    const char *bare = "name=GeekJeep model=FLEX-6700 serial=1234-5678 version=4.2.18 "
                       "ip=192.0.2.10 port=4992 status=Available nickname=Geek\x7fJeep\0\0\r";
    CHECK(flex_disc_parse((const uint8_t *)bare, strlen(bare) + 3, &r));
    CHECK_STR(r.nickname, "Geek Jeep");
    CHECK_STR(r.ip, "192.0.2.10");
    CHECK_STR(r.status, "Available");

    CASE("discovery, long values cut, never overrun");
    char big[600];
    snprintf(big, sizeof big, "serial=%s model=%s nickname=%s ip=1.2.3.4",
             "SSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS", "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM",
             "NNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNNN");
    n = vita(b, big, 0x001C2D, 0xFFFF, 0);
    CHECK(flex_disc_parse(b, n, &r));
    CHECK_EQ(strlen(r.serial), sizeof r.serial - 1);
    CHECK_EQ(strlen(r.model), sizeof r.model - 1);
    CHECK_EQ(strlen(r.nickname), sizeof r.nickname - 1);
}

static void test_memories(void)
{
    CASE("memory, all of it");
    flex_mem_t m;
    memset(&m, 0, sizeof m);
    bool gone = true;
    const char *s = "memory 3 owner=ON6URE group=Repeaters freq=29.620000 name=ON0TEN\x7f" "10m "
                    "mode=FM step=100 repeater=DOWN repeater_offset=0.100000 tone_mode=CTCSS_TX "
                    "tone_value=79.7 power=100 rx_filter_low=-8000 rx_filter_high=8000 "
                    "highlight=0 highlight_color=0x00000000 squelch=1 squelch_level=20";
    CHECK_EQ(flex_mem_index(s), 3);
    flex_mem_parse(s, &m, &gone);
    CHECK(!gone);
    CHECK_EQ(m.idx, 3);
    CHECK_EQ(m.hz, 29620000);
    CHECK_STR(m.name, "ON0TEN 10m");
    CHECK_STR(m.mode, "fm");
    CHECK_EQ(m.duplex, -1);
    CHECK_EQ(m.offset_hz, 100000);
    CHECK(m.tone_on);
    CHECK_EQ(m.tone_dhz, 797);
    CHECK_EQ(m.lo, -8000);
    CHECK_EQ(m.hi, 8000);

    CASE("memory, what changed only");
    flex_mem_parse("memory 3 name=Renamed tone_mode=OFF", &m, &gone);
    CHECK(!gone);
    CHECK_STR(m.name, "Renamed");
    CHECK(!m.tone_on);
    CHECK_EQ(m.hz, 29620000);                      /* untouched */
    CHECK_EQ(m.duplex, -1);
    flex_mem_parse("memory 3 repeater=UP repeater_offset=1.000000", &m, &gone);
    CHECK_EQ(m.duplex, 1);
    CHECK_EQ(m.offset_hz, 1000000);
    flex_mem_parse("memory 3 repeater=SIMPLEX", &m, &gone);
    CHECK_EQ(m.duplex, 0);

    CASE("memory, removed");
    flex_mem_parse("memory 3 removed", &m, &gone);
    CHECK(gone);
    flex_mem_parse("memory 3 in_use=0", &m, &gone);
    CHECK(gone);
    flex_mem_parse("memory 3 name=removed", &m, &gone);
    CHECK(!gone);                                  /* a name, not the word */

    CASE("memory, not one");
    CHECK_EQ(flex_mem_index("memoryx 3 freq=1"), -1);
    CHECK_EQ(flex_mem_index("memory x"), -1);
    CHECK_EQ(flex_mem_index("memory 70000 freq=1"), -1);
    CHECK_EQ(flex_mem_index("memory 12"), 12);
    CHECK_EQ(flex_mem_index("slice 0 freq=1"), -1);

    CASE("memory, name cut at 16");
    memset(&m, 0, sizeof m);
    flex_mem_parse("memory 1 name=ABCDEFGHIJKLMNOPQRSTUVWXYZ", &m, &gone);
    CHECK_STR(m.name, "ABCDEFGHIJKLMNOP");
}

static void test_memory_list(void)
{
    CASE("memories in order of frequency");
    flex_mem_t list[4], m;
    int n = 0;
    const struct { uint16_t idx; int64_t hz; } in[] = {
        { 0, 14074000 }, { 1, 7090000 }, { 2, 29620000 }, { 4, 3700000 },
    };
    for (int i = 0; i < 4; i++) {
        memset(&m, 0, sizeof m);
        m.idx = in[i].idx;
        m.hz = in[i].hz;
        CHECK(flex_mem_put(list, &n, 4, &m) >= 0);
    }
    CHECK_EQ(n, 4);
    CHECK_EQ(list[0].idx, 4);
    CHECK_EQ(list[1].idx, 1);
    CHECK_EQ(list[2].idx, 0);
    CHECK_EQ(list[3].idx, 2);

    CASE("memories, full");
    memset(&m, 0, sizeof m);
    m.idx = 9;
    m.hz = 21074000;
    CHECK_EQ(flex_mem_put(list, &n, 4, &m), -1);
    CHECK_EQ(n, 4);

    CASE("memories, one moved");
    m = list[1];                                   /* 1: 7.090 to 28.000 */
    m.hz = 28000000;
    CHECK_EQ(flex_mem_put(list, &n, 4, &m), 2);
    CHECK_EQ(n, 4);
    CHECK_EQ(list[1].idx, 0);
    CHECK_EQ(list[2].idx, 1);
    CHECK_EQ(flex_mem_put(list, &n, 4, &list[0]), 0);   /* itself: no harm */
    CHECK_EQ(list[0].idx, 4);

    CASE("memories, found and dropped");
    CHECK_EQ(flex_mem_find(list, n, 2), 3);
    CHECK_EQ(flex_mem_find(list, n, 7), -1);
    CHECK(flex_mem_drop(list, &n, 0));
    CHECK(!flex_mem_drop(list, &n, 0));
    CHECK_EQ(n, 3);
    CHECK_EQ(list[0].idx, 4);
    CHECK_EQ(list[1].idx, 1);
    CHECK_EQ(list[2].idx, 2);

    CASE("memories, nearest and on");
    CHECK_EQ(flex_mem_nearest(list, n, 14000000), 0);     /* 3.7 is 10.3 away, 28 is 14 */
    CHECK_EQ(flex_mem_nearest(list, n, 28500000), 1);
    CHECK_EQ(flex_mem_on(list, n, 28000040, 50), 1);
    CHECK_EQ(flex_mem_on(list, n, 28000060, 50), -1);
    CHECK_EQ(flex_mem_nearest(list, 0, 1), -1);

    CASE("memories, same frequency: by number");
    n = 0;
    for (int i = 3; i >= 1; i--) {
        memset(&m, 0, sizeof m);
        m.idx = (uint16_t)i;
        m.hz = 14200000;
        flex_mem_put(list, &n, 4, &m);
    }
    CHECK_EQ(list[0].idx, 1);
    CHECK_EQ(list[1].idx, 2);
    CHECK_EQ(list[2].idx, 3);
}

static void test_slice(void)
{
    CASE("a slice's antennas");
    const char *s = "slice 0 in_use=1 RF_frequency=14.094000 client_handle=0x31CE0037 rxant=ANT1 "
                    "mode=USB filter_lo=100 filter_hi=2800 txant=ANT2 tx=1 active=1 "
                    "ant_list=ANT1,ANT2,RX_A,RX_B,XVTA,XVTB tx_ant_list=ANT1,ANT2,XVTA,XVTB rfgain=8";
    char v[64];
    CHECK(flex_kv(s, "rxant", v, sizeof v));
    CHECK_STR(v, "ANT1");
    CHECK(flex_kv(s, "txant", v, sizeof v));
    CHECK_STR(v, "ANT2");
    CHECK(flex_kv(s, "ant_list", v, sizeof v));
    CHECK_STR(v, "ANT1,ANT2,RX_A,RX_B,XVTA,XVTB");     /* not tx_ant_list's */
    CHECK(flex_kv(s, "tx_ant_list", v, sizeof v));
    CHECK_STR(v, "ANT1,ANT2,XVTA,XVTB");
    CHECK(!flex_kv(s, "ant", v, sizeof v));
    CHECK(flex_kv(s, "tx", v, sizeof v) && !strcmp(v, "1"));
    int64_t hz;
    CHECK(flex_kv_mhz(s, "RF_frequency", &hz));
    CHECK_EQ(hz, 14094000);
    CHECK(flex_kv_mhz("x=0.6", "x", &hz) && hz == 600000);
    CHECK(flex_kv_mhz("x=-0.6", "x", &hz) && hz == -600000);
    CHECK(flex_kv_mhz("x=54.0000009", "x", &hz) && hz == 54000000);

    CASE("lists of names");
    CHECK_EQ(flex_list_count("ANT1,ANT2,RX_A,RX_B,XVTA,XVTB"), 6);
    CHECK_EQ(flex_list_count(""), 0);
    CHECK_EQ(flex_list_count("ANT1"), 1);
    CHECK_EQ(flex_list_count("ANT1,,ANT2,"), 2);
    CHECK_EQ(flex_list_find("ANT1,ANT2,RX_A", "RX_A"), 2);
    CHECK_EQ(flex_list_find("ANT1,ANT2,RX_A", "RX"), -1);
    CHECK_EQ(flex_list_find("ANT1,,ANT2", "ANT2"), 1);
    CHECK_EQ(flex_list_find("ANT1,ANT2", ""), -1);
    CHECK_EQ(flex_list_find("", "ANT1"), -1);
}

/* Anything at all off the network: no read or write out of bounds (the
 * sanitizers watch), no value unterminated. */
static void test_fuzz(void)
{
    CASE("fuzz");
    uint32_t x = 2463534242u;
    uint8_t b[1500];
    char s[700];
    static const char *const WORDS[] = {
        "serial=", "model=", "ip=", "port=", "nickname=", "memory ", "freq=", "name=",
        "repeater=", "tone_value=", "removed", "in_use=0", "=", " ", "\x7f", ",", "1.2.3.4",
    };
    for (int round = 0; round < 20000; round++) {
        size_t n = 0;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        const size_t want = x % sizeof b;
        if (round & 1) {
            for (; n < want; n++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[n] = (uint8_t)x; }
        } else {
            while (n + 16 < want) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                const char *w = WORDS[x % (sizeof WORDS / sizeof WORDS[0])];
                const size_t l = strlen(w);
                memcpy(b + n, w, l);
                n += l;
            }
        }
        if (round % 3 == 0 && n >= 28) vita(b, "", 0x001C2D, 0xFFFF, 0);   /* a real header on it */
        flex_disc_t r;
        if (flex_disc_parse(b, n, &r))
            CHECK(memchr(r.serial, 0, sizeof r.serial) && memchr(r.who, 0, sizeof r.who) &&
                  memchr(r.ip, 0, sizeof r.ip) && r.serial[0]);
        const size_t sl = n < sizeof s - 1 ? n : sizeof s - 1;
        memcpy(s, b, sl);
        s[sl] = 0;
        flex_mem_t m;
        memset(&m, 0, sizeof m);
        bool gone;
        flex_mem_parse(s, &m, &gone);
        CHECK(memchr(m.name, 0, sizeof m.name) && memchr(m.mode, 0, sizeof m.mode));
        (void)flex_list_count(s);
        (void)flex_list_find(s, "ANT1");
    }
}

T_MAIN({
    test_discovery();
    test_memories();
    test_memory_list();
    test_slice();
    test_fuzz();
})
