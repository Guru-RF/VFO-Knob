/* The ADS-B Radar firmware's arithmetic (VFO_RADIO=adsb): the aircraft read
 * from a feed's JSON, where they are from the radar's middle, the ranges on
 * the dial, the colour of an altitude, what is shown and what is an alert,
 * a flight's route, and the map's coasts, borders and airports. Only
 * arithmetic and parsing, for the face, the client and the host tests
 * (test/host test_adsb).
 *
 * The feeds and their fields are those of Alex-developer's ADSB-Radar (MIT,
 * github.com/Alex-developer/ADSB-Radar, forked as Guru-RF/ADSB-Radar), read
 * here without a JSON tree: a feed at 250 NM is a megabyte of it, and a tree
 * of that would not fit in the knob's internal RAM. */
#ifndef VFO_ADSB_H
#define VFO_ADSB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- the aircraft ---------------------------------------------------------- */

#define ADSB_ALT_NONE  INT32_MIN    /* no altitude in the feed */
#define ADSB_VR_NONE   INT16_MIN    /* no vertical rate */

/* dbFlags, as the readsb-based feeds set them. */
#define ADSB_DB_MILITARY    0x01
#define ADSB_DB_INTERESTING 0x02

typedef struct {
    char    hex[7];          /* its ICAO address, as the feed writes it */
    char    flight[9];       /* its callsign, trimmed; "" when it sends none */
    char    reg[9];          /* its registration ("r"), from the feed's database */
    char    type[5];         /* its ICAO type ("t"): A320, B77W, C17 */
    char    squawk[5];
    char    emerg[12];       /* the feed's "emergency", "" for none */
    bool    ground;          /* on the ground: alt_baro "ground" */
    uint8_t db_flags;
    int32_t alt_ft;          /* barometric, else geometric; ADSB_ALT_NONE */
    int16_t gs_kt;           /* ground speed; -1 unknown */
    int16_t track;           /* degrees true, 0-359; -1 unknown */
    int16_t vrate_fpm;       /* ADSB_VR_NONE unknown */
    float   seen_s;          /* since its position last came */
    double  lat, lon;
    float   dist_nm, brg;    /* from the radar's middle: great circle, degrees true */
} adsb_ac_t;

/* A feed's aircraft: readsb's aircraft.json ("aircraft") or the internet
 * feeds' ("ac"), each with a position kept, up to `cap`, nearest first,
 * within max_nm of the middle (lat0, lon0). Returns how many were kept, -1
 * for a document that is no feed; *total, if asked, counts every aircraft
 * in it, positions or not. js need not end in a NUL: len bounds it. */
int adsb_parse(const char *js, size_t len, double lat0, double lon0, float max_nm,
               adsb_ac_t *out, int cap, int *total);

/* The feeds: three on the internet, free and without a key, and a receiver
 * of one's own -- readsb, dump1090 or tar1090's aircraft.json. adsb.fi is
 * the default: airplanes.live answered 403 to everyone by 2026-11, until
 * its people have been written to. The numbers are kept on the card. */
enum { ADSB_SRC_AIRPLANES_LIVE, ADSB_SRC_ADSB_LOL, ADSB_SRC_ADSB_FI, ADSB_SRC_LOCAL, ADSB_SRCS };
bool        adsb_src_ok(long s);
const char *adsb_src_name(uint8_t src);       /* "airplanes.live", ..., "your receiver" */
/* The address to ask for the aircraft within `nm` of (lat, lon); a local
 * receiver's is the page's, http:// put in front when it has no scheme and
 * /data/aircraft.json after when it has no path. False when there is none
 * to ask (a local feed without an address) or it does not fit. */
bool        adsb_feed_url(uint8_t src, const char *local, double lat, double lon, float nm,
                          char *out, size_t n);

/* Great-circle distance in NM and initial bearing, degrees true 0-360. */
void adsb_dist_brg(double lat0, double lon0, double lat, double lon, float *nm, float *brg);

/* --- the ranges -------------------------------------------------------------
 * Ten a unit, round numbers in it, the dial a range a detent; 250 NM at the
 * most, the internet feeds' own limit. */
#define ADSB_RANGES 10
enum { ADSB_NM, ADSB_KM, ADSB_MI, ADSB_UNITS };

bool        adsb_units_ok(long u);
const char *adsb_unit_name(uint8_t units);                 /* "NM", "km", "mi" */
uint16_t    adsb_range_val(uint8_t units, uint8_t i);      /* in the unit */
float       adsb_range_nm(uint8_t units, uint8_t i);
float       adsb_nm_in(uint8_t units, float nm);           /* NM in the unit */
/* `detents` on from i, held at either end. */
uint8_t     adsb_range_step(uint8_t i, int32_t detents);

/* --- colour, filter, alerts -------------------------------------------------
 * The altitude bands of ADSB-Radar's colours: on the ground (or no
 * altitude), under 2000 ft, 10000, 20000, 30000, 40000, and above. */
enum { ADSB_BAND_GROUND, ADSB_BAND_2K, ADSB_BAND_10K, ADSB_BAND_20K, ADSB_BAND_30K,
       ADSB_BAND_40K, ADSB_BAND_HIGH, ADSB_BANDS };
uint8_t adsb_band(const adsb_ac_t *a);

/* What the page's Show offers: every aircraft, military ones, or those the
 * feed's database calls interesting. */
enum { ADSB_SHOW_ALL, ADSB_SHOW_MILITARY, ADSB_SHOW_INTERESTING, ADSB_SHOWS };
bool adsb_show_ok(long s);

/* An aircraft's alert: an emergency (a squawk of 7500, 7600 or 7700, or the
 * feed's emergency), or a type on the page's list. */
enum { ADSB_AL_NONE, ADSB_AL_TYPE, ADSB_AL_EMERGENCY };
uint8_t     adsb_alert(const adsb_ac_t *a, const char *types);
/* The emergency in words, for the banner: "EMERGENCY", "HIJACK", "NO RADIO",
 * "MEDICAL", "MINIMUM FUEL", "DOWNED"; "" for none. */
const char *adsb_emergency_name(const adsb_ac_t *a);
/* Whether type is on the list: types by comma or space, any case, a
 * trailing * for every type starting so ("B74*"). */
bool        adsb_type_listed(const char *types, const char *type);

/* Shown on the radar: the filter's, and on the ground only when asked.
 * An emergency always is. */
bool adsb_shown(const adsb_ac_t *a, uint8_t show, bool ground, const char *types);

/* --- a flight's route -------------------------------------------------------
 * adsbdb's answer for a callsign (api.adsbdb.com/v0/callsign/<cs>): where
 * from and where to, each its town ("municipality"), else its IATA code,
 * else ICAO. False for an unknown callsign. */
bool adsb_route_parse(const char *js, size_t len, char *from, size_t fn, char *to, size_t tn);

/* --- the map ------------------------------------------------------------------
 * tools/adsb_tables.py's tables, as they are in the image: map.bin's runs
 * of points (coasts and borders), airports.bin's airports. Positions in
 * 1e-4 degree. */
typedef struct {
    double lat0, lon0;      /* the middle */
    double px_nm;           /* pixels a nautical mile */
    double coslat;
} adsb_view_t;

/* The view for a range across `radius_px` round the middle. */
void adsb_view_set(adsb_view_t *v, double lat0, double lon0, float range_nm, float radius_px);
/* A point, in pixels from the middle: x east, y south (the glass's down). */
void adsb_project(const adsb_view_t *v, double lat, double lon, float *x, float *y);

enum { ADSB_RUN_COAST, ADSB_RUN_BORDER };
typedef struct {
    int32_t        lat, lon;     /* its first point */
    uint16_t       steps;        /* points after it */
    uint8_t        kind;
    const uint8_t *step;         /* int16 dlat, dlon each, little-endian */
} adsb_run_t;

/* Whether map.bin holds what it should; its runs, and run i if it lies
 * within the view's square of half-side `half_nm` round the middle. */
bool   adsb_map_ok(const uint8_t *p, size_t n);
size_t adsb_map_runs(const uint8_t *p);
bool   adsb_map_run(const uint8_t *p, size_t i, const adsb_view_t *v, float half_nm, adsb_run_t *out);
/* Step k of a run, 0 the one from its first point to its second. */
void   adsb_run_step(const adsb_run_t *r, uint16_t k, int32_t *dlat, int32_t *dlon);

typedef struct {
    int32_t lat, lon;
    char    code[5];
    bool    large;
} adsb_apt_t;

bool   adsb_apt_ok(const uint8_t *p, size_t n);
size_t adsb_apt_count(const uint8_t *p);
void   adsb_apt_get(const uint8_t *p, size_t i, adsb_apt_t *out);
/* The first airport at or north of lat (1e-4 degree): they are sorted so. */
size_t adsb_apt_from(const uint8_t *p, int32_t lat);
/* The airport by its ICAO code, any case: its index, or -1. */
long   adsb_apt_find(const uint8_t *p, const char *code);

#endif /* VFO_ADSB_H */
