/* The k4 firmware's Elecraft K4 client on the PC, against tools/mock_k4.py --
 * the knob's own code (k4_client, k4_parse, vfo_tune, ptt_fsm), ESP-IDF stood
 * in for by stubs.c. One scenario a run, as radio_start() runs once in a
 * firmware's life; run.sh starts a mock for each, and gives its log, where the
 * mock says what the knob sent and when the radio would have transmitted:
 *
 *   k4host basic    <port> <log>   log in, tune, mode, passband, AGC, gain,
 *                                  RIT, the DSP, the S-meter, receive audio;
 *                                  an over (the TX meters, the microphone's
 *                                  audio, the unkey) and a tune carrier
 *   k4host refuse   <port> <log>   (--refuse-tx) a key the radio will not
 *                                  have: refused with its reason, nothing sent
 *   k4host change   <port> <log>   (--change-after 4) the radio retuned from
 *                                  its front panel: the dial follows
 *   k4host password <port> <log>   a wrong password: the radio hangs up on it,
 *                                  and the face says why
 *   k4host silent   <port> <log>   (--mute-pong) a radio that stops
 *                                  answering: the link is called dead
 *
 * Each check prints PASS or FAIL; the exit status counts the failures. */
#include <stdarg.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ptt_fsm.h"
#include "radio.h"

bool     fh_logged(const char *needle);
bool     fh_mic_active(void);
uint64_t fh_frames(void);
static int s_fails;

static void check(bool ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char what[300];
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    if (!ok) s_fails++;
}

static char s_mocklog[256];
static char *mock_text(void)
{
    static char buf[1 << 20];
    FILE *f = fopen(s_mocklog, "r");
    if (!f) { buf[0] = 0; return buf; }
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}
static bool mock_has(const char *needle) { return strstr(mock_text(), needle) != NULL; }
static void ms(int n) { usleep((useconds_t)n * 1000); }

static radio_status_t s;
static radio_status_t *st(void) { radio_get_status(&s); return &s; }

static bool wait_mock(const char *needle, int ms_max)
{
    for (int t = 0; t < ms_max; t += 25) { if (mock_has(needle)) return true; ms(25); }
    return false;
}
static bool wait_ptt(int state, int ms_max)
{
    for (int t = 0; t < ms_max; t += 25) { if (st()->ptt_state == state) return true; ms(25); }
    return false;
}
static bool wait_log(const char *needle, int ms_max)
{
    for (int t = 0; t < ms_max; t += 25) { if (fh_logged(needle)) return true; ms(25); }
    return false;
}

static void ready_up(void)
{
    int t = 0;
    while (st()->link != RADIO_LINK_READY && t < 8000) { ms(50); t += 50; }
    check(s.link == RADIO_LINK_READY, "link READY (%d ms)", t);
    ms(700);
}

static void scenario_basic(void)
{
    ready_up();
    st();
    check(s.f_display == 14074000, "the radio's frequency is the dial's: %lld", (long long)s.f_display);
    check(!strcmp(s.mode, "usb"), "mode usb: '%s'", s.mode);
    check(!strcmp(s.model, "K4"), "the radio says K4: '%s'", s.model);
    check(!strcmp(s.agc, "fast"), "AGC fast: '%s'", s.agc);
    check(s.filt_hi - s.filt_lo == 2400 && s.filt_lo == 300, "passband %d..%d (2400 wide round 1500)", (int)s.filt_lo, (int)s.filt_hi);
    check(s.have_gain && s.gain == 0 && s.gain_min == -60 && s.gain_max == 0, "RF gain 0 of -60..0: %d", s.gain);
    check((s.dsp_has & (RADIO_DSP_NB | RADIO_DSP_NR | RADIO_DSP_ANF | RADIO_DSP_APF)) ==
          (RADIO_DSP_NB | RADIO_DSP_NR | RADIO_DSP_ANF | RADIO_DSP_APF), "all four DSP switches heard: 0x%x", s.dsp_has);
    check(s.smeter_dbm > -121.0f && s.smeter_dbm < -20.0f, "the S-meter is polled and read: %.0f dBm", s.smeter_dbm);
    check(fh_frames() > 400, "receive audio plays: %llu frames at 24 kHz", (unsigned long long)fh_frames());
    check(fh_logged("ready:"), "ready logged");

    /* The dial. */
    radio_tune_by(5, 1, 100);
    check(st()->f_display == 14074500, "five detents of 100 Hz up: %lld", (long long)s.f_display);
    check(wait_mock("tuned to 14074500", 1500), "...and FA sent to the radio");
    radio_set_mode("cw");
    check(wait_mock("mode MD3", 1500), "mode cw: MD3 sent");
    ms(200);
    check(!strcmp(st()->mode, "cw"), "...and shown: '%s'", s.mode);
    radio_set_filter(-250, 250);
    check(wait_mock("bandwidth 500 Hz", 1500), "filter 500 wide: BW0050 sent");
    radio_set_agc("slow");
    check(wait_mock("AGC GT1", 1500), "AGC slow: GT1 sent");
    radio_set_gain(-15);
    check(wait_mock("RF gain -15 dB", 1500), "RF gain -15 dB sent");
    radio_set_rit(100);
    check(wait_mock("RIT offset 100", 1500) && wait_mock("RIT on", 1500), "RIT +100: offset set and RIT on");
    ms(300);
    check(st()->rit_hz == 100, "...and shown: %d", (int)s.rit_hz);
    radio_set_rit(0);
    check(wait_mock("RIT off", 1500), "RIT 0: RIT off");
    radio_set_dsp(RADIO_DSP_NB, true);
    check(wait_mock("NB on", 1500), "noise blanker on");
    ms(300);
    check(st()->dsp_on & RADIO_DSP_NB, "...and lit");
    radio_set_dsp(RADIO_DSP_NR, true);
    check(wait_mock("NR on level 4", 1500), "noise reduction on, at the level the radio has it (4)");
    radio_set_dsp(RADIO_DSP_APF, true);
    check(wait_mock("APF on width 1", 1500), "audio peak filter on, at its width (1)");

    /* An over. */
    radio_set_mode("usb");
    ms(300);
    radio_ptt_key();
    check(wait_ptt(PTT_ON, 3000), "PTT confirmed by the radio: state %d", s.ptt_state);
    check(mock_has("would TRANSMIT (voice)"), "the mock saw TX;");
    check(fh_mic_active(), "the microphone is on");
    check(st()->tx, "the radio is on the air");
    ms(700);
    st();
    check(s.tx_fwd_w >= 49.0f && s.tx_fwd_w <= 51.0f, "forward power from TM, in watts at QRO: %.1f", s.tx_fwd_w);
    check(s.tx_swr > 1.1f && s.tx_swr < 1.3f, "SWR from TM: %.1f", s.tx_swr);
    check(wait_mock("TX audio: mode 3, 240 samples, rate code 0", 1500), "the microphone's audio goes to the radio, 12 kHz, 240 samples");
    check(s.txa_sent > 10, "...frames sent: %u", (unsigned)s.txa_sent);
    radio_ptt_unkey();
    check(wait_ptt(PTT_IDLE, 3000), "unkeyed: state %d", s.ptt_state);
    check(wait_mock("back on receive", 1500), "the radio is back on receive");
    check(!fh_mic_active(), "the microphone is off");

    /* A tune carrier. */
    radio_tune();
    check(wait_ptt(PTT_ON, 3000), "a tune carrier keyed");
    check(mock_has("TRANSMIT (tune carrier)"), "the mock saw TU1;");
    radio_ptt_unkey();
    check(wait_ptt(PTT_IDLE, 3000), "...and unkeyed");
    check(wait_mock("tune carrier off", 1500), "TU0 sent");
    ms(300);
    check(!st()->tx, "the radio is back on receive");
}

static void scenario_refuse(void)
{
    ready_up();
    radio_ptt_key();
    check(wait_mock("TX refused", 2000), "the radio was asked, and refused");
    check(wait_ptt(PTT_IDLE, 3000), "the key is no key: state %d", s.ptt_state);
    check(!strcmp(st()->tx_why, "TX INHIBITED"), "the face says why: '%s'", s.tx_why);
    check(!mock_has("would TRANSMIT"), "nothing was transmitted");
    check(!st()->tx && !fh_mic_active(), "no transmit, microphone off");
    radio_tune();
    check(wait_mock("TUNE refused", 2000), "a tune carrier is refused as well");
    check(wait_ptt(PTT_IDLE, 3000), "...and ends: state %d", s.ptt_state);
}

/* A key the radio never answers: the deadline calls it refused, the unkey is
 * sent, and the radio -- already on receive -- has nothing to report. That is
 * the end of it, not a climb up the teardown ladder to a reboot (esp_restart
 * exits 3 here). */
static void scenario_ignored(void)
{
    ready_up();
    radio_ptt_key();
    check(wait_mock("TX ignored", 2000), "the radio was asked, and said nothing");
    check(wait_ptt(PTT_IDLE, 3500), "the key ends on its own: state %d", s.ptt_state);
    ms(5000);                                  /* past every rung of the ladder */
    check(st()->ptt_state == PTT_IDLE, "...and stays ended: state %d", s.ptt_state);
    check(st()->link == RADIO_LINK_READY, "the link is still up: %d", (int)s.link);
    check(!fh_mic_active(), "the microphone is off");
}

/* The radio takes the key 2.5 s late, after the deadline has called it
 * refused, and ignores an RX; sent meanwhile. Whatever the knob concluded
 * meanwhile -- and a stale "receive" must not conclude it -- a radio that
 * transmits must not be left transmitting. */
static void scenario_late(void)
{
    ready_up();
    radio_ptt_key();
    check(wait_mock("take effect late", 2000), "the radio was asked, and will do it late");
    check(wait_mock("would TRANSMIT", 6000), "the radio did take the key, late");
    check(wait_mock("back on receive", 8000), "...and was put back on receive by the knob");
    /* What follows -- the ladder's last rung, a reboot, since this radio
     * could not be asked again in time -- is the ladder's own, and safe. */
}

static void scenario_change(void)
{
    ready_up();
    check(st()->f_display == 14074000, "starts on 14.074: %lld", (long long)s.f_display);
    check(wait_mock("front panel retunes", 6000), "the radio is retuned");
    ms(1200);
    check(st()->f_display == 7100000, "the dial follows: %lld", (long long)s.f_display);
    check(!strcmp(s.mode, "lsb"), "...and the mode: '%s'", s.mode);
    check(s.reconciles >= 1, "adopted as a reconcile: %u", (unsigned)s.reconciles);
    check(s.filt_lo < 0 && s.filt_hi <= 0, "lower sideband's passband is below the carrier: %d..%d", (int)s.filt_lo, (int)s.filt_hi);
    /* A turn of the dial now goes from the new frequency, not the old. */
    radio_tune_by(1, 1, 1000);
    check(st()->f_display == 7101000, "one 1 kHz detent from there: %lld", (long long)s.f_display);
}

static void scenario_password(void)
{
    check(wait_mock("WRONG PASSWORD", 8000), "the radio refused the proof");
    ms(1500);                  /* the knob retries, and the reason stays on its face */
    check(st()->link != RADIO_LINK_READY, "never ready: link %d", s.link);
    check(!strcmp(s.link_why, "PASSWORD?"), "the face says why: '%s'", s.link_why);
    check(s.closes >= 1, "the session was ended: %u", (unsigned)s.closes);
}

static void scenario_silent(void)
{
    ready_up();
    check(wait_log("the radio stopped answering", 9000), "no PONG for 5 s: the link is called dead");
    check(st()->closes >= 1, "closed: %u", (unsigned)s.closes);
    radio_ptt_key();
    ms(500);
    check(st()->ptt_state == PTT_IDLE && !mock_has("would TRANSMIT"), "a radio that does not answer is not keyed");
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    /* Sending to a radio that has hung up must fail with an error, as it does
     * in lwIP, not kill the process: the password scenario depends on it. */
    signal(SIGPIPE, SIG_IGN);
    const char *sc = argv[1];
    const int port = atoi(argv[2]);
    snprintf(s_mocklog, sizeof s_mocklog, "%s", argc > 3 ? argv[3] : "logs/mock.log");
    radio_start("127.0.0.1", (uint16_t)port, "", !strcmp(sc, "password") ? "wrong" : "secret");
    if      (!strcmp(sc, "basic"))    scenario_basic();
    else if (!strcmp(sc, "refuse"))   scenario_refuse();
    else if (!strcmp(sc, "ignored"))  scenario_ignored();
    else if (!strcmp(sc, "late"))     scenario_late();
    else if (!strcmp(sc, "change"))   scenario_change();
    else if (!strcmp(sc, "password")) scenario_password();
    else if (!strcmp(sc, "silent"))   scenario_silent();
    else { fprintf(stderr, "no scenario %s\n", sc); return 2; }
    printf("%d failure(s)\n", s_fails);
    return s_fails ? 1 : 0;
}
