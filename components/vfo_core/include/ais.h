/* The AIS Radar firmware's arithmetic (VFO_RADIO=ais): the ships read from
 * AISStream.io's messages or an AIS-catcher's ship list, what kind each is
 * and its colour, what is an alert, the ranges on the dial, AISStream's
 * subscription, and the harbours on the map. Only arithmetic and parsing,
 * for the face, the client and the host tests (test/host test_ais).
 *
 * Positions, distances, the view and the coasts are ADS-B Radar's (adsb.h):
 * the same radar, the same map. Read without a JSON tree, as adsb.c reads
 * its feeds (json_scan.h). */
#ifndef VFO_AIS_H
#define VFO_AIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- a ship ------------------------------------------------------------------ */

#define AIS_NAV_NONE 15             /* navigational status "not defined" */

/* What sent it, from its MMSI and its messages. */
enum { AIS_CLS_A, AIS_CLS_B, AIS_CLS_ATON, AIS_CLS_BASE, AIS_CLS_SAR_AIRCRAFT, AIS_CLS_SART, AIS_CLS_OTHER };

typedef struct {
    uint32_t mmsi;
    char     name[21];          /* trimmed, '@' padding cut; "" until known */
    char     callsign[8];
    char     dest[21];
    uint32_t imo;               /* 0: none */
    uint8_t  type;              /* AIS ship and cargo type, 0-99; 0 unknown */
    uint8_t  nav;               /* navigational status 0-15; AIS_NAV_NONE */
    uint8_t  cls;               /* AIS_CLS_* */
    int16_t  sog_dk;            /* speed over ground, 0.1 kn; -1 unknown */
    int16_t  cog_dd;            /* course over ground, 0.1 degree; -1 unknown */
    int16_t  hdg;               /* true heading, degrees; -1 unknown */
    uint16_t len_m, beam_m;     /* 0 unknown */
    uint8_t  draught_dm;        /* 0.1 m; 0 unknown */
    uint8_t  eta_mon, eta_day;  /* 0: none */
    uint8_t  eta_h, eta_m;      /* 24, 60: none */
    bool     pos;               /* lat and lon hold a position */
    double   lat, lon;
    float    seen_s;            /* since its position last came */
    float    dist_nm, brg;      /* from the radar's middle */
} ais_ship_t;

/* A ship as nothing has been heard of it yet. */
void ais_ship_init(ais_ship_t *s, uint32_t mmsi);

/* What a message carried, AIS_F_* bits: what an update holds of a ship. */
enum {
    AIS_F_POS = 1u << 0, AIS_F_SOG = 1u << 1, AIS_F_COG = 1u << 2, AIS_F_HDG = 1u << 3,
    AIS_F_NAV = 1u << 4, AIS_F_NAME = 1u << 5, AIS_F_CALL = 1u << 6, AIS_F_TYPE = 1u << 7,
    AIS_F_DIM = 1u << 8, AIS_F_DEST = 1u << 9, AIS_F_ETA = 1u << 10, AIS_F_IMO = 1u << 11,
    AIS_F_DRAUGHT = 1u << 12, AIS_F_CLS = 1u << 13,
};
/* What `u` holds (`fields`) laid over `dst`, the same MMSI's. */
void ais_merge(ais_ship_t *dst, const ais_ship_t *u, uint32_t fields);

/* --- AISStream.io --------------------------------------------------------------
 * One message of the stream: {"MessageType":..., "MetaData":{...},
 * "Message":{"<type>":{...}}}. AIS_MSG_SHIP and *u, *fields for a position
 * or a ship's static data; AIS_MSG_CONFIRM for the subscription's
 * confirmation (*compressed: whether the server compresses); AIS_MSG_ERROR
 * and why for {"error": ...}; AIS_MSG_OTHER for a message of no use here;
 * AIS_MSG_BAD for text that is no message. */
enum { AIS_MSG_BAD = -1, AIS_MSG_OTHER, AIS_MSG_SHIP, AIS_MSG_CONFIRM, AIS_MSG_ERROR };
int ais_stream_parse(const char *js, size_t len, ais_ship_t *u, uint32_t *fields, bool *compressed,
                     char *why, size_t wn);

/* The subscription for the ships within `nm` of (lat, lon): a box round
 * the radar's circle, a little wider, and the message types read here.
 * False when it does not fit. */
bool ais_stream_sub(const char *key, double lat, double lon, float nm, char *out, size_t n);

/* The box's corners: south-west and north-east, degrees. */
void ais_box(double lat, double lon, float nm, double *lat1, double *lon1, double *lat2, double *lon2);

/* --- AIS-catcher ----------------------------------------------------------------
 * Its web viewer's ship list (/api/ships.json, AIS-catcher -N <port>):
 * {"count":n,"ships":[{"mmsi":..,"lat":..,"lon":..,"speed":..,...}],...}.
 * Each with a position kept, up to `cap`, nearest first, within max_nm of
 * (lat0, lon0); -1 for a document that is no ship list. *total counts
 * every ship in it. */
int ais_catcher_parse(const char *js, size_t len, double lat0, double lon0, float max_nm,
                      ais_ship_t *out, int cap, int *total);

/* Its address as asked: http:// in front without a scheme, /api/ships.json
 * after without a path. False for none, or when it does not fit. */
bool ais_catcher_url(const char *addr, char *out, size_t n);

/* --- the sources ---------------------------------------------------------------- */
enum { AIS_SRC_AISSTREAM, AIS_SRC_CATCHER, AIS_SRCS };
bool        ais_src_ok(long s);
const char *ais_src_name(uint8_t src);       /* "AISStream", "your AIS-catcher" */

/* --- kinds and colours ------------------------------------------------------------
 * As the chart plotters and MarineTraffic colour them, by the AIS type; an
 * aid to navigation, a base station and a SAR aircraft by their MMSI. */
enum { AIS_K_OTHER, AIS_K_CARGO, AIS_K_TANKER, AIS_K_PASSENGER, AIS_K_HSC, AIS_K_SPECIAL,
       AIS_K_FISHING, AIS_K_PLEASURE, AIS_K_ATON, AIS_K_BASE, AIS_K_SAR_AIRCRAFT, AIS_K_EMERGENCY,
       AIS_KINDS };
uint8_t     ais_kind(const ais_ship_t *s);
/* The AIS type in words, for the card: "Cargo", "Tanker", "Tug", "Pilot
 * vessel", "Sailing", "Pleasure craft" ...; by the MMSI for those it says
 * what they are: "Aid to navigation", "Base station", "SAR aircraft",
 * "AIS-SART" ...; "" unknown. */
const char *ais_type_name(const ais_ship_t *s);
/* Its navigational status in words: "under way", "at anchor", "moored" ...;
 * "" for none. */
const char *ais_nav_name(uint8_t nav);
/* Whether it is under way: 0.5 kn or more over ground. */
bool        ais_moving(const ais_ship_t *s);
/* Its course for the arrow, degrees: over ground, else its heading; -1 none. */
int         ais_course(const ais_ship_t *s);

/* --- alerts ----------------------------------------------------------------------
 * An emergency: an AIS-SART (MMSI 970...), a man overboard device (972...),
 * an EPIRB (974...), or a navigational status of 14, "AIS-SART active". A
 * ship on the page's list: by its MMSI, or by its name. */
enum { AIS_AL_NONE, AIS_AL_WATCH, AIS_AL_EMERGENCY };
uint8_t     ais_alert(const ais_ship_t *s, const char *watch);
/* The emergency in words: "AIS-SART", "MAN OVERBOARD", "EPIRB"; "" none. */
const char *ais_emergency_name(const ais_ship_t *s);
/* Whether the ship is on the list: MMSIs and names by comma, any case; a
 * trailing * for every name starting so ("MSC*"). */
bool        ais_watched(const char *watch, const ais_ship_t *s);

/* What the page's Show offers: every ship and aid, ships alone (no aids,
 * no base stations), or those under way. An emergency always shows. */
enum { AIS_SHOW_ALL, AIS_SHOW_SHIPS, AIS_SHOW_MOVING, AIS_SHOWS };
bool ais_show_ok(long s);
bool ais_shown(const ais_ship_t *s, uint8_t show, const char *watch);

/* --- the ranges --------------------------------------------------------------------
 * Nine a unit (adsb.h's units), the dial a range a detent, 50 NM at the most:
 * a harbour's to a stretch of coast. */
#define AIS_RANGES 9
uint16_t ais_range_val(uint8_t units, uint8_t i);
float    ais_range_nm(uint8_t units, uint8_t i);
uint8_t  ais_range_step(uint8_t i, int32_t detents);

/* --- the harbours ------------------------------------------------------------------
 * tools/ais_tables.py's ports.bin: Natural Earth's ports, sorted by
 * latitude, positions in 1e-4 degree. */
typedef struct {
    int32_t lat, lon;
    char    name[20];
} ais_port_t;

bool   ais_port_ok(const uint8_t *p, size_t n);
size_t ais_port_count(const uint8_t *p);
void   ais_port_get(const uint8_t *p, size_t i, ais_port_t *out);
/* The first at or north of lat (1e-4 degree). */
size_t ais_port_from(const uint8_t *p, int32_t lat);

#endif /* VFO_AIS_H */
