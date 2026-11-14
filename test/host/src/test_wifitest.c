/* The WiFi Test firmware's arithmetic (components/vfo_core/src/wifitest.c):
 * the pings' counts, the minute a second a slot, the loss and the mean; the
 * signal's minute; the events newest first, the oldest let go; the counts
 * begun again with the targets kept; the reasons in words; and the verdict,
 * the worst thing first. */
#include "tiny.h"
#include "wifitest.h"

#include <string.h>

static wt_t w;

static void joined_quiet(void)
{
    memset(&w, 0, sizeof w);
    w.joined = true;
    w.up_s = 600;
    w.channel = 6;
    for (int i = 0; i < WT_HIST; i++) w.rssi_hist[i] = -55;
    w.p[WT_T_GW].state = WT_P_ON;
    w.p[WT_T_NET].state = WT_P_ON;
}

int main(void)
{
    char why[48];

    CASE("pings: sent, answered, lost; the slowest of the second into the minute");
    {
        wt_ping_t p = { 0 };
        wt_ping_sent(&p); wt_ping_answer(&p, 3, 200);
        wt_ping_sent(&p); wt_ping_answer(&p, 0, 200);       /* under a millisecond: 1 */
        wt_ping_sent(&p); wt_ping_answer(&p, 150, 200);     /* late */
        wt_ping_sent(&p); wt_ping_lost(&p, 1200);
        CHECK_EQ(p.sent, 4u);
        CHECK_EQ(p.got, 3u);
        CHECK_EQ(p.late, 1u);
        CHECK_EQ(p.min_ms, 1);
        CHECK_EQ(p.max_ms, 150);
        CHECK_EQ(p.last_ms, WT_LOST);
        CHECK_EQ(p.gap_ms, 1200u);
        CHECK_EQ(wt_loss_pm(&p), 250u);
        CHECK_EQ(wt_mean_ms(&p), 51u);                      /* (3 + 1 + 150) / 3 */
        wt_t t = { 0 };
        t.p[0] = p;
        wt_second(&t, -60);
        CHECK_EQ(t.p[0].hist[WT_HIST - 1], 150 | 0x8000);   /* the slowest, and a loss */
        CHECK_EQ(t.p[0].cur, 0);
        wt_ping_sent(&t.p[0]); wt_ping_lost(&t.p[0], 1400);
        wt_second(&t, -60);
        CHECK_EQ(t.p[0].hist[WT_HIST - 1], WT_LOST);        /* none came back */
        CHECK_EQ(t.p[0].hist[WT_HIST - 2], 150 | 0x8000);
        wt_second(&t, -60);
        CHECK_EQ(t.p[0].hist[WT_HIST - 1], 0);              /* none asked */
        wt_ping_sent(&t.p[0]); wt_ping_answer(&t.p[0], 7, 2600);
        CHECK_EQ(t.p[0].worst_gap_ms, 2600u);
        CHECK_EQ(t.p[0].gap_ms, 0u);
    }

    CASE("the signal's minute, its extremes, its mean");
    {
        wt_t t = { 0 };
        t.joined = true;
        wt_second(&t, -50);
        wt_second(&t, -70);
        t.joined = false;
        wt_second(&t, 0);
        CHECK_EQ(t.up_s, 3u);
        CHECK_EQ(t.join_s, 2u);
        CHECK_EQ(t.down_s, 1u);
        CHECK_EQ(t.rssi_min, -70);
        CHECK_EQ(t.rssi_max, -50);
        CHECK_EQ(t.rssi_hist[WT_HIST - 1], 0);
        CHECK_EQ(wt_rssi_recent(&t), -60);
        CHECK_EQ(t.rssi, 0);
    }

    CASE("events: newest first, the oldest let go");
    {
        wt_t t = { 0 };
        for (int i = 0; i < WT_EVENTS + 3; i++) {
            wt_event_t e = { .at_s = (uint32_t)i, .kind = WT_EV_SILENT };
            wt_event(&t, &e);
        }
        CHECK_EQ(t.n_ev, WT_EVENTS);
        CHECK_EQ(t.ev[0].at_s, (uint32_t)(WT_EVENTS + 2));
        CHECK_EQ(t.ev[WT_EVENTS - 1].at_s, 3u);
    }

    CASE("counting again: the targets kept, their counts gone");
    {
        joined_quiet();
        strcpy(w.p[WT_T_HOST].name, "IC-705.local");
        strcpy(w.p[WT_T_HOST].addr, "192.168.1.20");
        w.p[WT_T_HOST].state = WT_P_ON;
        w.p[WT_T_HOST].sent = 99;
        w.drops = 3;
        wt_event_t e = { .kind = WT_EV_DROPPED };
        wt_event(&w, &e);
        wt_reset(&w);
        CHECK_STR(w.p[WT_T_HOST].name, "IC-705.local");
        CHECK_STR(w.p[WT_T_HOST].addr, "192.168.1.20");
        CHECK_EQ(w.p[WT_T_HOST].state, WT_P_ON);
        CHECK_EQ(w.p[WT_T_HOST].sent, 0u);
        CHECK_EQ(w.drops, 0);
        CHECK_EQ(w.n_ev, 0);
        CHECK_EQ(w.up_s, 0u);
        CHECK(w.joined);                                    /* the link as it is */
    }

    CASE("reasons in words, and a hint where there is one");
    CHECK_STR(wt_reason_text(200), "beacons lost");
    CHECK_STR(wt_reason_text(4), "sent away for idling");
    CHECK_STR(wt_reason_text(15), "handshake timed out");
    CHECK_STR(wt_reason_text(204), "handshake timed out");
    CHECK_STR(wt_reason_text(250), "another reason");
    CHECK(wt_reason_hint(200)[0] && wt_reason_hint(4)[0] && wt_reason_hint(12)[0]);
    CHECK(!wt_reason_hint(1)[0]);
    CHECK_STR(wt_signal_word(-50), "excellent");
    CHECK_STR(wt_signal_word(-65), "good");
    CHECK_STR(wt_signal_word(-72), "fair");
    CHECK_STR(wt_signal_word(-80), "weak");
    CHECK_STR(wt_signal_word(-81), "poor");
    CHECK_STR(wt_signal_word(0), "none");

    CASE("verdict: off the network, and joining");
    memset(&w, 0, sizeof w);
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_POOR);
    CHECK_STR(why, "joining WiFi...");
    w.drops = 1; w.last_reason = 200;
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_POOR);
    CHECK_STR(why, "off WiFi: beacons lost");

    CASE("verdict: all well, and in its first half minute");
    joined_quiet();
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_FINE);
    CHECK_STR(why, "WiFi looks fine");
    w.up_s = 10;
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_FINE);
    CHECK_STR(why, "measuring...");

    CASE("verdict: drops first, over everything else");
    joined_quiet();
    w.drops = 2; w.last_reason = 4;
    for (int i = 0; i < WT_HIST; i++) w.rssi_hist[i] = -85;
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_POOR);
    CHECK_STR(why, "dropped 2 times: sent away for idling");
    w.drops = 1;
    CHECK_STR((wt_verdict(&w, why, sizeof why), why), "dropped once: sent away for idling");

    CASE("verdict: the signal, poor and weak");
    joined_quiet();
    for (int i = 0; i < WT_HIST; i++) w.rssi_hist[i] = -84;
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_POOR);
    CHECK_STR(why, "very weak signal, -84 dBm");
    for (int i = 0; i < WT_HIST; i++) w.rssi_hist[i] = -76;
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_FAIR);
    CHECK_STR(why, "weak signal, -76 dBm");

    CASE("verdict: the router's losses, slowness and silence");
    joined_quiet();
    w.p[WT_T_GW].sent = 1000; w.p[WT_T_GW].got = 940;
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_POOR);
    CHECK_STR(why, "router loses 6.0 % of pings");
    w.p[WT_T_GW].got = 985;
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_FAIR);
    CHECK_STR(why, "router loses 1.5 % of pings");
    w.p[WT_T_GW].got = 1000; w.p[WT_T_GW].late = 80;
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_FAIR);
    CHECK_STR(why, "router slow: 8.0 % late");
    w.p[WT_T_GW].late = 0; w.p[WT_T_GW].worst_gap_ms = 1700;
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_FAIR);
    CHECK_STR(why, "router silent 1.7 s");
    w.p[WT_T_GW].worst_gap_ms = 4200;
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_POOR);
    CHECK_STR(why, "router silent 4.2 s");
    w.p[WT_T_GW].sent = 50; w.p[WT_T_GW].got = 10; w.p[WT_T_GW].worst_gap_ms = 0;
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_FINE);     /* too few yet to say */

    CASE("verdict: a host not found, silent, or losing; the internet");
    joined_quiet();
    strcpy(w.p[WT_T_HOST].name, "IC-705.local");
    w.p[WT_T_HOST].state = WT_P_NOTFOUND;
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_FAIR);
    CHECK_STR(why, "IC-705.local not found");
    w.p[WT_T_HOST].state = WT_P_ON; w.p[WT_T_HOST].sent = 200;
    CHECK_STR((wt_verdict(&w, why, sizeof why), why), "IC-705.local: no answer");
    w.p[WT_T_HOST].got = 190;
    CHECK_STR((wt_verdict(&w, why, sizeof why), why), "IC-705.local loses 5.0 %");
    w.p[WT_T_HOST].got = 200;
    w.p[WT_T_NET].sent = 40;
    CHECK_STR((wt_verdict(&w, why, sizeof why), why), "no internet");
    w.p[WT_T_NET].sent = 100; w.p[WT_T_NET].got = 90;
    CHECK_STR((wt_verdict(&w, why, sizeof why), why), "internet loses 10.0 %");

    CASE("verdict: beacons missed, then a crowded channel");
    joined_quiet();
    w.beacons = 1;
    CHECK_STR((wt_verdict(&w, why, sizeof why), why), "beacons missed 1 time");
    w.beacons = 0;
    w.scan_at_s = 5;
    w.per_ch[1] = 3; w.per_ch[2] = 3; w.per_ch[6] = 6; w.per_ch[10] = 3; w.per_ch[11] = 9; /* 1 and 11 clear of 6 */
    CHECK_STR((wt_verdict(&w, why, sizeof why), why), "crowded: 12 networks near ch 6");
    w.per_ch[10] = 2;
    CHECK_EQ(wt_verdict(&w, why, sizeof why), WT_FINE);

    printf("%s: %d checks, %d failed\n", __FILE__, t_run, t_fail);
    return t_fail ? 1 : 0;
}
