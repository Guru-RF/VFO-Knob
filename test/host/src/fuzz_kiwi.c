/* Deterministic byte fuzzer for KiwiSDR's protocol (components/kiwi_proto).
 *
 * Everything a receiver says reaches these functions: a MSG's text, an SND
 * frame's bytes, its /status page, an answer's head and a redirect's
 * Location -- from any address on the internet the operator types in, and
 * from a receiver that may be something else entirely -- and so does the
 * address typed on the page. So they have to survive arbitrary bytes. Run
 * under ASan/UBSan (on by default) this catches overreads and overflows;
 * the fixed PRNG keeps a failure reproducible. A million rounds: random
 * bytes, mutated real messages, and every truncation of each. */
#include "tiny.h"
#include "kiwi_proto.h"

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* What receivers really say: MSGs (their bodies), and /status lines. */
static const char *MSGS[] = {
    "rx_chans=13", "chan_no_pwd=0", "is_local=0 max_camp=0", "badp=0", "badp=1", "badp=3",
    "center_freq=31000000 bandwidth=62000000 adc_clk_nom=122880000",
    "audio_init=0 audio_rate=12000 sample_rate=12001.135", "version_maj=2026 version_min=901",
    "freq_offset=100000.000", "ip_limit=60%2c81.83.21.23", "inactivity_timeout=30",
    "kiwi_kick=0,Kicked%20by%20the%20admin", "kiwi_kick=Gone%20for%20now", "too_busy=8",
    "too_busy=0", "redirect=http%3a%2f%2fother", "reason_disabled= down=1", "exclusive_use",
    "load_cfg=%7b%22ext_api_nchans%22%3a%204%2c%22rx_chans%22%3a%2013%7d",
    "client_public_ip=81.83.21.23", "cfg_loaded", "sample_rate=nan", "audio_rate=-1",
    /* Where each centres CW -- neither one captured: UberSDR's Kiwi input on
     * the carrier, as its source writes it (kiwi_websocket.go, Go's escapes);
     * a KiwiSDR's owner at 400..800, as tools/mock_kiwi.py sends it. */
    "load_cfg=%7B%22passbands%22%3A%7B%22usb%22%3A%7B%22lo%22%3A300%2C%22hi%22%3A2400%7D%2C%22cw%22%3A%7B"
    "%22lo%22%3A-400%2C%22hi%22%3A400%7D%2C%22cwn%22%3A%7B%22lo%22%3A-250%2C%22hi%22%3A250%7D%7D%7D",
    "load_cfg=%7b%22passbands%22%3a%20%7b%22cw%22%3a%20%7b%22lo%22%3a%20400%2c%20%22hi%22%3a%20800%2c%20"
    "%22pbw%22%3a%20400%7d%7d%7d",
    /* An answer's head, and the addresses a front's redirect gives -- or the
     * page: the kiwisdr.com proxy's 307, Cloudflare's 301. */
    "HTTP/1.1 307 Temporary Redirect\r\nLocation: https://n0bqv.proxy.kiwisdr.com/\r\nServer: x\r\n\r\n",
    "HTTP/1.1 301 Moved Permanently\nlocation:https://kiwisdr.on3rvh.be:8443/12345/SND\n\n",
    "https://kiwisdr.on3rvh.be/", "http://n0bqv.proxy.kiwisdr.com:80/?f=7074usb", "81.83.21.23:8077", "/status",
};
static const char *MODES_ANY[] = { "usb", "lsb", "cw", "cwr", "cwn", "am", "sam", "nbfm", "digl", "iq", "", "x" };
static const char STATUS[] =
    "status=active\noffline=no\nname=RF.Guru Lombardsijde | EchoTracer\n"
    "sdr_hw=Web-888 v2026.0901 \xE2\x81\xA3 \xF0\x9F\x93\xA1 GPS \xE2\x81\xA3 \xE2\x8F\xB3 LIMITS\n"
    "bands=0-62000000\nfreq_offset=0.000\nusers=10\nusers_max=13\nloc=Lombardsijde, Belgium\n"
    "sw_version=Web888_v2026.0901\nantenna=RF.Guru EchoTracer\next_api=4\nuptime=86400\n"
    "date=Fri Oct  2 13:12:00 2026\n";
static const char *KEYS[] = {
    "rx_chans", "badp", "ip_limit", "kiwi_kick", "too_busy", "down", "bandwidth", "sample_rate",
    "x", "=", " ", "",
};

static char        text[2048];
static uint8_t     bytes[2048], frame[2048 + 16];
static int16_t     pcm[KIWI_PCM_MAX], out[KIWI_OUT_MAX];
static kiwi_said_t said;
static kiwi_dsp_t  dsp;
static unsigned long emitted;

static void sink(void *ctx, const int16_t *p, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n; i++) emitted += (unsigned long)(p[i] & 1);   /* every sample read */
}

/* One input, every way in: as a MSG, a key's value, a /status page, a date,
 * a kick message, an SND frame. `n` bytes in text[] and bytes[]. */
static void feed(size_t n)
{
    char v[96], cut[2048];
    memcpy(cut, text, n);
    cut[n] = 0;                                  /* as kiwi_sess.c hands them: cut, ended */
    if (rnd() % 64 == 0) kiwi_said_init(&said);
    kiwi_said(&said, cut, rnd() & 1);
    kiwi_quiet(&said, (uint8_t)(rnd() % 3), (int)(rnd() % 3) - 1, rnd() & 1);
    kiwi_msg_val(cut, KEYS[rnd() % (sizeof KEYS / sizeof KEYS[0])], v, 1 + rnd() % sizeof v);
    kiwi_msg_val(cut, KEYS[rnd() % (sizeof KEYS / sizeof KEYS[0])], NULL, 0);

    kiwi_status_t st;
    memcpy(cut, text, n);
    cut[n] = 0;
    kiwi_status_parse(cut, &st);
    kiwi_mark_t m = { .hp = 1, .strikes = 1, .kind = (uint8_t)(rnd() % 3), .flags = (uint8_t)(rnd() & 7),
                      .rx_boot = rnd(), .mark_utc = rnd() };
    /* Clearing proves lifting: never the one without the other. */
    if (kiwi_mark_cleared(&m, &st) && !kiwi_mark_lifted(&m, &st)) abort();
    uint32_t t;
    kiwi_asctime(cut, &t);
    /* Its name for the dial, from what any receiver says of itself: within
     * its room (test_kiwi_proto checks the cut keeps characters whole --
     * random bytes are no UTF-8 to keep whole). */
    {
        char nm[16], lb[16];
        const size_t cap = 1 + rnd() % sizeof nm, lcap = 1 + rnd() % sizeof lb;
        kiwi_status_name(nm, cap, &st);
        if (strlen(nm) >= cap) abort();
        const uint32_t r = rnd();
        kiwi_label(lb, lcap, r & 1 ? st.name : "", r & 2 ? st.antenna : "", cut, (uint16_t)rnd(), r & 4);
        if (strlen(lb) >= lcap) abort();
    }
    /* ...and as who the owner sees: whole, or not at all. */
    {
        char who[KIWI_IDENT_MAX], c[128];
        memcpy(who, text, n < sizeof who - 1 ? n : sizeof who - 1);
        who[n < sizeof who - 1 ? n : sizeof who - 1] = 0;
        const int k = kiwi_ident_cmd(c, 1 + rnd() % sizeof c, who);
        if (k >= 0 && (strlen(c) != (size_t)k || strchr(c + 15, ' '))) abort();
    }
    /* As an answer's head and its Location, and an address typed or pasted:
     * whatever comes out fits where it goes, a host never empty, a port a
     * port, and a redirect followed only to TLS on the host it came from. */
    {
        memcpy(cut, text, n);
        cut[n] = 0;
        char loc[128], host[64], to[64];
        const size_t lcap = 1 + rnd() % sizeof loc;
        if (kiwi_http_header(cut, "Location", loc, lcap) && strlen(loc) >= lcap) abort();
        uint16_t port = 0;
        bool tls = false;
        const size_t hcap = 1 + rnd() % sizeof host;
        if (kiwi_url(cut, rnd() & 1 ? 8073 : 0, host, hcap, &port, &tls) && (!host[0] || strlen(host) >= hcap || !port))
            abort();
        const bool clear = rnd() & 1;
        if (kiwi_redirect(cut, "kiwisdr.on3rvh.be", !clear, &port, to, sizeof to) && (!clear || to[0]))
            abort();
        char hh[96];
        kiwi_host_hdr(hh, 1 + rnd() % sizeof hh, host[0] ? host : "x", port, tls);
    }
    memcpy(cut, text, n);
    cut[n] = 0;
    kiwi_unescape(cut);

    /* As a frame's text from its start, for where the receiver centres CW
     * -- "MSG " in front, or "MSG load_cfg=", or nothing -- in pieces of any
     * size, as the frames and reads break it: a centre within -1000..1500 Hz,
     * whatever it said, and the tune with it whole or not at all. */
    {
        static const char *const HEAD[] = { "MSG ", "MSG load_cfg=", "" };
        const char *h = HEAD[rnd() % 3];
        const size_t hl = strlen(h), fl = hl + n;
        memcpy(frame, h, hl);
        memcpy(frame + hl, text, n);
        kiwi_cw_t cw;
        kiwi_cw_init(&cw);
        for (size_t i = 0; i < fl;) {
            size_t k = 1 + rnd() % 64;
            if (k > fl - i) k = fl - i;
            kiwi_cw_feed(&cw, frame + i, k);
            i += k;
        }
        const int32_t ctr = kiwi_cw_centre(&cw, KIWI_CW_PITCH);
        if (ctr < -1000 || ctr > 1500) abort();
        kiwi_tune_t t = { .hz = (int64_t)(rnd() % 62000001), .lo = (int32_t)(rnd() % 60001) - 30000,
                          .hi = (int32_t)(rnd() % 60001) - 30000 };
        snprintf(t.mode, sizeof t.mode, "%s", MODES_ANY[rnd() % (sizeof MODES_ANY / sizeof MODES_ANY[0])]);
        char cmd[128];
        const size_t cap = 1 + rnd() % sizeof cmd;
        const int k = kiwi_tune_cmd(cmd, cap, &t, rnd() & 1 ? (double)(rnd() % 100000) : 0.0,
                                    rnd() & 1 ? 30000000 : 0, rnd() & 1 ? 4000.0 + rnd() % 46001 : 0.0, ctr);
        if (k >= 0 && ((size_t)k >= cap || strlen(cmd) != (size_t)k)) abort();
    }

    /* As an SND frame, with "SND" in front half the time so it gets in. */
    if (n >= 3 && rnd() & 1) memcpy(bytes, "SND", 3);
    kiwi_snd_t f;
    if (kiwi_snd(bytes, n, &f)) {
        if (rnd() % 16 == 0) kiwi_dsp_reset(&dsp, 4000 + rnd() % 46001);
        if (rnd() % 16 == 0) kiwi_dsp_trim(&dsp, 0.997f + (float)(rnd() % 6000) / 1e6f);
        /* What the session reckons the frame is worth: never more than its bytes hold. */
        if (kiwi_snd_len(&f, n) > 2 * n) abort();
        kiwi_snd_audio(&dsp, &f, bytes, n, rnd() & 1, pcm, out, sink, NULL);
        kiwi_dbm(f.smeter, rnd() & 1);
    }
}

int main(void)
{
    kiwi_said_init(&said);
    kiwi_dsp_init(&dsp);
    const size_t nmsgs = sizeof MSGS / sizeof MSGS[0];
    long rounds = 0;

    /* 1. Pure random bytes, NULs and all. */
    for (int i = 0; i < 400000; i++, rounds++) {
        const size_t n = rnd() % 600;
        for (size_t j = 0; j < n; j++) bytes[j] = (uint8_t)(text[j] = (char)rnd());
        feed(n);
    }

    /* 2. Real messages, mutated: far more likely to reach the deep paths. */
    for (int i = 0; i < 560000; i++, rounds++) {
        const char *s = rnd() % 8 ? MSGS[rnd() % nmsgs] : STATUS;
        size_t n = strlen(s);
        memcpy(text, s, n);
        const int muts = (int)(rnd() % 5);
        for (int k = 0; k < muts && n; k++) {
            const size_t p = rnd() % n;
            switch (rnd() % 5) {
            case 0: text[p] = (char)rnd(); break;
            case 1: text[p] = (char)"=% ,\n\r0"[rnd() % 7]; break;
            case 2: n = p; break;                                    /* cut short */
            case 3:                                                  /* two glued */
                if (n + 64 < sizeof text) {
                    const char *t2 = MSGS[rnd() % nmsgs];
                    const size_t l2 = strlen(t2) < 64 ? strlen(t2) : 64;
                    memcpy(text + n, t2, l2);
                    n += l2;
                }
                break;
            case 4: text[p] = '%'; break;                            /* a broken escape */
            }
        }
        memcpy(bytes, text, n);
        feed(n);
    }

    /* 3. Every message cut at every length -- the classic overread. */
    while (rounds < 1000000) {
        for (size_t s = 0; s < nmsgs && rounds < 1000000; s++) {
            const size_t full = strlen(MSGS[s]);
            for (size_t n = 0; n <= full && rounds < 1000000; n++, rounds++) {
                memcpy(text, MSGS[s], n);
                memcpy(bytes, MSGS[s], n);
                feed(n);
            }
        }
        const size_t full = strlen(STATUS);
        for (size_t n = 0; n <= full && rounds < 1000000; n++, rounds++) {
            memcpy(text, STATUS, n);
            memcpy(bytes, STATUS, n);
            feed(n);
        }
    }

    CASE("fuzz");
    CHECK(rounds >= 1000000);   /* reaching here without a sanitizer abort is the result */
    printf("fuzz_kiwi: %ld rounds, no crash (%lu odd samples heard)\n", rounds, emitted);
    return t_fail ? 1 : 0;
}
