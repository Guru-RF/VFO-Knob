/* The WiFi Test firmware's arithmetic: see wifitest.h. */
#include "wifitest.h"

#include <stdio.h>
#include <string.h>

void wt_reset(wt_t *w)
{
    wt_ping_t keep[WT_TARGETS];
    memcpy(keep, w->p, sizeof keep);
    w->up_s = 0;
    w->rssi_min = w->rssi_max = 0;
    w->rssi_sum = 0;
    w->rssi_n = 0;
    memset(w->rssi_hist, 0, sizeof w->rssi_hist);
    w->down_s = 0;
    w->drops = w->roams = w->beacons = w->ip_changes = w->silences = 0;
    w->last_reason = 0;
    memset(w->p, 0, sizeof w->p);
    for (int i = 0; i < WT_TARGETS; i++) {
        memcpy(w->p[i].name, keep[i].name, sizeof w->p[i].name);
        memcpy(w->p[i].addr, keep[i].addr, sizeof w->p[i].addr);
        w->p[i].state = keep[i].state;
    }
    w->dns_ok = w->dns_fail = w->mdns_ok = w->mdns_fail = 0;
    w->dns_ms = 0;
    w->n_ev = 0;
}

void wt_ping_sent(wt_ping_t *p)
{
    p->sent++;
    if (p->cur_sent < 255) p->cur_sent++;
}

void wt_ping_answer(wt_ping_t *p, uint32_t rtt_ms, uint32_t since_ms)
{
    if (rtt_ms < 1) rtt_ms = 1;                 /* under a millisecond: 1, as 0 says none */
    if (rtt_ms > 0x7FFE) rtt_ms = 0x7FFE;
    const uint16_t ms = (uint16_t)rtt_ms;
    p->got++;
    if (ms > WT_LATE_MS) p->late++;
    p->sum_ms += ms;
    if (!p->min_ms || ms < p->min_ms) p->min_ms = ms;
    if (ms > p->max_ms) p->max_ms = ms;
    p->last_ms = ms;
    if (since_ms > p->worst_gap_ms) p->worst_gap_ms = since_ms;
    p->gap_ms = 0;
    if (ms > p->cur) p->cur = ms;
}

void wt_ping_lost(wt_ping_t *p, uint32_t since_ms)
{
    p->last_ms = WT_LOST;
    p->gap_ms  = since_ms;
    if (p->cur_lost < 255) p->cur_lost++;
}

/* A second's slot: its worst answer, the top bit set if any went unanswered
 * (WT_LOST if none came back at all), 0 if none was asked. */
static uint16_t slot(const wt_ping_t *p)
{
    if (!p->cur_sent && !p->cur_lost) return 0;
    if (!p->cur) return p->cur_lost ? WT_LOST : 0;
    return (uint16_t)(p->cur | (p->cur_lost ? 0x8000 : 0));
}

void wt_second(wt_t *w, int8_t rssi)
{
    w->up_s++;
    if (w->joined) w->join_s++;
    else w->down_s++;
    memmove(w->rssi_hist, w->rssi_hist + 1, sizeof w->rssi_hist - sizeof w->rssi_hist[0]);
    w->rssi_hist[WT_HIST - 1] = rssi;
    w->rssi = rssi;
    if (rssi) {
        if (!w->rssi_n || rssi < w->rssi_min) w->rssi_min = rssi;
        if (!w->rssi_n || rssi > w->rssi_max) w->rssi_max = rssi;
        w->rssi_sum += rssi;
        w->rssi_n++;
    }
    for (int i = 0; i < WT_TARGETS; i++) {
        wt_ping_t *p = &w->p[i];
        memmove(p->hist, p->hist + 1, sizeof p->hist - sizeof p->hist[0]);
        p->hist[WT_HIST - 1] = slot(p);
        p->cur = 0;
        p->cur_sent = p->cur_lost = 0;
    }
}

void wt_event(wt_t *w, const wt_event_t *e)
{
    const int keep = w->n_ev < WT_EVENTS ? w->n_ev : WT_EVENTS - 1;
    memmove(&w->ev[1], &w->ev[0], (size_t)keep * sizeof w->ev[0]);
    w->ev[0] = *e;
    w->n_ev = (uint8_t)(keep + 1);
}

unsigned wt_loss_pm(const wt_ping_t *p)
{
    /* Those still on their way are neither: sent counts them, got not yet. */
    if (!p->sent) return 0;
    const uint32_t lost = p->sent > p->got ? p->sent - p->got : 0;
    return (unsigned)((lost * 1000u + p->sent / 2) / p->sent);
}

unsigned wt_mean_ms(const wt_ping_t *p)
{
    return p->got ? (unsigned)((p->sum_ms + p->got / 2) / p->got) : 0;
}

int wt_rssi_recent(const wt_t *w)
{
    int sum = 0, n = 0;
    for (int i = 0; i < WT_HIST; i++)
        if (w->rssi_hist[i]) { sum += w->rssi_hist[i]; n++; }
    return n ? (sum - n / 2) / n : 0;
}

const char *wt_reason_text(uint8_t r)
{
    switch (r) {
    case 0:   return "no reason given";
    case 1:   return "unspecified";
    case 2:   return "its login expired";
    case 3:   return "the access point left";
    case 4:   return "sent away for idling";
    case 5:   return "access point full";
    case 6:
    case 7:   return "access point forgot it";
    case 8:   return "left the network";
    case 9:   return "not logged in";
    case 10:
    case 11:  return "refused its radio";
    case 12:  return "steered to another AP";
    case 13:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 24:
    case 29:  return "security not agreed";
    case 14:  return "key check failed";
    case 15:
    case 204: return "handshake timed out";
    case 16:  return "group key timed out";
    case 23:  return "802.1X login failed";
    case 34:  return "too many missed ACKs";
    case 36:  return "station leaving";
    case 39:  return "timed out";
    case 46:
    case 47:  return "the access point ended it";
    case 200: return "beacons lost";
    case 201: return "network not found";
    case 202: return "wrong password?";
    case 203: return "access point refused it";
    case 205: return "could not connect";
    case 206: return "access point restarted";
    case 207: return "roaming";
    case 208: return "AP kept it waiting";
    case 209: return "protected frames timed out";
    case 210: return "no AP with its security";
    case 211: return "security too weak";
    case 212: return "signal too weak";
    default:  return "another reason";
    }
}

const char *wt_reason_hint(uint8_t r)
{
    switch (r) {
    case 200:
        return "The access point went unheard for seconds: too far, a wall or interference in between, "
               "or the access point restarted or changed its channel.";
    case 4:
        return "The access point drops stations it thinks idle: look for an inactivity or idle "
               "timeout in the router's WiFi settings.";
    case 2:
    case 3:
    case 6:
    case 7:
    case 46:
    case 47:
        return "The access point ended the link itself: it may have restarted, changed its settings, "
               "or be steering stations between bands or access points.";
    case 12:
    case 207:
        return "A mesh or band steering moved the knob on: the knob has 2.4 GHz only, so steering "
               "it to 5 GHz fails. Turn band steering off, or give 2.4 GHz a name of its own.";
    case 5:
        return "The access point has as many stations as it takes.";
    case 15:
    case 16:
    case 204:
        return "The key exchange did not finish: a weak signal, or WPA3 or PMF settings the knob "
               "does not agree with. WPA2 (AES) is the safe choice.";
    case 202:
        return "The password was refused.";
    case 201:
        return "The network was not heard at all: out of reach, or not on 2.4 GHz.";
    case 209:
        return "Protected management frames went unanswered: try PMF \"optional\", not \"required\".";
    case 34:
        return "The access point stopped acknowledging: a weak or crowded channel.";
    case 206:
        return "The access point restarted.";
    default:
        return "";
    }
}

const char *wt_signal_word(int rssi)
{
    if (!rssi) return "none";
    if (rssi >= -55) return "excellent";
    if (rssi >= -65) return "good";
    if (rssi >= -72) return "fair";
    if (rssi >= -80) return "weak";
    return "poor";
}

/* x.x %, from per mille. */
static void pct(char *o, size_t n, unsigned pm) { snprintf(o, n, "%u.%u %%", pm / 10, pm % 10); }

int wt_verdict(const wt_t *w, char *why, size_t n)
{
    char p[16];
    if (!w->joined) {
        if (w->drops) snprintf(why, n, "off WiFi: %s", wt_reason_text(w->last_reason));
        else snprintf(why, n, "joining WiFi...");
        return WT_POOR;
    }
    const wt_ping_t *g = &w->p[WT_T_GW];
    const int rs = wt_rssi_recent(w);
    /* What breaks a radio's link. */
    if (w->drops == 1) {
        snprintf(why, n, "dropped once: %s", wt_reason_text(w->last_reason));
        return WT_POOR;
    }
    if (w->drops) {
        snprintf(why, n, "dropped %u times: %s", (unsigned)w->drops, wt_reason_text(w->last_reason));
        return WT_POOR;
    }
    if (rs && rs < -80) {
        snprintf(why, n, "very weak signal, %d dBm", rs);
        return WT_POOR;
    }
    if (g->sent >= 100 && wt_loss_pm(g) >= 50) {
        pct(p, sizeof p, wt_loss_pm(g));
        snprintf(why, n, "router loses %s of pings", p);
        return WT_POOR;
    }
    if (g->worst_gap_ms >= 3000) {
        snprintf(why, n, "router silent %u.%u s", (unsigned)(g->worst_gap_ms / 1000),
                 (unsigned)(g->worst_gap_ms % 1000 / 100));
        return WT_POOR;
    }
    /* What a radio's audio would feel. */
    if (rs && rs < -72) {
        snprintf(why, n, "weak signal, %d dBm", rs);
        return WT_FAIR;
    }
    if (g->sent >= 100 && wt_loss_pm(g) >= 10) {
        pct(p, sizeof p, wt_loss_pm(g));
        snprintf(why, n, "router loses %s of pings", p);
        return WT_FAIR;
    }
    if (g->got >= 100 && g->late * 1000u / g->got >= 50) {
        pct(p, sizeof p, (unsigned)(g->late * 1000u / g->got));
        snprintf(why, n, "router slow: %s late", p);
        return WT_FAIR;
    }
    if (g->worst_gap_ms >= 1000) {
        snprintf(why, n, "router silent %u.%u s", (unsigned)(g->worst_gap_ms / 1000),
                 (unsigned)(g->worst_gap_ms % 1000 / 100));
        return WT_FAIR;
    }
    for (int i = WT_T_HOST; i < WT_T_HOST + WT_HOSTS; i++) {
        const wt_ping_t *h = &w->p[i];
        if (h->state == WT_P_NOTFOUND) {
            snprintf(why, n, "%.24s not found", h->name);
            return WT_FAIR;
        }
        if (h->state != WT_P_ON || h->sent < 100) continue;
        if (!h->got) {
            snprintf(why, n, "%.24s: no answer", h->name);
            return WT_FAIR;
        }
        if (wt_loss_pm(h) >= 20) {
            pct(p, sizeof p, wt_loss_pm(h));
            snprintf(why, n, "%.20s loses %s", h->name, p);
            return WT_FAIR;
        }
    }
    const wt_ping_t *net = &w->p[WT_T_NET];
    if (net->state == WT_P_ON && net->sent >= 30 && !net->got) {
        snprintf(why, n, "no internet");
        return WT_FAIR;
    }
    if (net->state == WT_P_ON && net->sent >= 60 && wt_loss_pm(net) >= 50) {
        pct(p, sizeof p, wt_loss_pm(net));
        snprintf(why, n, "internet loses %s", p);
        return WT_FAIR;
    }
    if (w->beacons) {
        snprintf(why, n, "beacons missed %u time%s", (unsigned)w->beacons, w->beacons == 1 ? "" : "s");
        return WT_FAIR;
    }
    /* Channels 5 apart do not overlap: 1, 6 and 11. */
    if (w->scan_at_s && w->channel) {
        int near = 0;
        for (int c = 1; c <= WT_CHANNELS; c++)
            if (c - w->channel < 5 && w->channel - c < 5) near += w->per_ch[c];
        if (near >= 12) {
            snprintf(why, n, "crowded: %d networks near ch %u", near, (unsigned)w->channel);
            return WT_FAIR;
        }
    }
    if (w->up_s < 30) {
        snprintf(why, n, "measuring...");
        return WT_FINE;
    }
    snprintf(why, n, "WiFi looks fine");
    return WT_FINE;
}
