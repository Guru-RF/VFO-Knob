/* Parser tests. Formats verified against AetherSDR's TciProtocol.cpp /
 * TciServer.cpp rather than against the TCI spec, because AetherSDR is what
 * we actually talk to. */
#include "tiny.h"
#include "tci_parse.h"

static tci_fact_t P(const char *s)
{
    tci_fact_t f;
    tci_parse(s, strlen(s), &f);
    return f;
}

static void test_vfo(void)
{
    CASE("vfo");
    tci_fact_t f = P("vfo:0,0,14074000;");
    CHECK_EQ(f.kind, TCI_VFO);
    CHECK_EQ(f.trx, 0);
    CHECK_EQ(f.channel, 0);
    CHECK_EQ(f.hz, 14074000);

    /* Channel 1 is the TX projection; the caller must ignore it, but we still
     * have to parse it so it is not counted as unknown. */
    f = P("vfo:1,1,7100000;");
    CHECK_EQ(f.trx, 1);
    CHECK_EQ(f.channel, 1);
    CHECK_EQ(f.hz, 7100000);

    /* 6 m and up exceed 32 bits when expressed in Hz. */
    f = P("vfo:0,0,5000000000;");
    CHECK_EQ(f.hz, 5000000000LL);

    /* Terminator and whitespace are both optional. */
    CHECK_EQ(P("vfo:0,0,14074000").hz, 14074000);
    CHECK_EQ(P("  vfo:0,0,14074000;  \r\n").hz, 14074000);
}

static void test_ptt(void)
{
    CASE("trx / ptt");
    tci_fact_t f = P("trx:0,true;");
    CHECK_EQ(f.kind, TCI_TRX);
    CHECK_EQ(f.trx, 0);
    CHECK(f.b0);

    f = P("trx:0,false;");
    CHECK_EQ(f.kind, TCI_TRX);
    CHECK(!f.b0);

    /* AetherSDR parses the state strictly and drops anything else; a garbled
     * PTT frame must never be read as "true". */
    CHECK_EQ(P("trx:0,yes;").kind,  TCI_UNKNOWN);
    CHECK_EQ(P("trx:0,1;").kind,    TCI_UNKNOWN);
    CHECK_EQ(P("trx:0;").kind,      TCI_UNKNOWN);
    CHECK_EQ(P("trx:0,;").kind,     TCI_UNKNOWN);

    /* Case tolerance on the wire value is harmless and cheap. */
    CHECK(P("trx:0,TRUE;").b0);
}

static void test_greeting(void)
{
    CASE("greeting");
    CHECK_EQ(P("ready;").kind, TCI_READY);
    tci_fact_t f = P("protocol:ExpertSDR3,1.5;");
    CHECK_EQ(f.kind, TCI_PROTOCOL);
    CHECK_STR(f.s0, "ExpertSDR3,1.5");   /* must NOT split on the comma */

    f = P("modulations_list:usb,lsb,cw,cwr,am,sam,fm,nfm,digu,digl,rtty;");
    CHECK_EQ(f.kind, TCI_MODULATIONS_LIST);
    CHECK(strncmp(f.s0, "usb,lsb,cw", 10) == 0);

    CHECK_EQ(P("trx_count:2;").i0, 2);
    /* Both of these appeared as UNPARSED against a live AetherSDR greeting. */
    CHECK_EQ(P("channels_count:2;").kind, TCI_CHANNELS_COUNT);
    CHECK_EQ(P("channels_count:2;").i0, 2);
    CHECK_EQ(P("receive_only:false;").kind, TCI_RECEIVE_ONLY);
    CHECK(!P("receive_only:false;").b0);
    CHECK(P("receive_only:true;").b0);

    /* Captured from a live AetherSDR greeting. active_slice carries a display
     * LETTER as well as an index, which the first implementation dropped. */
    tci_fact_t a = P("active_slice:0,A;");
    CHECK_EQ(a.kind, TCI_ACTIVE_SLICE);
    CHECK_EQ(a.i0, 0);
    CHECK_STR(a.s0, "A");

    /* Audio format is advertised up front; v1 stores it so v2 need not touch
     * the parser. */
    CHECK_EQ(P("audio_samplerate:48000;").i0, 48000);
    CHECK_STR(P("audio_stream_sample_type:float32;").s0, "float32");
    CHECK_EQ(P("audio_stream_channels:2;").i0, 2);
    CHECK_EQ(P("audio_stream_samples:2048;").i0, 2048);
    CHECK_EQ(P("tx_stream_audio_buffering:50;").i0, 50);

    /* Everything else in the real greeting must be recognised, not unknown. */
    static const char *GREETING_REST[] = {
        "rx_enable:0,true;", "sql_enable:0,false;", "sql_level:0,20;",
        "agc_mode:0,med;", "rx_nb_enable:0,false;", "rx_nr_enable:0,false;",
        "rx_anf_enable:0,false;", "rx_apf_enable:0,false;", "mute:0,false;",
        "tune_drive:0,10;", "mic_level:99;", "volume:0;",
        "iq_samplerate:48000;", "start;",
    };
    for (size_t i = 0; i < sizeof GREETING_REST / sizeof GREETING_REST[0]; i++) {
        tci_fact_t g = P(GREETING_REST[i]);
        t_run++;
        if (g.kind == TCI_UNKNOWN) {
            t_fail++;
            fprintf(stderr, "FAIL [greeting] unparsed: %s\n", GREETING_REST[i]);
        }
    }
    /* Hardcoded server-side, so recognised but ignorable. */
    CHECK(tci_is_ignorable(P("vfo_limits:1000,75000000;").kind));
    CHECK(tci_is_ignorable(P("if_limits:-48000,48000;").kind));
}

static void test_state_and_telemetry(void)
{
    CASE("state");
    tci_fact_t f = P("modulation:0,USB;");
    CHECK_EQ(f.kind, TCI_MODULATION);
    CHECK_STR(f.s0, "usb");              /* normalised to lower case */

    f = P("rx_filter_band:0,-2700,-300;");
    CHECK_EQ(f.i0, -2700);
    CHECK_EQ(f.i1, -300);

    CHECK(P("lock:0,true;").b0);
    CHECK_EQ(P("rit_offset:0,-120;").i0, -120);

    /* AetherSDR passes the FlexRadio names through: off, slow, med, fast. */
    f = P("agc_mode:1,MED;");
    CHECK_EQ(f.kind, TCI_AGC_MODE);
    CHECK_EQ(f.trx, 1);
    CHECK_STR(f.s0, "med");

    CASE("telemetry");
    f = P("rx_smeter:0,-93;");
    CHECK_EQ(f.kind, TCI_RX_SMETER);
    CHECK_EQ(f.i0, -93);

    f = P("rx_channel_sensors:0,0,-93.4;");
    CHECK_EQ(f.kind, TCI_RX_CHANNEL_SENSORS);
    CHECK_EQ(f.channel, 0);
    CHECK(f.f0 < -93.3f && f.f0 > -93.5f);

    /* tx_sensors:0,<mic>,<fwd>,<peak>,<swr>,<alc>; -- trx is hardcoded 0 and
     * peak carries the same cached value as fwd (TciServer.cpp:3699). */
    f = P("tx_sensors:0,-12.5,48.2,48.2,1.4,-3.0;");
    CHECK_EQ(f.kind, TCI_TX_SENSORS);
    CHECK(f.f1 > 48.1f && f.f1 < 48.3f);
    CHECK(f.f3 > 1.39f && f.f3 < 1.41f);
}

static void test_robustness(void)
{
    CASE("robustness");
    tci_fact_t f;
    CHECK(!tci_parse("", 0, &f));
    CHECK(!tci_parse(";", 1, &f));
    CHECK(!tci_parse("   ", 3, &f));
    CHECK(!tci_parse(NULL, 10, &f));

    CHECK_EQ(P("wibble:1,2,3;").kind, TCI_UNKNOWN);
    CHECK_EQ(P("vfo").kind, TCI_VFO);            /* no args -> defaults */
    CHECK_EQ(P("vfo").trx, -1);
    CHECK_EQ(P("vfo:;").hz, 0);
    CHECK_EQ(P("vfo:a,b,c;").hz, 0);             /* junk -> defaults, no crash */

    /* Must not read past len even without a NUL. */
    const char raw[] = { 'v','f','o',':','0',',','0',',','7','0','0' };
    CHECK(tci_parse(raw, sizeof raw, &f));
    CHECK_EQ(f.hz, 700);

    /* Over-long input is truncated, not overflowed. */
    char big[4096];
    memset(big, 'x', sizeof big);
    CHECK(tci_parse(big, sizeof big, &f));
    CHECK_EQ(f.kind, TCI_UNKNOWN);

    /* dds: arrives with every accepted tune and must not count as unknown. */
    CHECK(tci_is_ignorable(P("dds:0,14074000;").kind));
}

T_MAIN({
    test_vfo();
    test_ptt();
    test_greeting();
    test_state_and_telemetry();
    test_robustness();
})
