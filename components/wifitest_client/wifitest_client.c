/* The WiFi Test firmware's radio.h, and what it measures (VFO_RADIO=wifitest).
 *
 * There is no radio. It answers for a receiver always ready, as the Clock
 * does, so nothing says NO LINK over the face, and the face is the test
 * (ui.c WIFITEST_FACE). radio_start() begins the measuring, once the knob is
 * on its network:
 *
 *  - the signal, every second, and the access point it comes from -- its
 *    channel, width, security, country;
 *  - every drop, with the reason the WiFi driver gives -- beacons lost, sent
 *    away for idling, steered elsewhere -- every join again, through the same
 *    access point or another, every missed beacon, every new address;
 *  - pings: the router five times a second, the hosts as often -- the
 *    radios and computers the knob's other firmwares talk to, found there
 *    (net_prov_hosts_elsewhere) or set on the page -- and the internet
 *    (1.1.1.1) once a second; each answer's time, the late ones, the lost,
 *    and the longest silence;
 *  - each minute, a name on the internet looked up, and the hosts' names --
 *    mDNS for a .local one, which a mesh or a guest network can swallow;
 *  - the channels round it, scanned at the start and every half hour (the
 *    pings paused for the second or two it takes), and when the page asks.
 *
 * All of it in wt_t (components/vfo_core wifitest.h), which the face and the
 * page copy (wifitest_snapshot), and a line every 30 s to the log, each
 * event as it comes. The pings are this task's own, over one raw socket:
 * an echo's identifier says which target it was for, its sequence number
 * which one, so that they go on at their pace through any loss. */
#include "radio.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mdns.h"

#include "net_prov.h"
#include "wifitest_client.h"

static const char *TAG = "wifitest";

const char *radio_link_name(void) { return "WIFI TEST"; }

static void start_test(void);

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    (void)host; (void)port; (void)user; (void)pass;
    start_test();
    return ESP_OK;
}

int64_t radio_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz)
{
    (void)detents; (void)accel_mult; (void)step_hz;
    return 0;
}

void radio_get_status(radio_status_t *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
    o->link    = RADIO_LINK_READY;
    o->rx_only = true;
}

bool radio_is_ready(void) { return true; }
bool radio_on_air(void)   { return false; }

void radio_set_step(int32_t step_hz)               { (void)step_hz; }
void radio_audio_suspend(bool suspend)             { (void)suspend; }
void radio_ptt_key(void)                           {}
void radio_ptt_unkey(void)                         {}
void radio_ptt_toggle(void)                        {}
void radio_ptt_force_abort(uint8_t reason)         { (void)reason; }
void radio_set_mode(const char *mode)              { (void)mode; }
void radio_set_filter(int32_t lo, int32_t hi)      { (void)lo; (void)hi; }
void radio_select_filter(uint8_t n)                { (void)n; }
void radio_set_rit(int32_t hz)                     { (void)hz; }
void radio_set_agc(const char *agc)                { (void)agc; }
void radio_set_gain(int8_t gain)                   { (void)gain; }
void radio_goto_freq(int64_t hz)                   { (void)hz; }
void radio_memory_mode(bool on)                    { (void)on; }
void radio_memory_group(uint8_t group)             { (void)group; }
void radio_select_rx(uint8_t rx)                   { (void)rx; }
void radio_set_antenna(uint8_t ant, bool rx_ant)   { (void)ant; (void)rx_ant; }
void radio_tune(void)                              {}
void radio_atu_tune(void)                          {}
void radio_atu_memories(bool on)                   { (void)on; }
void radio_set_dsp(uint8_t which, bool on)         { (void)which; (void)on; }
void radio_set_rf_gain(uint8_t pct)                { (void)pct; }
void radio_set_rf_power(uint8_t pct)               { (void)pct; }
void radio_set_tuner(bool on)                      { (void)on; }
void radio_set_squelch(uint8_t pct)                { (void)pct; }
void radio_tg_lock(bool locked)                    { (void)locked; }
void radio_mute(bool muted)                        { (void)muted; }

bool radio_get_choice(uint8_t i, char *title, size_t tn, char *name, size_t nn)
{
    (void)i; (void)title; (void)tn; (void)name; (void)nn;
    return false;
}
void radio_choose(uint8_t i) { (void)i; }

extern const httpd_uri_t wifitest_web_uris[];
extern const size_t      wifitest_web_uris_n;

size_t radio_web_endpoints(const httpd_uri_t **out);
size_t radio_web_endpoints(const httpd_uri_t **out)
{
    *out = wifitest_web_uris;
    return wifitest_web_uris_n;
}

/* ---- what is measured ----------------------------------------------------- */

#define PING_ID        0x5700         /* the echo's identifier: this, the target's index in the low byte */
#define PING_SLOTS     8              /* echoes on their way, a target */
#define PING_WAIT_US   1000000        /* unanswered this long: lost */
#define PING_FAST_US   200000         /* the router and the hosts */
#define PING_SLOW_US   1000000        /* the internet */
#define NET_TARGET     "1.1.1.1"
#define DNS_NAME       "github.com"
#define NAMES_EVERY_S  60
#define SCAN_FIRST_S   8              /* the first scan, once joined */
#define SCAN_EVERY_S   1800
#define LOG_EVERY_S    30
#define SILENT_MS      1000           /* the router this long quiet, joined: an event */

/* The measured, under s_lock: a mutex, not a spinlock -- the face and the
 * page copy all 3 kB of it, and every user is a task (the event loop is one). */
EXT_RAM_BSS_ATTR static wt_t s_w;
static SemaphoreHandle_t s_lock;
#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

typedef struct {
    uint16_t seq;
    int64_t  at;                      /* sent, esp_timer; 0 a free slot */
} echo_t;

/* The pinger's own, this task's alone but for ip (s_lock). */
static struct {
    uint32_t ip;                      /* network order, 0 none */
    int64_t  every, next;
    uint16_t seq;
    echo_t   out[PING_SLOTS];
    int64_t  heard;                   /* its last answer, or the join, whichever is later */
} s_t[WT_TARGETS];

static TaskHandle_t s_task, s_names;
static int          s_sock = -1;
static atomic_bool  s_want_scan, s_want_recount, s_hosts_due = true;
static atomic_bool  s_scan_done, s_void_due, s_joined_now;
static volatile bool s_my_scan;      /* the scan under way is ours: its results ours to read */
static uint8_t      s_last_bssid[6];  /* the access point joined last, through drops */
static int64_t      s_down_at;        /* when the network was lost, 0 joined */
static uint32_t     s_prev_ip;        /* the address before, network order */
static int8_t       s_drop_rssi;      /* the signal at the last second joined */
static int64_t      s_scan_at;        /* our scan's start */
#define SCAN_GIVE_UP_US 10000000      /* a scan with no end in this long: the pings go on */

static int64_t unix_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec > 1700000000 ? (int64_t)tv.tv_sec : 0;
}

static void bssid_text(const uint8_t *b, char *s, size_t n)
{
    snprintf(s, n, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
}

/* An event: its time stamped, kept (s_lock taken here), and logged. */
static void note(uint8_t kind, uint8_t reason, int8_t rssi, uint8_t ch, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
static void note(uint8_t kind, uint8_t reason, int8_t rssi, uint8_t ch, const char *fmt, ...)
{
    wt_event_t e = { .unix_s = unix_now(), .kind = kind, .reason = reason, .rssi = rssi, .channel = ch };
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.what, sizeof e.what, fmt, ap);
    va_end(ap);
    LOCK();
    e.at_s = s_w.up_s;
    wt_event(&s_w, &e);
    UNLOCK();
    ESP_LOGW(TAG, "event: %s", e.what);
}

static const char *auth_text(int a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Ent";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_OWE:             return "OWE";
    default:                        return "other";
    }
}

/* The access point's particulars, from the driver (this task). */
static void ap_read(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    wifi_phy_mode_t pm = WIFI_PHY_MODE_11G;
    const bool have_pm = esp_wifi_sta_get_negotiated_phymode(&pm) == ESP_OK;
    const char *phy = !have_pm ? (ap.phy_11n ? "11n" : ap.phy_11g ? "11g" : "11b")
                    : pm == WIFI_PHY_MODE_11B ? "11b" : pm == WIFI_PHY_MODE_11G ? "11g"
                    : pm == WIFI_PHY_MODE_HT40 ? "11n HT40" : pm == WIFI_PHY_MODE_HE20 ? "11ax" : "11n";
    LOCK();
    memcpy(s_w.ssid, ap.ssid, sizeof s_w.ssid - 1);
    s_w.ssid[sizeof s_w.ssid - 1] = 0;
    memcpy(s_w.bssid, ap.bssid, 6);
    if (!memcmp(s_last_bssid, "\0\0\0\0\0\0", 6)) memcpy(s_last_bssid, ap.bssid, 6);
    s_w.channel = ap.primary;
    s_w.ht40 = ap.second == WIFI_SECOND_CHAN_ABOVE ? 1 : ap.second == WIFI_SECOND_CHAN_BELOW ? 2 : 0;
    strlcpy(s_w.phy, phy, sizeof s_w.phy);
    strlcpy(s_w.auth, auth_text(ap.authmode), sizeof s_w.auth);
    s_w.country[0] = 0;
    if (ap.country.cc[0] > ' ' && ap.country.cc[1] > ' ') {
        s_w.country[0] = ap.country.cc[0];
        s_w.country[1] = ap.country.cc[1];
        s_w.country[2] = 0;
    }
    UNLOCK();
}

/* The router's address, and ours (this task). */
static void ip_read(void)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ii;
    if (!nif || esp_netif_get_ip_info(nif, &ii) != ESP_OK) return;
    esp_netif_dns_info_t dn = { 0 };
    esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &dn);
    char ip[16], gw[16], dns[16];
    snprintf(ip, sizeof ip, IPSTR, IP2STR(&ii.ip));
    snprintf(gw, sizeof gw, IPSTR, IP2STR(&ii.gw));
    snprintf(dns, sizeof dns, IPSTR, IP2STR(&dn.ip.u_addr.ip4));
    LOCK();
    strlcpy(s_w.ip, ip, sizeof s_w.ip);
    strlcpy(s_w.gw, gw, sizeof s_w.gw);
    strlcpy(s_w.dns, dns, sizeof s_w.dns);
    if (ii.gw.addr) {
        strlcpy(s_w.p[WT_T_GW].addr, gw, sizeof s_w.p[WT_T_GW].addr);
        s_w.p[WT_T_GW].state = WT_P_ON;
    }
    s_t[WT_T_GW].ip = ii.gw.addr;
    UNLOCK();
    if (!s_prev_ip) s_prev_ip = ii.ip.addr;
}

/* ---- the WiFi driver's events, on the default event loop ---- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    const int64_t now = esp_timer_get_time();
    char b[18];
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *c = data;
        const bool known = memcmp(s_last_bssid, "\0\0\0\0\0\0", 6) != 0;
        if (known && memcmp(s_last_bssid, c->bssid, 6)) {
            bssid_text(c->bssid, b, sizeof b);
            LOCK();
            s_w.roams++;
            UNLOCK();
            note(WT_EV_ROAMED, 0, 0, c->channel, "through another AP: %s, ch %u", b, (unsigned)c->channel);
        }
        memcpy(s_last_bssid, c->bssid, 6);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        bool was;
        LOCK();
        was = s_w.joined;
        if (was) {
            s_w.joined = false;
            s_w.drops++;
            s_w.last_reason = d->reason;
            s_w.join_s = 0;
        }
        UNLOCK();
        if (was) {
            s_down_at = now;
            atomic_store(&s_void_due, true);
            note(WT_EV_DROPPED, d->reason, d->rssi ? d->rssi : s_drop_rssi, s_w.channel,
                 "dropped: %s (reason %u), %d dBm", wt_reason_text(d->reason), (unsigned)d->reason,
                 d->rssi ? d->rssi : s_drop_rssi);
            const char *h = wt_reason_hint(d->reason);
            if (h[0]) ESP_LOGW(TAG, "  %s", h);
        } else {
            /* Joining again, refused or unheard: the reason kept for the face. */
            LOCK();
            s_w.last_reason = d->reason;
            UNLOCK();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_BEACON_TIMEOUT) {
        LOCK();
        s_w.beacons++;
        const uint8_t ch = s_w.channel;
        UNLOCK();
        note(WT_EV_BEACONS, 200, s_drop_rssi, ch, "beacons missed, %d dBm", s_drop_rssi);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (s_my_scan) atomic_store(&s_scan_done, true);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        const uint32_t ip = e->ip_info.ip.addr;
        if (s_prev_ip && ip != s_prev_ip) {
            char was[16], now_ip[16];
            snprintf(was, sizeof was, IPSTR, IP2STR((esp_ip4_addr_t *)&s_prev_ip));
            snprintf(now_ip, sizeof now_ip, IPSTR, IP2STR(&e->ip_info.ip));
            LOCK();
            s_w.ip_changes++;
            UNLOCK();
            note(WT_EV_ADDRESS, 0, 0, 0, "a new address: %s, was %s", now_ip, was);
        }
        s_prev_ip = ip;
        atomic_store(&s_joined_now, true);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        note(WT_EV_ADDRESS, 0, 0, 0, "the address lost");
    }
}

/* ---- the pings ---- */

static void echo_send(int t, int64_t now)
{
    struct {
        struct icmp_echo_hdr h;
        uint8_t              data[32];
    } pkt;
    memset(&pkt, 0, sizeof pkt);
    ICMPH_TYPE_SET(&pkt.h, ICMP_ECHO);
    ICMPH_CODE_SET(&pkt.h, 0);
    pkt.h.id    = lwip_htons((uint16_t)(PING_ID | t));
    pkt.h.seqno = lwip_htons(++s_t[t].seq);
    for (size_t i = 0; i < sizeof pkt.data; i++) pkt.data[i] = (uint8_t)('a' + i % 26);
    pkt.h.chksum = inet_chksum(&pkt, sizeof pkt);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_len = sizeof to };
    to.sin_addr.s_addr = s_t[t].ip;
    /* A slot: a free one, else the oldest, lost already in all but name. */
    int k = 0;
    for (int i = 0; i < PING_SLOTS; i++) {
        if (!s_t[t].out[i].at) { k = i; break; }
        if (s_t[t].out[i].at < s_t[t].out[k].at) k = i;
    }
    if (s_t[t].out[k].at) {
        LOCK();
        wt_ping_lost(&s_w.p[t], (uint32_t)((now - s_t[t].heard) / 1000));
        UNLOCK();
    }
    s_t[t].out[k].at = 0;
    if (sendto(s_sock, &pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to) != (int)sizeof pkt) return;
    s_t[t].out[k].seq = s_t[t].seq;
    s_t[t].out[k].at  = now;
    LOCK();
    wt_ping_sent(&s_w.p[t]);
    UNLOCK();
}

static void echoes_read(int64_t now)
{
    uint8_t buf[128];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const int n = recvfrom(s_sock, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
        if (n <= 0) return;
        const int ihl = (buf[0] & 0x0F) * 4;
        if (n < ihl + (int)sizeof(struct icmp_echo_hdr)) continue;
        const struct icmp_echo_hdr *e = (const struct icmp_echo_hdr *)(buf + ihl);
        if (ICMPH_TYPE(e) != ICMP_ER) continue;
        const uint16_t id = lwip_ntohs(e->id), seq = lwip_ntohs(e->seqno);
        if ((id & 0xFF00) != PING_ID || (id & 0xFF) >= WT_TARGETS) continue;
        const int t = id & 0xFF;
        for (int i = 0; i < PING_SLOTS; i++) {
            echo_t *o = &s_t[t].out[i];
            if (!o->at || o->seq != seq) continue;
            const uint32_t rtt   = (uint32_t)((now - o->at + 500) / 1000);
            const uint32_t since = (uint32_t)((now - s_t[t].heard) / 1000);
            o->at = 0;
            s_t[t].heard = now;
            bool silent_end;
            LOCK();
            silent_end = t == WT_T_GW && s_w.joined && since >= SILENT_MS + PING_FAST_US / 1000;
            wt_ping_answer(&s_w.p[t], rtt, since);
            if (silent_end) s_w.silences++;
            UNLOCK();
            if (silent_end)
                note(WT_EV_SILENT, 0, s_drop_rssi, s_w.channel, "the router silent %u.%u s, joined",
                     (unsigned)(since / 1000), (unsigned)(since % 1000 / 100));
            break;
        }
    }
}

/* Those sent and not answered in their time: lost. */
static void echoes_expire(int64_t now)
{
    for (int t = 0; t < WT_TARGETS; t++)
        for (int i = 0; i < PING_SLOTS; i++) {
            echo_t *o = &s_t[t].out[i];
            if (!o->at || now - o->at < PING_WAIT_US) continue;
            o->at = 0;
            LOCK();
            wt_ping_lost(&s_w.p[t], (uint32_t)((now - s_t[t].heard) / 1000));
            UNLOCK();
        }
}

/* Those on their way when the network went, or a scan began: neither lost
 * nor answered -- the drop or the scan is what is counted. */
static void echoes_void(int64_t now)
{
    LOCK();
    for (int t = 0; t < WT_TARGETS; t++) {
        for (int i = 0; i < PING_SLOTS; i++) {
            if (!s_t[t].out[i].at) continue;
            s_t[t].out[i].at = 0;
            if (s_w.p[t].sent) s_w.p[t].sent--;
            if (s_w.p[t].cur_sent) s_w.p[t].cur_sent--;
        }
        s_t[t].heard = now;
        s_w.p[t].gap_ms = 0;
    }
    UNLOCK();
}

/* ---- the names: the hosts' and the internet's, each minute, a task of their own ---- */

static bool resolve(const char *name, uint32_t *ip, bool *mdns_used)
{
    *mdns_used = false;
    struct in_addr a;
    if (inet_aton(name, &a)) {          /* an address already */
        *ip = a.s_addr;
        return true;
    }
    const size_t n = strlen(name);
    if (n > 6 && !strcasecmp(name + n - 6, ".local")) {
        char base[64];
        strlcpy(base, name, sizeof base);
        base[n - 6] = 0;
        *mdns_used = true;
        esp_ip4_addr_t m = { 0 };
        if (mdns_query_a(base, 2000, &m) != ESP_OK || !m.addr) return false;
        *ip = m.addr;
        return true;
    }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(name, NULL, &hints, &res) != 0 || !res) return false;
    *ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
}

/* A question for DNS_NAME's address, straight to the network's DNS server
 * and not through lwIP's cache, which answers a name it knew in no time: the
 * server's answer and how long it took, or none in 3 s. */
static bool dns_probe(uint32_t server, uint32_t *ms)
{
    *ms = 0;
    if (!server) return false;
    const int sk = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sk < 0) return false;
    struct timeval tv = { .tv_sec = 3 };
    setsockopt(sk, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    uint8_t q[64];
    size_t n = 0;
    const uint16_t id = (uint16_t)esp_random();
    const uint8_t head[12] = { id >> 8, id & 0xFF, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0 };  /* recursion, one question */
    memcpy(q, head, sizeof head);
    n = sizeof head;
    for (const char *p = DNS_NAME; *p;) {
        const char *dot = strchr(p, '.');
        const size_t l = dot ? (size_t)(dot - p) : strlen(p);
        q[n++] = (uint8_t)l;
        memcpy(q + n, p, l);
        n += l;
        p += l;
        if (*p == '.') p++;
    }
    const uint8_t tail[5] = { 0, 0, 1, 0, 1 };              /* the root; A; IN */
    memcpy(q + n, tail, sizeof tail);
    n += sizeof tail;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_len = sizeof to, .sin_port = lwip_htons(53) };
    to.sin_addr.s_addr = server;
    const int64_t t0 = esp_timer_get_time();
    bool ok = false;
    if (sendto(sk, q, n, 0, (struct sockaddr *)&to, sizeof to) == (int)n) {
        uint8_t r[512];
        for (;;) {
            const int k = recv(sk, r, sizeof r, 0);
            if (k < 0) break;                                /* its time is up */
            if (k >= 12 && r[0] == q[0] && r[1] == q[1] && (r[2] & 0x80)) {
                ok = (r[3] & 0x0F) == 0;                     /* NOERROR */
                break;
            }
        }
    }
    *ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    close(sk);
    return ok;
}

/* The hosts from the list (net_prov's), into the targets: their names, and
 * the list seeded from the other firmwares' the first time. */
static void hosts_load(void)
{
    net_radio_t r[WT_HOSTS];
    int n = 0;
    for (int i = 0; i < net_prov_radio_count() && n < WT_HOSTS; i++)
        if (net_prov_radio_get(i, &r[n]) && r[n].host[0]) n++;
    if (!n && !net_prov_radios_stored()) {
        n = net_prov_hosts_elsewhere(r, WT_HOSTS);
        if (n) {
            for (int i = 0; i < n; i++) ESP_LOGI(TAG, "a host from another firmware: %s (%s)", r[i].host, r[i].name);
            net_prov_radios_save(r, n, 0);
        }
    }
    LOCK();
    for (int i = 0; i < WT_HOSTS; i++) {
        wt_ping_t *p = &s_w.p[WT_T_HOST + i];
        const char *h = i < n ? r[i].host : "";
        if (strcmp(p->name, h)) {
            memset(p, 0, sizeof *p);
            strlcpy(p->name, h, sizeof p->name);
            p->state = h[0] ? WT_P_FINDING : WT_P_OFF;
            s_t[WT_T_HOST + i].ip = 0;
        }
    }
    UNLOCK();
}

static void names_task(void *arg)
{
    (void)arg;
    bool dns_bad = false, host_bad[WT_HOSTS] = { 0 };
    for (;;) {
        if (atomic_exchange(&s_hosts_due, false)) hosts_load();
        if (!s_w.joined) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        /* The internet's name: what the network's DNS server answers, and how fast. */
        uint32_t server = 0, ms, ip;
        bool m;
        {
            esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_dns_info_t dn = { 0 };
            if (nif && esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &dn) == ESP_OK) server = dn.ip.u_addr.ip4.addr;
        }
        const bool ok = dns_probe(server, &ms);
        LOCK();
        if (ok) s_w.dns_ok++;
        else s_w.dns_fail++;
        s_w.dns_ms = (uint16_t)(ms > 0xFFFF ? 0xFFFF : ms);
        UNLOCK();
        if (!ok && !dns_bad) note(WT_EV_NAME, 0, 0, 0, "DNS did not answer for " DNS_NAME " (%u ms)", (unsigned)ms);
        dns_bad = !ok;
        /* The hosts' names. */
        for (int i = 0; i < WT_HOSTS; i++) {
            char name[40];
            LOCK();
            strlcpy(name, s_w.p[WT_T_HOST + i].name, sizeof name);
            UNLOCK();
            if (!name[0]) continue;
            const bool found = resolve(name, &ip, &m);
            char addr[16] = "";
            if (found) snprintf(addr, sizeof addr, IPSTR, IP2STR((esp_ip4_addr_t *)&ip));
            LOCK();
            wt_ping_t *p = &s_w.p[WT_T_HOST + i];
            if (strcmp(p->name, name)) {        /* changed meanwhile */
                UNLOCK();
                continue;
            }
            const bool had = p->addr[0] != 0;
            if (m) {
                if (found) s_w.mdns_ok++;
                else s_w.mdns_fail++;
            }
            if (found) {
                strlcpy(p->addr, addr, sizeof p->addr);
                p->state = WT_P_ON;
                s_t[WT_T_HOST + i].ip = ip;
            } else if (!p->addr[0]) {
                p->state = WT_P_NOTFOUND;     /* never found: nothing to ping */
            }
            UNLOCK();
            if (!found && !host_bad[i])
                note(WT_EV_NAME, 0, 0, 0, "%s%s not found%s", m ? "mDNS: " : "", name,
                     had ? ", its last address pinged" : "");
            if (found && host_bad[i]) note(WT_EV_NAME, 0, 0, 0, "%s found again: %s", name, addr);
            host_bad[i] = !found;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(NAMES_EVERY_S * 1000));
    }
}

/* ---- the channels round it ---- */

static void scan_start(void)
{
    wifi_scan_config_t sc = {
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time   = { .active = { .min = 40, .max = 100 } },
    };
    LOCK();
    s_w.scanning = true;
    UNLOCK();
    s_my_scan = true;
    s_scan_at = esp_timer_get_time();
    echoes_void(s_scan_at);
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
        s_my_scan = false;
        LOCK();
        s_w.scanning = false;
        UNLOCK();
        ESP_LOGW(TAG, "scan refused (another under way?)");
    }
}

static int ap_cmp(const void *a, const void *b)
{
    return ((const wt_ap_t *)b)->rssi - ((const wt_ap_t *)a)->rssi;
}

static void scan_read(void)
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 64) n = 64;
    wifi_ap_record_t *r = n ? heap_caps_calloc(n, sizeof *r, MALLOC_CAP_SPIRAM) : NULL;
    uint16_t got = n;
    if (r && esp_wifi_scan_get_ap_records(&got, r) != ESP_OK) got = 0;
    if (!r) esp_wifi_clear_ap_list();
    EXT_RAM_BSS_ATTR static wt_ap_t aps[64];
    uint8_t per[WT_CHANNELS + 1] = { 0 };
    char ours[33];
    LOCK();
    strlcpy(ours, s_w.ssid, sizeof ours);
    UNLOCK();
    int same = 0;
    for (int i = 0; i < got; i++) {
        wt_ap_t *a = &aps[i];
        memset(a, 0, sizeof *a);
        memcpy(a->ssid, r[i].ssid, 32);
        memcpy(a->bssid, r[i].bssid, 6);
        a->channel = r[i].primary;
        a->rssi    = r[i].rssi;
        a->auth    = (uint8_t)r[i].authmode;
        a->ours    = ours[0] && !strcmp(a->ssid, ours);
        same += a->ours;
        if (a->channel >= 1 && a->channel <= WT_CHANNELS && per[a->channel] < 255) per[a->channel]++;
    }
    free(r);
    qsort(aps, got, sizeof aps[0], ap_cmp);
    /* The strongest kept, and every one of the knob's own network's. */
    wt_ap_t keep[WT_APS];
    int k = 0;
    for (int i = 0; i < got && k < WT_APS; i++)
        if (aps[i].ours) keep[k++] = aps[i];
    for (int i = 0; i < got && k < WT_APS; i++)
        if (!aps[i].ours) keep[k++] = aps[i];
    qsort(keep, k, sizeof keep[0], ap_cmp);
    LOCK();
    memcpy(s_w.ap, keep, (size_t)k * sizeof keep[0]);
    s_w.n_ap = (uint8_t)k;
    s_w.n_heard = (uint8_t)got;
    memcpy(s_w.per_ch, per, sizeof per);
    s_w.scan_at_s = s_w.up_s ? s_w.up_s : 1;
    s_w.scanning = false;
    const uint8_t ch = s_w.channel;
    UNLOCK();
    s_my_scan = false;
    /* The silence the scan made is the scan's: each target's last answer
     * counts from its end. */
    echoes_void(esp_timer_get_time());
    int near = 0;
    for (int c = 1; c <= WT_CHANNELS; c++)
        if (ch && c - ch < 5 && ch - c < 5) near += per[c];
    note(WT_EV_SCAN, 0, 0, ch, "scan: %u networks, %d near ch %u, %d AP%s of its own", (unsigned)got, near,
         (unsigned)ch, same, same == 1 ? "" : "s");
    for (int i = 0; i < got && i < 12; i++) {
        char b[18];
        bssid_text(aps[i].bssid, b, sizeof b);
        ESP_LOGI(TAG, "  %c ch %2u %4d dBm %s %s", aps[i].ours ? '*' : ' ', (unsigned)aps[i].channel,
                 aps[i].rssi, b, aps[i].ssid[0] ? aps[i].ssid : "(hidden)");
    }
}

/* ---- the log ---- */

static void log_line(void)
{
    EXT_RAM_BSS_ATTR static wt_t w;
    wifitest_snapshot(&w);
    char why[48];
    static const char *const G[] = { "FINE", "FAIR", "POOR" };
    const int g = wt_verdict(&w, why, sizeof why);
    char line[200];
    int o = snprintf(line, sizeof line, "%s %d dBm ch %u | ", w.joined ? "on" : "OFF", w.rssi, (unsigned)w.channel);
    static const char *const ABBR[WT_TARGETS] = { "gw", "h1", "h2", "h3", "net" };
    for (int i = 0; i < WT_TARGETS && o < (int)sizeof line; i++) {
        const wt_ping_t *p = &w.p[i];
        if (p->state != WT_P_ON) continue;
        const unsigned pm = wt_loss_pm(p);
        o += snprintf(line + o, sizeof line - o, "%s %u/%u ms %u.%u%% | ", ABBR[i], wt_mean_ms(p),
                      (unsigned)p->max_ms, pm / 10, pm % 10);
    }
    if (o < (int)sizeof line)
        snprintf(line + o, sizeof line - o, "drops %u, %s: %s", (unsigned)w.drops, G[g], why);
    ESP_LOGI(TAG, "%s", line);
}

/* ---- the task ---- */

static void test_task(void *arg)
{
    (void)arg;
    s_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s_sock < 0) ESP_LOGE(TAG, "no raw socket for the pings: errno %d", errno);
    for (int t = 0; t < WT_TARGETS; t++) s_t[t].every = t == WT_T_NET ? PING_SLOW_US : PING_FAST_US;
    {
        struct in_addr a;
        inet_aton(NET_TARGET, &a);
        s_t[WT_T_NET].ip = a.s_addr;
    }
    LOCK();
    strlcpy(s_w.p[WT_T_GW].name, "router", sizeof s_w.p[WT_T_GW].name);
    strlcpy(s_w.p[WT_T_NET].name, "internet", sizeof s_w.p[WT_T_NET].name);
    strlcpy(s_w.p[WT_T_NET].addr, NET_TARGET, sizeof s_w.p[WT_T_NET].addr);
    s_w.p[WT_T_NET].state = WT_P_ON;
    UNLOCK();

    /* Joined already, as the knob is by the time radio_start() comes. */
    if (net_prov_is_connected()) atomic_store(&s_joined_now, true);
    note(WT_EV_START, 0, 0, 0, "counting began");

    int64_t next_s = esp_timer_get_time() + 1000000;
    uint32_t last_scan = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (atomic_exchange(&s_want_recount, false)) {
            LOCK();
            wt_reset(&s_w);
            UNLOCK();
            echoes_void(now);
            last_scan = 0;
            note(WT_EV_START, 0, 0, 0, "counting began again");
        }
        if (atomic_exchange(&s_void_due, false)) echoes_void(now);
        if (atomic_exchange(&s_joined_now, false)) {
            ap_read();
            ip_read();
            echoes_void(now);
            int8_t rssi = 0;
            uint8_t ch;
            char ssid[33], b[18];
            bool again;
            LOCK();
            again = s_w.drops > 0 || s_down_at;
            s_w.joined = true;
            s_w.join_s = 0;
            ch = s_w.channel;
            strlcpy(ssid, s_w.ssid, sizeof ssid);
            bssid_text(s_w.bssid, b, sizeof b);
            UNLOCK();
            int r;
            if (esp_wifi_sta_get_rssi(&r) == ESP_OK) rssi = (int8_t)r;
            if (again && s_down_at)
                note(WT_EV_JOINED, 0, rssi, ch, "joined again after %u.%u s: ch %u, %d dBm",
                     (unsigned)((now - s_down_at) / 1000000), (unsigned)((now - s_down_at) / 100000 % 10),
                     (unsigned)ch, rssi);
            else note(WT_EV_JOINED, 0, rssi, ch, "joined \"%.24s\": ch %u, %d dBm", ssid, (unsigned)ch, rssi);
            ESP_LOGI(TAG, "  access point %s; %s, %s", b, s_w.phy, s_w.auth);
            s_down_at = 0;
            xTaskNotifyGive(s_names);      /* the names looked up again at once */
        }
        if (atomic_exchange(&s_scan_done, false)) scan_read();
        if (s_my_scan && now - s_scan_at > SCAN_GIVE_UP_US) {
            s_my_scan = false;
            LOCK();
            s_w.scanning = false;
            UNLOCK();
            echoes_void(now);
            ESP_LOGW(TAG, "the scan never ended: the pings go on");
        }

        bool joined, scanning;
        LOCK();
        joined = s_w.joined;
        scanning = s_w.scanning;
        UNLOCK();

        if (s_sock >= 0 && joined && !scanning) {
            for (int t = 0; t < WT_TARGETS; t++) {
                if (!s_t[t].ip || now < s_t[t].next) continue;
                /* On a pace of its own: none bunched after a pause. */
                s_t[t].next = now - s_t[t].next < s_t[t].every ? s_t[t].next + s_t[t].every : now + s_t[t].every;
                echo_send(t, now);
            }
        }
        if (s_sock >= 0) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s_sock, &rd);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
            if (select(s_sock + 1, &rd, NULL, NULL, &tv) > 0) echoes_read(esp_timer_get_time());
            now = esp_timer_get_time();
            echoes_expire(now);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
            now = esp_timer_get_time();
        }

        if (now < next_s) continue;
        next_s += 1000000;
        if (now - next_s > 2000000) next_s = now + 1000000;    /* held up: no seconds made up */
        int r = 0;
        if (joined && esp_wifi_sta_get_rssi(&r) != ESP_OK) r = 0;
        if (r < -127 || r > 0) r = 0;
        if (joined && r) s_drop_rssi = (int8_t)r;
        if (joined) ip_read();
        uint32_t up;
        LOCK();
        wt_second(&s_w, joined ? (int8_t)r : 0);
        up = s_w.up_s;
        UNLOCK();
        if (joined && up % 10 == 0) ap_read();                  /* its channel or width may change */
        if (up % LOG_EVERY_S == 0) log_line();
        if (joined && !scanning && (atomic_exchange(&s_want_scan, false) ||
                                    (!last_scan && up >= SCAN_FIRST_S) ||
                                    (last_scan && up - last_scan >= SCAN_EVERY_S))) {
            last_scan = up;
            scan_start();
        }
    }
}

static void start_test(void)
{
    if (s_task) return;
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, NULL, NULL));
    /* Core 1, as the knob's other network helpers: core 0 is WiFi's. */
    xTaskCreatePinnedToCore(names_task, "wtnames", 6144, NULL, 2, &s_names, 1);
    xTaskCreatePinnedToCore(test_task, "wifitest", 5120, NULL, 4, &s_task, 1);
    ESP_LOGI(TAG, "measuring: the router and the hosts 5 times a second, " NET_TARGET " once");
}

/* ---- for the face and the page ---- */

void wifitest_snapshot(wt_t *out)
{
    if (!s_lock) {                      /* not begun yet */
        memset(out, 0, sizeof *out);
        return;
    }
    LOCK();
    *out = s_w;
    UNLOCK();
}

void wifitest_scan(void) { atomic_store(&s_want_scan, true); }
void wifitest_recount(void) { atomic_store(&s_want_recount, true); }

void wifitest_hosts_changed(void)
{
    atomic_store(&s_hosts_due, true);
    if (s_names) xTaskNotifyGive(s_names);
}

/* ---- the report ---- */

static size_t add(char *b, size_t n, size_t o, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static size_t add(char *b, size_t n, size_t o, const char *fmt, ...)
{
    if (o >= n) return o;
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(b + o, n - o, fmt, ap);
    va_end(ap);
    return k < 0 ? o : o + (size_t)k;
}

static void dur(char *s, size_t n, uint32_t sec)
{
    if (sec >= 3600) snprintf(s, n, "%uh %02um", (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60));
    else if (sec >= 60) snprintf(s, n, "%um %02us", (unsigned)(sec / 60), (unsigned)(sec % 60));
    else snprintf(s, n, "%us", (unsigned)sec);
}

size_t wifitest_report(char *b, size_t n)
{
    wt_t *w = heap_caps_malloc(sizeof *w, MALLOC_CAP_SPIRAM);
    if (!w) return (size_t)snprintf(b, n, "no memory for the report\n");
    wifitest_snapshot(w);
    char why[48], t1[16], t2[16], bs[18];
    static const char *const G[] = { "FINE", "FAIR", "POOR" };
    const int g = wt_verdict(w, why, sizeof why);
    size_t o = 0;
    const esp_app_desc_t *d = esp_app_get_description();
    dur(t1, sizeof t1, w->up_s);
    o = add(b, n, o, "VFO-Knob WiFi Test %s -- measured for %s\n", d->version, t1);
    o = add(b, n, o, "verdict: %s, %s\n\n", G[g], why);
    bssid_text(w->bssid, bs, sizeof bs);
    o = add(b, n, o, "network: \"%s\" %s, access point %s\n", w->ssid, w->joined ? "joined" : "NOT JOINED", bs);
    o = add(b, n, o, "  channel %u%s, %s, %s%s%s\n", (unsigned)w->channel,
            w->ht40 == 1 ? " (40 MHz, +)" : w->ht40 == 2 ? " (40 MHz, -)" : " (20 MHz)", w->phy, w->auth,
            w->country[0] ? ", country " : "", w->country);
    o = add(b, n, o, "  address %s, router %s, DNS %s\n", w->ip, w->gw, w->dns);
    const int rr = wt_rssi_recent(w);
    o = add(b, n, o, "signal: %d dBm now (%s), last minute %d, lowest %d, highest %d, mean %d\n", w->rssi,
            wt_signal_word(w->rssi), rr, w->rssi_min, w->rssi_max,
            w->rssi_n ? (int)((w->rssi_sum - (int32_t)w->rssi_n / 2) / (int32_t)w->rssi_n) : 0);
    dur(t1, sizeof t1, w->join_s);
    dur(t2, sizeof t2, w->down_s);
    o = add(b, n, o, "drops: %u, through another AP %u, beacons missed %u, address changed %u; "
                     "this join %s, off WiFi %s in all\n",
            (unsigned)w->drops, (unsigned)w->roams, (unsigned)w->beacons, (unsigned)w->ip_changes, t1, t2);
    if (w->drops) {
        o = add(b, n, o, "  the last drop: %s (reason %u)\n", wt_reason_text(w->last_reason),
                (unsigned)w->last_reason);
        const char *h = wt_reason_hint(w->last_reason);
        if (h[0]) o = add(b, n, o, "  %s\n", h);
    }
    o = add(b, n, o, "\npings (sent, lost, mean/min/max ms, late >%u ms, longest silence):\n", WT_LATE_MS);
    for (int i = 0; i < WT_TARGETS; i++) {
        const wt_ping_t *p = &w->p[i];
        if (p->state == WT_P_OFF) continue;
        const char *label = i == WT_T_GW ? "router" : i == WT_T_NET ? "internet" : "host";
        if (p->state == WT_P_FINDING || p->state == WT_P_NOTFOUND) {
            o = add(b, n, o, "  %-8s %s: %s\n", label, p->name, p->state == WT_P_FINDING ? "looking up" : "NOT FOUND");
            continue;
        }
        const unsigned pm = wt_loss_pm(p);
        char who[64];
        if (i == WT_T_GW || i == WT_T_NET) snprintf(who, sizeof who, "%s", p->addr);
        else snprintf(who, sizeof who, "%s (%s)", p->name, p->addr);
        o = add(b, n, o, "  %-8s %s: %u sent, %u.%u %% lost, %u/%u/%u ms, %u late, silent %u.%u s\n", label, who,
                (unsigned)p->sent, pm / 10, pm % 10, wt_mean_ms(p), (unsigned)p->min_ms, (unsigned)p->max_ms,
                (unsigned)p->late, (unsigned)(p->worst_gap_ms / 1000), (unsigned)(p->worst_gap_ms % 1000 / 100));
    }
    o = add(b, n, o, "names: DNS (" DNS_NAME ", asked of %s) %u answered, %u not, last %u ms; mDNS %u found, %u not\n",
            w->dns,
            (unsigned)w->dns_ok, (unsigned)w->dns_fail, (unsigned)w->dns_ms, (unsigned)w->mdns_ok,
            (unsigned)w->mdns_fail);
    if (w->scan_at_s) {
        dur(t1, sizeof t1, w->up_s - w->scan_at_s);
        o = add(b, n, o, "\nthe air, %s ago: %u networks heard; per channel:", t1, (unsigned)w->n_heard);
        for (int c = 1; c <= 13; c++) o = add(b, n, o, " %d:%u", c, (unsigned)w->per_ch[c]);
        o = add(b, n, o, "\n");
        for (int i = 0; i < w->n_ap; i++) {
            const wt_ap_t *a = &w->ap[i];
            bssid_text(a->bssid, bs, sizeof bs);
            o = add(b, n, o, "  %c ch %2u %4d dBm %s %-9s %s\n", a->ours ? '*' : ' ', (unsigned)a->channel, a->rssi,
                    bs, auth_text(a->auth), a->ssid[0] ? a->ssid : "(hidden)");
        }
    }
    o = add(b, n, o, "\nevents, newest first:\n");
    for (int i = 0; i < w->n_ev; i++) {
        const wt_event_t *e = &w->ev[i];
        char when[24];
        if (e->unix_s) {
            struct tm tm;
            const time_t tt = (time_t)e->unix_s;
            localtime_r(&tt, &tm);
            strftime(when, sizeof when, "%d %b %H:%M:%S", &tm);
        } else {
            dur(when, sizeof when, e->at_s);
        }
        o = add(b, n, o, "  %-15s %s\n", when, e->what);
    }
    free(w);
    if (o >= n) o = n - 1;
    return o;
}
