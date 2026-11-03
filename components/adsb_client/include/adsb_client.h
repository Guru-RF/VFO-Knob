/* The ADS-B Radar firmware's client (VFO_RADIO=adsb): no radio, but a feed
 * of the aircraft round the radar's middle, asked every few seconds (the
 * page's Refresh), and adsbdb for a flight's route. Its own task, on core 1,
 * one request at a time; app_main hands what comes to the face (ui.c
 * ADSB_FACE). radio.h answers as the setup firmware's does, but for the
 * link: READY while the feed answers. */
#ifndef ADSB_CLIENT_H
#define ADSB_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "adsb.h"

#define ADSB_KEEP 300            /* aircraft kept from a feed, nearest first */

typedef struct {
    uint32_t gen;                /* one more each time aircraft come */
    int      n, total;           /* kept, and in the feed */
    int64_t  at_us;              /* when they came */
} adsb_snap_t;

/* The newest aircraft, if their gen is not `gen`: up to cap into out, and
 * *snap; false when there is nothing new. */
bool adsb_client_take(adsb_ac_t *out, int cap, adsb_snap_t *snap, uint32_t gen);

/* Ask the feed now, not at the next refresh: the range or the middle moved. */
void adsb_client_poke(void);

/* A flight's route: looked up the first time, then from what is kept. True
 * once known, into from and to; *pending while it is being asked. */
bool adsb_client_route(const char *callsign, char *from, size_t fn, char *to, size_t tn, bool *pending);

/* The feed's state in a few words, for the face's foot: its name while it
 * answers ("airplanes.live"), else what went wrong ("no answer", "HTTP 429"). */
void adsb_client_feed(char *out, size_t n, bool *ok);

/* The page saved the settings (adsb_web.c): the feed asked anew, and once,
 * true from take_settings -- for app_main to hand them to the face. */
void adsb_client_settings_saved(void);
bool adsb_client_take_settings(void);

/* The map's tables, in the image (tools/adsb_tables.py). */
void adsb_client_tables(const uint8_t **map, size_t *mn, const uint8_t **apt, size_t *an);

#endif /* ADSB_CLIENT_H */
