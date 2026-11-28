/* The AIS Radar firmware's client (VFO_RADIO=ais): no radio, but the ships
 * round the radar's middle -- from AISStream.io's stream, over a WebSocket
 * kept open (ais_ws.h), with the owner's own key; or from an AIS-catcher of
 * their own, its ship list asked every few seconds. Its own task, on core 0
 * beside WiFi as ADS-B Radar's; the ships kept by MMSI, each message laid
 * over what was known (components/vfo_core ais.c), and the nearest handed
 * to the face (ui.c AIS_FACE) every PUBLISH_MS. radio.h answers as ADS-B
 * Radar's does: READY while ships come. */
#ifndef AIS_CLIENT_H
#define AIS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ais.h"

#define AIS_KEEP 400             /* ships handed to the face, nearest first */

typedef struct {
    uint32_t gen;                /* one more each time ships are handed over */
    int      n, total;           /* handed over, and known */
    int64_t  at_us;
} ais_snap_t;

/* The newest ships, if their gen is not `gen`: up to cap into out, and
 * *snap; false when there is nothing new. */
bool ais_client_take(ais_ship_t *out, int cap, ais_snap_t *snap, uint32_t gen);

/* The range or the middle moved: the stream subscribed again for it (or
 * the AIS-catcher asked), and the ships handed over at once. */
void ais_client_poke(void);

/* The source's state in a few words, for the face's foot: its name while
 * ships come ("AISStream"), else what is wrong ("no key", "key refused",
 * "no answer"). */
void ais_client_feed(char *out, size_t n, bool *ok);

/* For the page: messages a minute, the last minute's; ships known. */
void ais_client_counts(uint32_t *per_min, int *known);

/* The page saved the settings (ais_web.c): the source asked anew, and once,
 * true from take_settings -- for app_main to hand them to the face. */
void ais_client_settings_saved(void);
bool ais_client_take_settings(void);

/* The map's tables, in the image: ADS-B Radar's coasts and borders
 * (tools/adsb_tables.py), the harbours (tools/ais_tables.py). */
void ais_client_tables(const uint8_t **map, size_t *mn, const uint8_t **ports, size_t *pn);

#endif /* AIS_CLIENT_H */
