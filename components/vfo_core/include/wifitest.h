/* The WiFi Test firmware's arithmetic (VFO_RADIO=wifitest): what the knob
 * measures of the network it is on -- the signal, every drop and why, the
 * answers to pings, the channels round it -- kept as counts and as the last
 * minute second by second, and what it all comes to in a few words. Plain
 * C, so test/host checks it on the PC (test_wifitest); the measuring itself
 * is components/wifitest_client's, the face ui.c's. */
#ifndef WIFITEST_H
#define WIFITEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WT_HIST     60          /* the last minute, a second a slot, oldest first */
#define WT_HOSTS    3           /* the radios and computers pinged beside the router */
#define WT_TARGETS  (2 + WT_HOSTS)
#define WT_EVENTS   12          /* the newest kept, newest first */
#define WT_APS      20          /* the networks heard at the last scan, strongest first */
#define WT_CHANNELS 14          /* 2.4 GHz: 1-13, and Japan's 14 */
#define WT_LATE_MS  100         /* an answer later than this would be heard in a radio's audio */
#define WT_LOST     0xFFFF      /* a second's slot: nothing came back */

/* The targets: the router first, the hosts, the internet last. */
enum { WT_T_GW = 0, WT_T_HOST = 1, WT_T_NET = WT_TARGETS - 1 };

/* A target's state. */
enum {
    WT_P_OFF,                   /* nothing there to ping */
    WT_P_FINDING,               /* its name not yet found */
    WT_P_NOTFOUND,              /* its name found nowhere */
    WT_P_ON,                    /* pinged */
};

typedef struct {
    char     name[40];          /* as said: "router", "IC-705.local", "internet" */
    char     addr[16];          /* its IPv4 address, "" while there is none */
    uint8_t  state;             /* WT_P_* */
    uint32_t sent, got;         /* since counting began */
    uint32_t late;              /* answers after WT_LATE_MS */
    uint32_t sum_ms;            /* the answers' times, summed: the mean */
    uint16_t min_ms, max_ms;
    uint16_t last_ms;           /* the last answer's, WT_LOST the last asked went unanswered */
    uint32_t gap_ms;            /* how long it has said nothing, now */
    uint32_t worst_gap_ms;      /* ...and the longest it ever did, joined */
    uint16_t cur;               /* this second's worst so far: 0 none answered */
    uint8_t  cur_sent, cur_lost;
    uint16_t hist[WT_HIST];     /* each second's worst answer, WT_LOST all lost, 0 none asked */
} wt_ping_t;

/* What happened, in order. */
enum {
    WT_EV_START,                /* counting began */
    WT_EV_JOINED,               /* the network joined, an address got */
    WT_EV_DROPPED,              /* the network lost, the access point's reason with it */
    WT_EV_ROAMED,               /* joined again through another access point */
    WT_EV_BEACONS,              /* the access point's beacons missed */
    WT_EV_ADDRESS,              /* a new address, or none */
    WT_EV_SILENT,               /* the router quiet over a second, joined all the while */
    WT_EV_NAME,                 /* a name not found: DNS or mDNS */
    WT_EV_SCAN,                 /* the channels scanned */
};

typedef struct {
    uint32_t at_s;              /* the counting's second */
    int64_t  unix_s;            /* the time, 0 while the knob has none */
    uint8_t  kind;              /* WT_EV_* */
    uint8_t  reason;            /* a drop's, as the WiFi driver gives it */
    int8_t   rssi;              /* dBm then, 0 unknown */
    uint8_t  channel;
    char     what[56];          /* in words */
} wt_event_t;

typedef struct {
    char    ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t  rssi;
    uint8_t auth;               /* wifi_auth_mode_t */
    bool    ours;               /* the network the knob is on, this or another of its access points */
} wt_ap_t;

typedef struct {
    uint32_t up_s;              /* seconds since counting began */
    bool     joined;            /* on the network, with an address */
    char     ssid[33];
    uint8_t  bssid[6];
    uint8_t  channel;
    uint8_t  ht40;              /* 0 20 MHz wide; 1 40, the second channel above; 2 below */
    char     phy[8];            /* "11n", "11g", "11b" */
    char     auth[16];          /* "WPA2", "WPA2/WPA3", ... */
    char     country[4];        /* the access point's, "" unsaid */
    int8_t   rssi;              /* dBm, now */
    int8_t   rssi_min, rssi_max;
    int32_t  rssi_sum;
    uint32_t rssi_n;
    int8_t   rssi_hist[WT_HIST]; /* each second's, 0 off the network */
    char     ip[16], gw[16], dns[16];
    uint32_t join_s;            /* how long this join has lasted */
    uint32_t down_s;            /* off the network, in all */
    uint16_t drops;             /* the network lost */
    uint16_t roams;             /* joined again through another access point */
    uint16_t beacons;           /* beacon timeouts */
    uint16_t ip_changes;
    uint16_t silences;          /* WT_EV_SILENT's */
    uint8_t  last_reason;       /* the last drop's */
    wt_ping_t p[WT_TARGETS];
    uint32_t dns_ok, dns_fail;  /* a name on the internet, looked up each minute */
    uint16_t dns_ms;            /* the last lookup's */
    uint32_t mdns_ok, mdns_fail; /* the hosts' .local names, the same */
    wt_event_t ev[WT_EVENTS];
    uint8_t  n_ev;
    wt_ap_t  ap[WT_APS];
    uint8_t  n_ap;              /* kept */
    uint8_t  n_heard;           /* heard, kept or not */
    uint8_t  per_ch[WT_CHANNELS + 1]; /* networks heard on each channel, [1]-[14] */
    uint32_t scan_at_s;         /* the last scan's second, 0 none yet */
    bool     scanning;
} wt_t;

/* Counting from nothing, the targets' names and addresses kept. */
void wt_reset(wt_t *w);

/* A target's pings: one sent, one answered after rtt_ms, one never answered
 * (after its time was up). The second's worst goes into the minute at
 * wt_second(). since_ms: how long since the target's last answer, for the
 * longest silence. */
void wt_ping_sent(wt_ping_t *p);
void wt_ping_answer(wt_ping_t *p, uint32_t rtt_ms, uint32_t since_ms);
void wt_ping_lost(wt_ping_t *p, uint32_t since_ms);

/* A second gone by: the minute moves on a slot -- the signal's and each
 * target's -- and the counts of time with it. rssi: now, 0 off the network. */
void wt_second(wt_t *w, int8_t rssi);

/* An event, newest first, the oldest let go once there are WT_EVENTS. */
void wt_event(wt_t *w, const wt_event_t *e);

/* A target's answers lost, per mille of those sent; its mean answer, ms. */
unsigned wt_loss_pm(const wt_ping_t *p);
unsigned wt_mean_ms(const wt_ping_t *p);
/* The signal's mean over the last minute, dBm; 0 off the network all that minute. */
int wt_rssi_recent(const wt_t *w);

/* The WiFi driver's reason for a drop, in a few words ("beacons lost",
 * "sent away for idling"), and what it tends to mean, a sentence. */
const char *wt_reason_text(uint8_t reason);
const char *wt_reason_hint(uint8_t reason);
/* The signal in a word: excellent, good, fair, weak, poor. */
const char *wt_signal_word(int rssi);

/* What it all comes to: WT_FINE, WT_FAIR or WT_POOR, and why, in a few words
 * for the face's foot -- the worst thing first. */
enum { WT_FINE, WT_FAIR, WT_POOR };
int wt_verdict(const wt_t *w, char *why, size_t n);

#endif /* WIFITEST_H */
