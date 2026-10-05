/* The multiflex firmware's FlexRadio client on the PC, against
 * tools/mock_flex.py -- the knob's own code (flex_client, flex_parse,
 * vfo_tune, ptt_fsm), ESP-IDF stood in for by stubs.c. One scenario a run,
 * as radio_start() runs once in a firmware's life; run.sh starts a mock for
 * each, and gives its log, where the mock says what the knob sent and where
 * the slice would transmit after it:
 *
 *   flexhost basic     <port> <log>   the antennas, both ways; memory mode,
 *                                     stepping, a repeater's shift and tone
 *                                     -- the transmit offset set after each
 *                                     memory apply, as the radio leaves it
 *                                     stale -- and back to the VFO, simplex
 *   flexhost refuse    <port> <log>   (--refuse-apply) memory apply refused:
 *                                     the slice tuned by hand, shift and tone
 *   flexhost airrefuse <port> <log>   (--refuse-apply --air-on-apply) refused
 *                                     with another station on the air: tuned
 *                                     only once it is back on receive
 *   flexhost change    <port> <log>   (--change-after 6) a memory renamed,
 *                                     one added, the one shown removed
 *   flexhost other     <port> <log>   another station tunes the slice away
 *
 * Each check prints PASS or FAIL; the exit status counts the failures. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "radio.h"

bool fh_logged(const char *needle);
int  fh_log_count(const char *needle);
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

/* The mock's log (run.sh writes it): what the knob sent, and where the slice
 * would transmit after it. */
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
/* The last "transmits on X MHz": X, or "" for none. */
static const char *last_tx(void)
{
    static char out[24];
    out[0] = 0;
    const char *t = mock_text(), *p, *last = NULL;
    for (p = t; (p = strstr(p, "transmits on ")); p++) last = p;
    if (last) sscanf(last + 13, "%23s", out);
    return out;
}
static bool mock_has(const char *needle) { return strstr(mock_text(), needle) != NULL; }
/* Where a needle first is in the mock's log: its offset, or -1. */
static long mock_at(const char *needle)
{
    const char *t = mock_text(), *p = strstr(t, needle);
    return p ? (long)(p - t) : -1;
}

static radio_status_t s;
static radio_status_t *st(void) { radio_get_status(&s); return &s; }
static void ms(int n) { usleep((useconds_t)n * 1000); }

static bool wait_log(const char *needle, int ms_max)
{
    for (int t = 0; t < ms_max; t += 20) { if (fh_logged(needle)) return true; ms(20); }
    return false;
}

static void dump(const char *when)
{
    st();
    printf("  [%s] link=%d f=%lld mode=%s has_mem=%d mem_all=%d mem_state=%u mem_ch=%u name='%s' dup=%d off=%ld tone=%u "
           "n_ant=%u ant=%u have_ant=%d names='%s' n_tx=%u tx=%u have_tx=%d tx_names='%s'\n",
           when, s.link, (long long)s.f_display, s.mode, s.has_memories, s.mem_all, s.mem_state,
           (unsigned)s.mem_ch, s.mem_name, s.mem_duplex, (long)s.mem_offset_hz, (unsigned)s.mem_tone_dhz,
           s.n_ant, s.ant, s.have_ant, s.ant_names, s.n_tx_ant, s.tx_ant, s.have_tx_ant, s.tx_ant_names);
    fflush(stdout);
}

/* A second station on the mock, changing the slice as another client would. */
static void other_station(int port, const char *cmd)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { perror("connect"); return; }
    char line[256];
    snprintf(line, sizeof line, "C1|%s\n", cmd);
    ms(200);
    if (write(fd, line, strlen(line)) < 0) perror("write");
    ms(300);
    close(fd);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const char *sc = argv[1];
    const int port = atoi(argv[2]);
    snprintf(s_mocklog, sizeof s_mocklog, "%s", argc > 3 ? argv[3] : "logs/mock.log");
    radio_start("127.0.0.1", (uint16_t)port, "", "");
    int t = 0;
    while (st()->link != RADIO_LINK_READY && t < 8000) { ms(50); t += 50; }
    check(s.link == RADIO_LINK_READY, "link READY (%d ms)", t);
    ms(800);
    dump("ready");
    check(s.has_memories, "has_memories");
    check(s.mem_all, "mem_all");
    check(s.n_ant == 6 && !strcmp(s.ant_names, "ANT1,ANT2,RX_A,RX_B,XVTA,XVTB"), "receive antennas listed: %u '%s'", s.n_ant, s.ant_names);
    check(s.n_tx_ant == 4 && !strcmp(s.tx_ant_names, "ANT1,ANT2,XVTA,XVTB"), "transmit antennas listed: %u '%s'", s.n_tx_ant, s.tx_ant_names);
    check(s.have_ant && s.ant == 0 && s.have_tx_ant && s.tx_ant == 0, "ANT1 both ways");

    if (!strcmp(sc, "basic")) {
        radio_set_antenna(2, false);
        ms(300);
        dump("rxant");
        check(fh_logged("receive antenna: RX_A"), "rxant=RX_A sent");
        check(st()->ant == 2, "status shows RX_A (ant=2)");
        radio_set_tx_antenna(1);
        ms(300);
        check(fh_logged("transmit antenna: ANT2"), "txant=ANT2 sent");
        check(st()->tx_ant == 1, "status shows TX ANT2");
        radio_set_antenna(9, false);           /* past the list */
        ms(200);
        check(st()->ant == 2, "an antenna past the list changes nothing");

        radio_memory_mode(true);
        ms(500);
        dump("memory mode");
        check(st()->mem_state == RADIO_MEM_READY, "memory mode READY");
        check(fh_logged("memory mode: M"), "memory mode logged");
        const unsigned first = s.mem_ch;
        const long long f0 = (long long)s.f_display;
        /* one detent: the next memory up in frequency */
        radio_tune_by(1, 1, 100);
        ms(400);
        dump("+1");
        check(s.mem_ch != first && (long long)s.f_display > f0, "one detent: the next memory up (M%u %lld -> M%u %lld)", first, f0, (unsigned)s.mem_ch, (long long)s.f_display);
        /* five quick detents */
        for (int i = 0; i < 5; i++) { radio_tune_by(1, 1, 100); ms(10); }
        ms(500);
        dump("+5");
        radio_tune_by(-1, 1, 100);
        ms(400);
        dump("-1");
        /* the memory with a shift: find it by stepping */
        for (int i = 0; i < 6 && st()->mem_duplex == 0; i++) { radio_tune_by(1, 1, 100); ms(300); }
        dump("shifted");
        check(s.mem_duplex == -1 && s.mem_offset_hz == 100000 && s.mem_tone_dhz == 797 && !strcmp(s.mem_name, "ON0TEN"),
              "ON0TEN shows DUP- 100 kHz T79.7");
        ms(300);
        check(!strcmp(last_tx(), "29.520000"), "ON0TEN applied: the slice transmits 100 kHz down, on 29.520 (mock: %s)", last_tx());
        check(mock_has("tx_offset_freq=-0.100000 fm_tone_mode=ctcss_tx fm_tone_value=79.7"),
              "...its TX offset and tone set after memory apply");
        /* the next memory up, a simplex one: no shift left behind */
        radio_tune_by(1, 1, 100);
        ms(400);
        dump("after ON0TEN");
        check(st()->mem_duplex == 0 && !strcmp(last_tx(), "50.150000"),
              "the next memory, simplex: transmits on its own frequency (mock: %s)", last_tx());
        /* up the list: 145.000 shifted up 600 kHz, then 70 cm 7.6 MHz down */
        radio_tune_by(1, 1, 100);
        ms(400);
        dump("up");
        check(st()->mem_duplex == 1 && !strcmp(last_tx(), "145.600000"),
              "a memory shifted up: transmits 600 kHz up (mock: %s)", last_tx());
        radio_tune_by(1, 1, 100);
        ms(400);
        dump("70 cm");
        check(st()->mem_duplex == -1 && st()->mem_offset_hz == 7600000 && !strcmp(last_tx(), "431.550000"),
              "70 cm, 7.6 MHz down: transmits on 431.550 (mock: %s)", last_tx());
        check(mock_has("fm_repeater_offset_freq=7.600000 tx_offset_freq=-7.600000 fm_tone_mode=ctcss_tx fm_tone_value=74.4"),
              "...its offset and tone, in the radio's words");
        radio_tune_by(-3, 1, 100);
        ms(500);
        check(!strcmp(last_tx(), "29.520000"), "back on ON0TEN: 29.520 again (mock: %s)", last_tx());
        radio_memory_mode(false);
        ms(400);
        dump("vfo");
        check(st()->mem_state == RADIO_MEM_OFF, "back to VFO");
        check(fh_logged("VFO mode, simplex"), "VFO mode, simplex logged");
        check(!strcmp(last_tx(), "29.620000"), "VFO: simplex, transmits on the receive frequency (mock: %s)", last_tx());
        check(mock_has("repeater_offset_dir=simplex tx_offset_freq=0.000000"), "...simplex sent in lower case, with the offset zeroed");
        check(!mock_has("!!!"), "the mock saw nothing amiss");
        /* tuning in VFO mode works */
        const long long fv = (long long)st()->f_display;
        radio_tune_by(10, 1, 100);
        ms(400);
        check((long long)st()->f_display == fv + 1000, "VFO tunes again: %lld -> %lld", fv, (long long)st()->f_display);
        /* goto_freq in memory mode leaves it */
        radio_memory_mode(true);
        ms(400);
        radio_goto_freq(7100000);
        ms(500);
        dump("goto");
        check(st()->mem_state == RADIO_MEM_OFF, "a frequency asked for leaves memory mode");
    } else if (!strcmp(sc, "refuse")) {
        radio_memory_mode(true);
        ms(800);
        dump("refused");
        check(wait_log("tuned there by hand", 1500), "refused apply: tuned by hand");
        ms(300);
        check(mock_at("slice tune 0 14.074000") >= 0 &&
              mock_at("slice tune 0 14.074000") < mock_at("C27|slice set 0 repeater_offset_dir=simplex"),
              "...tuned, then its shift and tone set");
        /* The refused memory with a shift: tuned by hand, shift and tone too. */
        for (int i = 0; i < 6 && st()->mem_duplex == 0; i++) { radio_tune_by(1, 1, 100); ms(400); }
        ms(400);
        dump("by hand, shifted");
        check(!strcmp(last_tx(), "29.520000"), "a repeater memory tuned by hand transmits 100 kHz down (mock: %s)", last_tx());
    } else if (!strcmp(sc, "airrefuse")) {
        /* (mock --refuse-apply --air-on-apply) The refusal comes with
         * another station on the air: nothing tuned until it is off. */
        radio_memory_mode(true);
        check(wait_log("radio transmitting (another station)", 1500), "another station on the air");
        ms(300);
        check(!fh_logged("tuned there by hand"), "refused on the air: not tuned by hand yet");
        check(wait_log("tuned there by hand", 3000), "...tuned by hand once it is back on receive");
        ms(300);
        const long back = mock_at("another station back on receive"), tune = mock_at("|slice tune 0");
        check(back >= 0 && tune > back, "the slice tuned only after the other station's over (%ld, %ld)", back, tune);
    } else if (!strcmp(sc, "change")) {
        radio_memory_mode(true);
        ms(400);
        dump("before");
        ms(7000);
        dump("after changes");
        check(fh_logged("memory 4, the one shown, was removed") || st()->mem_state == RADIO_MEM_READY, "still sane after rename/add/remove");
    } else if (!strcmp(sc, "other")) {
        radio_memory_mode(true);
        ms(500);
        dump("mem");
        other_station(port, "slice set 0 RF_frequency=7.150000");
        ms(1500);
        dump("after other");
        check(fh_logged("elsewhere: memory mode left"), "tuned elsewhere: memory mode left");
    }
    printf("%d failed\n", s_fails);
    fflush(stdout);
    _exit(s_fails ? 1 : 0);
}
