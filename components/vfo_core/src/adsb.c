#include "adsb.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_D      3.14159265358979323846
#define EARTH_NM  3440.065           /* the earth's mean radius */

/* --- a JSON reader, no tree -------------------------------------------------
 * Walks the text once; each value is read where it is wanted, or stepped
 * over. Bounded by `end`, so the text need not end in a NUL. */
typedef struct { const char *p, *end; } js_t;

static void ws(js_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\n' || *s->p == '\r' || *s->p == '\t')) s->p++;
}

static char peek(js_t *s)
{
    ws(s);
    return s->p < s->end ? *s->p : 0;
}

static bool eat(js_t *s, char c)
{
    if (peek(s) != c) return false;
    s->p++;
    return true;
}

/* A string, into out (cap bytes, NUL-ended) or, out NULL, stepped over. An
 * escape keeps the character after the backslash; \uXXXX becomes '?'. */
static bool str(js_t *s, char *out, size_t cap)
{
    if (!eat(s, '"')) return false;
    size_t n = 0;
    while (s->p < s->end) {
        char c = *s->p++;
        if (c == '"') {
            if (out && cap) out[n] = 0;
            return true;
        }
        if (c == '\\') {
            if (s->p >= s->end) return false;
            c = *s->p++;
            if (c == 'u') {
                if (s->end - s->p < 4) return false;
                s->p += 4;
                c = '?';
            }
        }
        if (out && n + 1 < cap) out[n++] = c;
    }
    return false;
}

static bool num(js_t *s, double *v)
{
    ws(s);
    char b[32];
    size_t n = 0;
    while (s->p < s->end && n < sizeof b - 1 && *s->p && strchr("+-0123456789.eE", *s->p)) b[n++] = *s->p++;
    if (!n) return false;
    b[n] = 0;
    char *e;
    *v = strtod(b, &e);
    return e != b;
}

static bool skip(js_t *s, int depth)
{
    const char c = peek(s);
    if (c == '"') return str(s, NULL, 0);
    if (c == '{' || c == '[') {
        if (depth > 32) return false;
        const char close = c == '{' ? '}' : ']';
        s->p++;
        if (eat(s, close)) return true;
        for (;;) {
            if (c == '{' && (!str(s, NULL, 0) || !eat(s, ':'))) return false;
            if (!skip(s, depth + 1)) return false;
            if (eat(s, ',')) continue;
            return eat(s, close);
        }
    }
    const char *start = s->p;                /* a number, true, false, null */
    while (s->p < s->end && *s->p != ',' && *s->p != '}' && *s->p != ']' && *s->p != ' ' &&
           *s->p != '\n' && *s->p != '\r' && *s->p != '\t') s->p++;
    return s->p > start;
}

/* A text field: a string copied and its trailing blanks cut; anything else
 * (null) stepped over and the field left empty. */
static bool text(js_t *s, char *out, size_t cap)
{
    out[0] = 0;
    if (peek(s) != '"') return skip(s, 0);
    if (!str(s, out, cap)) return false;
    size_t n = strlen(out);
    while (n && out[n - 1] == ' ') out[--n] = 0;
    return true;
}

/* A number field: false only for broken text; *got says if it was one. */
static bool number(js_t *s, double *v, bool *got)
{
    const char c = peek(s);
    if (c == '-' || (c >= '0' && c <= '9')) return (*got = num(s, v));
    *got = false;
    return skip(s, 0);
}

/* --- the aircraft ------------------------------------------------------------ */

static double norm_deg(double d)
{
    d = fmod(d, 360.0);
    return d < 0 ? d + 360.0 : d;
}

void adsb_dist_brg(double lat0, double lon0, double lat, double lon, float *nm, float *brg)
{
    const double r = PI_D / 180.0;
    const double p1 = lat0 * r, p2 = lat * r, dp = (lat - lat0) * r, dl = (lon - lon0) * r;
    const double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    if (nm) *nm = (float)(EARTH_NM * 2 * atan2(sqrt(a), sqrt(1 - a)));
    if (brg) {
        const double y = sin(dl) * cos(p2);
        const double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
        *brg = (float)norm_deg(atan2(y, x) / r);
    }
}

/* One aircraft object; *pos says whether it had a position. */
static bool aircraft(js_t *s, adsb_ac_t *a, bool *pos)
{
    memset(a, 0, sizeof *a);
    a->alt_ft = ADSB_ALT_NONE;
    a->gs_kt = -1;
    a->track = -1;
    a->vrate_fpm = ADSB_VR_NONE;
    bool has_lat = false, has_lon = false, has_alt = false, has_seen_pos = false;
    double geom = 0, heading = -1, rate_baro = 0, rate_geom = 0;
    bool has_geom = false, has_track = false, has_rate_baro = false, has_rate_geom = false;
    *pos = false;
    if (!eat(s, '{')) return false;
    if (eat(s, '}')) return true;
    for (;;) {
        char k[16];
        if (!str(s, k, sizeof k) || !eat(s, ':')) return false;
        double v = 0;
        bool got = false, ok;
        if (!strcmp(k, "hex"))          ok = text(s, a->hex, sizeof a->hex);
        else if (!strcmp(k, "flight"))  ok = text(s, a->flight, sizeof a->flight);
        else if (!strcmp(k, "r"))       ok = text(s, a->reg, sizeof a->reg);
        else if (!strcmp(k, "t"))       ok = text(s, a->type, sizeof a->type);
        else if (!strcmp(k, "squawk"))  ok = text(s, a->squawk, sizeof a->squawk);
        else if (!strcmp(k, "emergency")) {
            ok = text(s, a->emerg, sizeof a->emerg);
            if (!strcmp(a->emerg, "none")) a->emerg[0] = 0;
        } else if (!strcmp(k, "alt_baro")) {
            if (peek(s) == '"') {
                char t[8];
                ok = str(s, t, sizeof t);
                if (!strcmp(t, "ground")) a->ground = true;
            } else {
                ok = number(s, &v, &got);
                if (got) { a->alt_ft = (int32_t)lround(v); has_alt = true; }
            }
        } else if (!strcmp(k, "alt_geom")) {
            ok = number(s, &geom, &has_geom);
        } else if (!strcmp(k, "gs")) {
            ok = number(s, &v, &got);
            if (got && v >= 0) a->gs_kt = (int16_t)lround(fmin(v, 9999));
        } else if (!strcmp(k, "track")) {
            ok = number(s, &v, &got);
            if (got) { heading = v; has_track = true; }
        } else if (!strcmp(k, "true_heading") || !strcmp(k, "mag_heading") || !strcmp(k, "nav_heading")) {
            ok = number(s, &v, &got);
            if (got && !has_track && heading < 0) heading = v;
        } else if (!strcmp(k, "baro_rate")) {
            ok = number(s, &rate_baro, &has_rate_baro);
        } else if (!strcmp(k, "geom_rate")) {
            ok = number(s, &rate_geom, &has_rate_geom);
        } else if (!strcmp(k, "dbFlags") || !strcmp(k, "dbflags")) {
            ok = number(s, &v, &got);
            if (got) a->db_flags = (uint8_t)fmax(0, fmin(255, v));
        } else if (!strcmp(k, "lat")) {
            ok = number(s, &a->lat, &has_lat);
        } else if (!strcmp(k, "lon")) {
            ok = number(s, &a->lon, &has_lon);
        } else if (!strcmp(k, "seen_pos")) {
            ok = number(s, &v, &got);
            if (got) { a->seen_s = (float)v; has_seen_pos = true; }
        } else if (!strcmp(k, "seen")) {
            ok = number(s, &v, &got);
            if (got && !has_seen_pos) a->seen_s = (float)v;
        } else {
            ok = skip(s, 0);
        }
        if (!ok) return false;
        if (eat(s, ',')) continue;
        if (!eat(s, '}')) return false;
        break;
    }
    if (!has_alt && !a->ground && has_geom) a->alt_ft = (int32_t)lround(geom);
    if (a->ground) a->alt_ft = 0;
    if (heading >= 0) a->track = (int16_t)((int)lround(norm_deg(heading)) % 360);
    const double rate = has_rate_baro ? rate_baro : rate_geom;
    if (has_rate_baro || has_rate_geom) a->vrate_fpm = (int16_t)lround(fmax(-32000, fmin(32000, rate)));
    *pos = has_lat && has_lon && fabs(a->lat) <= 90 && fabs(a->lon) <= 180;
    return true;
}

static int by_dist(const void *l, const void *r)
{
    const float a = ((const adsb_ac_t *)l)->dist_nm, b = ((const adsb_ac_t *)r)->dist_nm;
    return (a > b) - (a < b);
}

int adsb_parse(const char *js, size_t len, double lat0, double lon0, float max_nm,
               adsb_ac_t *out, int cap, int *total)
{
    js_t s = { js, js + len };
    int n = 0, all = 0;
    bool found = false;
    if (total) *total = 0;
    if (!eat(&s, '{')) return -1;
    if (eat(&s, '}')) return -1;
    for (;;) {
        char k[16];
        if (!str(&s, k, sizeof k) || !eat(&s, ':')) return -1;
        if ((!strcmp(k, "ac") || !strcmp(k, "aircraft")) && peek(&s) == '[' && !found) {
            found = true;
            s.p++;
            if (!eat(&s, ']')) {
                for (;;) {
                    adsb_ac_t a;
                    bool pos;
                    if (!aircraft(&s, &a, &pos)) return -1;
                    all++;
                    if (pos) {
                        adsb_dist_brg(lat0, lon0, a.lat, a.lon, &a.dist_nm, &a.brg);
                        if (a.dist_nm <= max_nm && cap > 0) {
                            if (n < cap) {
                                out[n++] = a;
                            } else {
                                /* Full: the farthest kept gives way to a nearer. */
                                int far = 0;
                                for (int i = 1; i < n; i++) if (out[i].dist_nm > out[far].dist_nm) far = i;
                                if (a.dist_nm < out[far].dist_nm) out[far] = a;
                            }
                        }
                    }
                    if (eat(&s, ',')) continue;
                    if (!eat(&s, ']')) return -1;
                    break;
                }
            }
        } else if (!skip(&s, 0)) {
            return -1;
        }
        if (eat(&s, ',')) continue;
        if (!eat(&s, '}')) return -1;
        break;
    }
    if (!found) return -1;
    qsort(out, (size_t)n, sizeof *out, by_dist);
    if (total) *total = all;
    return n;
}

/* --- the feeds --------------------------------------------------------------------- */

bool adsb_src_ok(long s) { return s >= 0 && s < ADSB_SRCS; }

const char *adsb_src_name(uint8_t src)
{
    static const char *const N[ADSB_SRCS] = { "airplanes.live", "adsb.lol", "adsb.fi", "your receiver" };
    return N[src < ADSB_SRCS ? src : ADSB_SRC_ADSB_FI];
}

bool adsb_feed_url(uint8_t src, const char *local, double lat, double lon, float nm, char *out, size_t n)
{
    /* Whole NM, rounded up, 1 to 250: the internet feeds' own limit. */
    int r = (int)ceilf(nm);
    if (r < 1) r = 1;
    if (r > 250) r = 250;
    int k;
    switch (src < ADSB_SRCS ? src : ADSB_SRC_ADSB_FI) {
    case ADSB_SRC_AIRPLANES_LIVE:
        k = snprintf(out, n, "https://api.airplanes.live/v2/point/%.4f/%.4f/%d", lat, lon, r);
        break;
    case ADSB_SRC_ADSB_LOL:
        k = snprintf(out, n, "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", lat, lon, r);
        break;
    case ADSB_SRC_LOCAL: {
        while (local && *local == ' ') local++;
        if (!local || !*local) return false;
        size_t len = strlen(local);
        while (len && local[len - 1] == ' ') len--;
        const char *scheme = strstr(local, "://") ? "" : "http://";
        const char *host = strstr(local, "://") ? strstr(local, "://") + 3 : local;
        const char *slash = memchr(host, '/', len - (size_t)(host - local));
        k = snprintf(out, n, "%s%.*s%s", scheme, (int)len, local, slash ? "" : "/data/aircraft.json");
        break;
    }
    default:
        k = snprintf(out, n, "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%d", lat, lon, r);
        break;
    }
    return k > 0 && (size_t)k < n;
}

/* --- the ranges ---------------------------------------------------------------- */

static const uint16_t RANGES[ADSB_UNITS][ADSB_RANGES] = {
    [ADSB_NM] = { 5, 10, 20, 30, 50, 75, 100, 150, 200, 250 },
    [ADSB_KM] = { 10, 20, 40, 60, 100, 150, 200, 300, 400, 450 },
    [ADSB_MI] = { 5, 10, 20, 30, 50, 75, 100, 150, 200, 250 },
};
static const float NM_IN[ADSB_UNITS] = { 1.0f, 1.852f, 1.150779f };

static uint8_t unit(uint8_t u) { return u < ADSB_UNITS ? u : ADSB_NM; }

bool        adsb_units_ok(long u)          { return u >= 0 && u < ADSB_UNITS; }
const char *adsb_unit_name(uint8_t units)  { static const char *const N[] = { "NM", "km", "mi" }; return N[unit(units)]; }
float       adsb_nm_in(uint8_t units, float nm) { return nm * NM_IN[unit(units)]; }

uint16_t adsb_range_val(uint8_t units, uint8_t i)
{
    return RANGES[unit(units)][i < ADSB_RANGES ? i : ADSB_RANGES - 1];
}

float adsb_range_nm(uint8_t units, uint8_t i)
{
    return (float)adsb_range_val(units, i) / NM_IN[unit(units)];
}

uint8_t adsb_range_step(uint8_t i, int32_t detents)
{
    int32_t r = (int32_t)(i < ADSB_RANGES ? i : ADSB_RANGES - 1) + detents;
    if (r < 0) r = 0;
    if (r > ADSB_RANGES - 1) r = ADSB_RANGES - 1;
    return (uint8_t)r;
}

/* --- colour, filter, alerts ------------------------------------------------------ */

uint8_t adsb_band(const adsb_ac_t *a)
{
    static const int32_t TOP[] = { 2000, 10000, 20000, 30000, 40000 };
    if (a->ground || a->alt_ft == ADSB_ALT_NONE || a->alt_ft <= 0) return ADSB_BAND_GROUND;
    for (uint8_t i = 0; i < sizeof TOP / sizeof TOP[0]; i++)
        if (a->alt_ft < TOP[i]) return (uint8_t)(ADSB_BAND_2K + i);
    return ADSB_BAND_HIGH;
}

bool adsb_show_ok(long s) { return s >= 0 && s < ADSB_SHOWS; }

const char *adsb_emergency_name(const adsb_ac_t *a)
{
    static const struct { const char *code, *name; } SQ[] = {
        { "7500", "HIJACK" }, { "7600", "NO RADIO" }, { "7700", "EMERGENCY" },
    };
    static const struct { const char *code, *name; } EM[] = {
        { "general", "EMERGENCY" }, { "lifeguard", "MEDICAL" }, { "minfuel", "MINIMUM FUEL" },
        { "nordo", "NO RADIO" },    { "unlawful", "HIJACK" },   { "downed", "DOWNED" },
    };
    for (size_t i = 0; i < sizeof SQ / sizeof SQ[0]; i++)
        if (!strcmp(a->squawk, SQ[i].code)) return SQ[i].name;
    if (!a->emerg[0]) return "";
    for (size_t i = 0; i < sizeof EM / sizeof EM[0]; i++)
        if (!strcmp(a->emerg, EM[i].code)) return EM[i].name;
    return "EMERGENCY";
}

bool adsb_type_listed(const char *types, const char *type)
{
    if (!types || !type || !type[0]) return false;
    const char *p = types;
    while (*p) {
        while (*p && strchr(", ;\t\r\n", *p)) p++;
        const char *t = p;
        while (*p && !strchr(", ;\t\r\n", *p)) p++;
        size_t n = (size_t)(p - t);
        if (!n) continue;
        const bool prefix = t[n - 1] == '*';
        if (prefix) n--;
        if (prefix ? strlen(type) < n : strlen(type) != n) continue;
        size_t i = 0;
        while (i < n && toupper((unsigned char)t[i]) == toupper((unsigned char)type[i])) i++;
        if (i == n) return true;
    }
    return false;
}

uint8_t adsb_alert(const adsb_ac_t *a, const char *types)
{
    if (adsb_emergency_name(a)[0]) return ADSB_AL_EMERGENCY;
    if (adsb_type_listed(types, a->type)) return ADSB_AL_TYPE;
    return ADSB_AL_NONE;
}

bool adsb_shown(const adsb_ac_t *a, uint8_t show, bool ground, const char *types)
{
    const uint8_t al = adsb_alert(a, types);
    if (al == ADSB_AL_EMERGENCY) return true;
    if (a->ground && !ground) return false;
    if (al == ADSB_AL_TYPE) return true;
    switch (show) {
    case ADSB_SHOW_MILITARY:    return a->db_flags & ADSB_DB_MILITARY;
    case ADSB_SHOW_INTERESTING: return a->db_flags & ADSB_DB_INTERESTING;
    default:                    return true;
    }
}

/* --- a flight's route --------------------------------------------------------------- */

/* One end of the route, its object read whole: the town, else a code. */
static bool route_end(js_t *s, char *out, size_t cap)
{
    char town[40] = "", iata[8] = "", icao[8] = "";
    if (!eat(s, '{')) return skip(s, 0);
    if (!eat(s, '}')) {
        for (;;) {
            char k[16];
            if (!str(s, k, sizeof k) || !eat(s, ':')) return false;
            bool ok;
            if (!strcmp(k, "municipality"))   ok = text(s, town, sizeof town);
            else if (!strcmp(k, "iata_code")) ok = text(s, iata, sizeof iata);
            else if (!strcmp(k, "icao_code")) ok = text(s, icao, sizeof icao);
            else                              ok = skip(s, 0);
            if (!ok) return false;
            if (eat(s, ',')) continue;
            if (!eat(s, '}')) return false;
            break;
        }
    }
    const char *v = town[0] ? town : iata[0] ? iata : icao;
    if (cap) {
        strncpy(out, v, cap - 1);
        out[cap - 1] = 0;
    }
    return true;
}

/* Into the member `key` of the object at s, s left at its value. */
static bool member(js_t *s, const char *key)
{
    if (!eat(s, '{') || eat(s, '}')) return false;
    for (;;) {
        char k[24];
        if (!str(s, k, sizeof k) || !eat(s, ':')) return false;
        if (!strcmp(k, key)) return true;
        if (!skip(s, 0)) return false;
        if (!eat(s, ',')) return false;
    }
}

bool adsb_route_parse(const char *js, size_t len, char *from, size_t fn, char *to, size_t tn)
{
    js_t s = { js, js + len };
    if (fn) from[0] = 0;
    if (tn) to[0] = 0;
    if (!member(&s, "response") || !member(&s, "flightroute")) return false;
    if (!eat(&s, '{') || eat(&s, '}')) return false;
    for (;;) {
        char k[24];
        if (!str(&s, k, sizeof k) || !eat(&s, ':')) return false;
        bool ok;
        if (!strcmp(k, "origin"))           ok = route_end(&s, from, fn);
        else if (!strcmp(k, "destination")) ok = route_end(&s, to, tn);
        else                                ok = skip(&s, 0);
        if (!ok) return false;
        if (eat(&s, ',')) continue;
        break;
    }
    return fn && tn && from[0] && to[0];
}

/* --- the map ------------------------------------------------------------------------ */

#define MAP_HEAD 12
#define MAP_RUN  24
#define APT_HEAD 8
#define APT_REC  16

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int32_t  i32(const uint8_t *p) { int32_t v;  memcpy(&v, p, 4); return v; }
static int16_t  i16(const uint8_t *p) { int16_t v;  memcpy(&v, p, 2); return v; }
static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

void adsb_view_set(adsb_view_t *v, double lat0, double lon0, float range_nm, float radius_px)
{
    v->lat0 = lat0;
    v->lon0 = lon0;
    v->px_nm = range_nm > 0 ? radius_px / range_nm : 1;
    v->coslat = fmax(0.05, cos(lat0 * PI_D / 180.0));
}

void adsb_project(const adsb_view_t *v, double lat, double lon, float *x, float *y)
{
    double dl = fmod(lon - v->lon0 + 540.0, 360.0) - 180.0;    /* round the date line */
    *x = (float)(dl * 60.0 * v->coslat * v->px_nm);
    *y = (float)(-(lat - v->lat0) * 60.0 * v->px_nm);
}

bool adsb_map_ok(const uint8_t *p, size_t n)
{
    if (!p || n < MAP_HEAD || memcmp(p, "ADM1", 4)) return false;
    const uint64_t want = MAP_HEAD + (uint64_t)u32(p + 4) * MAP_RUN + (uint64_t)u32(p + 8) * 4;
    return want == n;
}

size_t adsb_map_runs(const uint8_t *p) { return u32(p + 4); }

static bool overlap(double a0, double a1, double b0, double b1) { return a0 <= b1 && b0 <= a1; }

bool adsb_map_run(const uint8_t *p, size_t i, const adsb_view_t *v, float half_nm, adsb_run_t *out)
{
    const uint8_t *r = p + MAP_HEAD + i * MAP_RUN;
    const double dlat = half_nm / 60.0, dlon = half_nm / (60.0 * v->coslat);
    const double la0 = i16(r + 8) / 100.0, la1 = i16(r + 10) / 100.0;
    const double lo0 = i16(r + 12) / 100.0, lo1 = i16(r + 14) / 100.0;
    if (!overlap(la0, la1, v->lat0 - dlat, v->lat0 + dlat)) return false;
    bool near = false;
    for (int w = -1; w <= 1 && !near; w++)
        near = overlap(lo0 + 360.0 * w, lo1 + 360.0 * w, v->lon0 - dlon, v->lon0 + dlon);
    if (!near) return false;
    out->lat = i32(r);
    out->lon = i32(r + 4);
    out->steps = u16(r + 20);
    out->kind = r[22];
    out->step = p + MAP_HEAD + adsb_map_runs(p) * MAP_RUN + (size_t)u32(r + 16) * 4;
    return true;
}

void adsb_run_step(const adsb_run_t *r, uint16_t k, int32_t *dlat, int32_t *dlon)
{
    *dlat = i16(r->step + (size_t)k * 4);
    *dlon = i16(r->step + (size_t)k * 4 + 2);
}

bool adsb_apt_ok(const uint8_t *p, size_t n)
{
    return p && n >= APT_HEAD && !memcmp(p, "ADA1", 4) && APT_HEAD + (uint64_t)u32(p + 4) * APT_REC == n;
}

size_t adsb_apt_count(const uint8_t *p) { return u32(p + 4); }

void adsb_apt_get(const uint8_t *p, size_t i, adsb_apt_t *out)
{
    const uint8_t *r = p + APT_HEAD + i * APT_REC;
    out->lat = i32(r);
    out->lon = i32(r + 4);
    memcpy(out->code, r + 8, 4);
    out->code[4] = 0;
    out->large = r[12];
}

size_t adsb_apt_from(const uint8_t *p, int32_t lat)
{
    size_t lo = 0, hi = adsb_apt_count(p);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (i32(p + APT_HEAD + mid * APT_REC) < lat) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

long adsb_apt_find(const uint8_t *p, const char *code)
{
    if (!code || strlen(code) != 4) return -1;
    char c[4];
    for (int i = 0; i < 4; i++) c[i] = (char)toupper((unsigned char)code[i]);
    const size_t n = adsb_apt_count(p);
    for (size_t i = 0; i < n; i++)
        if (!memcmp(p + APT_HEAD + i * APT_REC + 8, c, 4)) return (long)i;
    return -1;
}
