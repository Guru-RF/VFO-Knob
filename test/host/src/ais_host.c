/* AIS Radar's stream on the PC: components/ais_client/ais_ws.c -- the
 * connection, its TLS (kiwi_tls on ESP-IDF's mbedTLS), the upgrade with
 * permessage-deflate, the messages whole and inflated -- and vfo_core ais.c
 * reading them, against tools/mock_aisstream.py or AISStream.io itself.
 *
 *   ais_host URL SECONDS [--no-deflate] [--resub]
 *
 * The key from $AIS_KEY ("test-key", the mock's, without it), never printed;
 * the middle from $AIS_LAT and $AIS_LON (Westende without), the range
 * $AIS_NM (12). Says what came: the confirmation, the messages, the ships by
 * MMSI and kind, the bytes on the wire and inflated, the nearest few. Exit
 * 0 when the subscription was confirmed and a ship with a position came, 2
 * for an error from the stream, 1 for anything else. --resub subscribes
 * again halfway with half the range, as the dial does. */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ais.h"
#include "adsb.h"
#include "ais_ws.h"
#include "esp_timer.h"

#define KEEP 2000

static ais_ship_t s_ships[KEEP];
static int s_n;

static ais_ship_t *find(uint32_t mmsi)
{
    for (int i = 0; i < s_n; i++)
        if (s_ships[i].mmsi == mmsi) return &s_ships[i];
    if (s_n == KEEP) return NULL;
    ais_ship_init(&s_ships[s_n], mmsi);
    return &s_ships[s_n++];
}

static int by_dist(const void *a, const void *b)
{
    const ais_ship_t *l = a, *r = b;
    if (l->pos != r->pos) return l->pos ? -1 : 1;
    return (l->dist_nm > r->dist_nm) - (l->dist_nm < r->dist_nm);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s URL SECONDS [--no-deflate] [--resub]\n", argv[0]);
        return 1;
    }
    bool deflate = true, resub = false;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--no-deflate")) deflate = false;
        if (!strcmp(argv[i], "--resub")) resub = true;
    }
    signal(SIGPIPE, SIG_IGN);              /* TLS writes to a closed far end: an error, not the end */
    const char *key = getenv("AIS_KEY");
    if (!key || !*key) key = "test-key";
    const double lat = getenv("AIS_LAT") ? atof(getenv("AIS_LAT")) : 51.160;
    const double lon = getenv("AIS_LON") ? atof(getenv("AIS_LON")) : 2.760;
    const float nm = getenv("AIS_NM") ? (float)atof(getenv("AIS_NM")) : 12.0f;
    const int secs = atoi(argv[2]);

    char why[96];
    const int64_t t0 = esp_timer_get_time();
    ais_ws_t *w = ais_ws_open(argv[1], deflate, "VFO-Knob-AIS/test (+github.com/Guru-RF/VFO-Knob)", NULL, NULL,
                              why, sizeof why);
    if (!w) {
        printf("open: %s\n", why);
        return 1;
    }
    printf("open in %d ms: %s\n", (int)((esp_timer_get_time() - t0) / 1000),
           ais_ws_deflated(w) ? "deflate" : "uncompressed");
    char sub[1024];
    if (!ais_stream_sub(key, lat, lon, nm * 1.2f, sub, sizeof sub) || !ais_ws_send_text(w, sub, strlen(sub))) {
        printf("subscription not sent\n");
        ais_ws_close(w);
        return 1;
    }
    bool confirmed = false, resubbed = false;
    int msgs = 0, ships = 0, other = 0, bad = 0, err = 0;
    int64_t last_say = esp_timer_get_time(), last_ping = last_say;
    const int64_t end = esp_timer_get_time() + (int64_t)secs * 1000000;
    while (esp_timer_get_time() < end) {
        const char *m;
        size_t n;
        const int r = ais_ws_recv(w, 200, &m, &n, why, sizeof why);
        if (r == AIS_WS_CLOSED || r == AIS_WS_FAILED) {
            printf("%s: %s\n", r == AIS_WS_CLOSED ? "closed" : "failed", why);
            break;
        }
        const int64_t now = esp_timer_get_time();
        if (now - last_ping > 10000000) {
            ais_ws_ping(w);
            last_ping = now;
        }
        if (resub && !resubbed && now > end - (int64_t)secs * 500000) {
            resubbed = true;
            ais_stream_sub(key, lat, lon, nm * 0.6f, sub, sizeof sub);
            printf("subscribed again: %s\n", ais_ws_send_text(w, sub, strlen(sub)) ? "sent" : "not sent");
        }
        if (now - last_say > 5000000) {
            last_say = now;
            uint64_t wire, plain;
            uint32_t dropped;
            ais_ws_counts(w, &wire, &plain, &dropped);
            printf("%3ds: %d messages, %d ships known, %llu bytes on the wire, %llu inflated, %u dropped\n",
                   (int)((now - t0) / 1000000), msgs, s_n, (unsigned long long)wire, (unsigned long long)plain,
                   (unsigned)dropped);
            fflush(stdout);
        }
        if (r != AIS_WS_MSG) continue;
        msgs++;
        ais_ship_t u;
        uint32_t f;
        bool comp = false;
        const int k = ais_stream_parse(m, n, &u, &f, &comp, why, sizeof why);
        if (k == AIS_MSG_CONFIRM) {
            confirmed = true;
            printf("confirmed: compression %s\n", comp ? "on" : "off");
        } else if (k == AIS_MSG_ERROR) {
            printf("error: %s\n", why);
            err = 1;
            break;
        } else if (k == AIS_MSG_SHIP) {
            ships++;
            ais_ship_t *s = find(u.mmsi);
            if (s) ais_merge(s, &u, f);
        } else if (k == AIS_MSG_OTHER) {
            other++;
        } else {
            if (bad++ < 3) printf("not a message: %.120s\n", m);
        }
    }
    uint64_t wire, plain;
    uint32_t dropped;
    ais_ws_counts(w, &wire, &plain, &dropped);
    ais_ws_close(w);

    int with_pos = 0, kinds[AIS_KINDS] = { 0 };
    for (int i = 0; i < s_n; i++) {
        ais_ship_t *s = &s_ships[i];
        if (s->pos) {
            with_pos++;
            adsb_dist_brg(lat, lon, s->lat, s->lon, &s->dist_nm, &s->brg);
        }
        kinds[ais_kind(s)]++;
    }
    qsort(s_ships, (size_t)s_n, sizeof s_ships[0], by_dist);
    printf("in all: %d messages (%d of ships, %d other, %d not read), %d ships, %d with a position; "
           "%llu bytes on the wire, %llu inflated (%.1fx), %u dropped\n",
           msgs, ships, other, bad, s_n, with_pos, (unsigned long long)wire, (unsigned long long)plain,
           wire ? (double)plain / (double)wire : 0.0, (unsigned)dropped);
    static const char *const K[AIS_KINDS] = { "other", "cargo", "tanker", "passenger", "high-speed", "special",
                                              "fishing", "pleasure", "aid", "base", "SAR aircraft", "emergency" };
    printf("kinds:");
    for (int i = 0; i < AIS_KINDS; i++) if (kinds[i]) printf(" %s %d", K[i], kinds[i]);
    printf("\n");
    for (int i = 0; i < s_n && i < 12; i++) {
        const ais_ship_t *s = &s_ships[i];
        if (!s->pos) break;
        printf("  %09u %-20s %-18s %5.1f kn %3d deg %-16s %5.1f NM %03.0f\n", (unsigned)s->mmsi,
               s->name[0] ? s->name : "-", ais_type_name(s)[0] ? ais_type_name(s) : "-",
               s->sog_dk >= 0 ? s->sog_dk / 10.0 : -1.0, ais_course(s), ais_nav_name(s->nav), (double)s->dist_nm,
               (double)s->brg);
    }
    if (err) return 2;
    return confirmed && with_pos ? 0 : 1;
}
