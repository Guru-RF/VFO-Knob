#include "ais.h"
#include "adsb.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_scan.h"

/* --- a ship ---------------------------------------------------------------------- */

void ais_ship_init(ais_ship_t *s, uint32_t mmsi)
{
    memset(s, 0, sizeof *s);
    s->mmsi = mmsi;
    s->nav = AIS_NAV_NONE;
    s->cls = AIS_CLS_OTHER;
    s->sog_dk = -1;
    s->cog_dd = -1;
    s->hdg = -1;
    s->eta_h = 24;
    s->eta_m = 60;
}

void ais_merge(ais_ship_t *d, const ais_ship_t *u, uint32_t f)
{
    if (f & AIS_F_POS) { d->pos = u->pos; d->lat = u->lat; d->lon = u->lon; d->seen_s = u->seen_s; }
    if (f & AIS_F_SOG) d->sog_dk = u->sog_dk;
    if (f & AIS_F_COG) d->cog_dd = u->cog_dd;
    if (f & AIS_F_HDG) d->hdg = u->hdg;
    if (f & AIS_F_NAV) d->nav = u->nav;
    if ((f & AIS_F_NAME) && u->name[0]) memcpy(d->name, u->name, sizeof d->name);
    if ((f & AIS_F_CALL) && u->callsign[0]) memcpy(d->callsign, u->callsign, sizeof d->callsign);
    if (f & AIS_F_TYPE) d->type = u->type;
    if (f & AIS_F_DIM) { d->len_m = u->len_m; d->beam_m = u->beam_m; }
    if (f & AIS_F_DEST) memcpy(d->dest, u->dest, sizeof d->dest);
    if (f & AIS_F_ETA) { d->eta_mon = u->eta_mon; d->eta_day = u->eta_day; d->eta_h = u->eta_h; d->eta_m = u->eta_m; }
    if (f & AIS_F_IMO) d->imo = u->imo;
    if (f & AIS_F_DRAUGHT) d->draught_dm = u->draught_dm;
    if (f & AIS_F_CLS) d->cls = u->cls;
}

/* AIS text as sent: '@' (six-bit nought) pads it, and blanks; cut at the
 * first '@', and the blanks at either end. */
static void ais_text(char *t)
{
    char *at = strchr(t, '@');
    if (at) *at = 0;
    size_t n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\t')) t[--n] = 0;
    size_t i = 0;
    while (t[i] == ' ') i++;
    if (i) memmove(t, t + i, n - i + 1);
}

static bool atext(js_t *s, char *out, size_t cap)
{
    if (!text(s, out, cap)) return false;
    ais_text(out);
    return true;
}

static bool ok_lat(double v) { return fabs(v) <= 90; }
static bool ok_lon(double v) { return fabs(v) <= 180; }

static int16_t sog_of(double kn)
{
    if (kn < 0 || kn >= 102.25) return -1;    /* 102.3: not available */
    return (int16_t)lround(kn * 10);
}

static int16_t cog_of(double deg)
{
    if (deg < 0 || deg >= 360) return -1;    /* 360: not available */
    return (int16_t)(lround(deg * 10) % 3600);
}

static int16_t hdg_of(double deg)
{
    if (deg < 0 || deg >= 360) return -1;    /* 511: not available */
    return (int16_t)((int)lround(deg) % 360);
}

static void dims(ais_ship_t *u, double a, double b, double c, double d)
{
    const double l = fmax(0, a) + fmax(0, b), w = fmax(0, c) + fmax(0, d);
    u->len_m = (uint16_t)fmin(1023, lround(l));
    u->beam_m = (uint16_t)fmin(127, lround(w));
}

/* --- AISStream.io ------------------------------------------------------------------ */

enum { T_NONE, T_POS_A, T_POS_B, T_POS_BX, T_STATIC, T_STATIC_B, T_ATON, T_BASE, T_SAR };

static int type_of(const char *t)
{
    static const struct { const char *n; int t; } T[] = {
        { "PositionReport", T_POS_A },
        { "StandardClassBPositionReport", T_POS_B },
        { "ExtendedClassBPositionReport", T_POS_BX },
        { "ShipStaticData", T_STATIC },
        { "StaticDataReport", T_STATIC_B },
        { "AidsToNavigationReport", T_ATON },
        { "BaseStationReport", T_BASE },
        { "StandardSearchAndRescueAircraftReport", T_SAR },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(t, T[i].n)) return T[i].t;
    return T_NONE;
}

/* An object, its members handed to `each` one at a time; false for broken
 * text, or what `each` refused. */
typedef bool (*member_fn)(js_t *s, const char *key, void *ctx);

static bool members(js_t *s, member_fn each, void *ctx)
{
    if (!eat(s, '{')) return false;
    if (eat(s, '}')) return true;
    for (;;) {
        char k[40];
        if (!str(s, k, sizeof k) || !eat(s, ':')) return false;
        if (!each(s, k, ctx)) return false;
        if (eat(s, ',')) continue;
        return eat(s, '}');
    }
}

typedef struct {
    ais_ship_t *u;
    uint32_t   *f;
    int         t;
    double      lat, lon;
    bool        has_lat, has_lon;
    double      dim[4];
    bool        has_dim;
    bool        part_b, has_part;    /* StaticDataReport's PartNumber */
    bool        valid_a, valid_b;    /* ...each part's own Valid */
    bool        has_va, has_vb;
    char        name_a[21];
    char        call_b[8];
    double      type_b;
    bool        has_type_b;
    double      dim_b[4];
    bool        has_dim_b;
} body_t;

static bool dim_member(js_t *s, const char *k, void *ctx)
{
    double *d = ctx, v = 0;
    bool got = false;
    const int i = !strcmp(k, "A") ? 0 : !strcmp(k, "B") ? 1 : !strcmp(k, "C") ? 2 : !strcmp(k, "D") ? 3 : -1;
    if (i < 0) return skip(s, 0);
    if (!number(s, &v, &got)) return false;
    if (got) d[i] = v;
    return true;
}

static bool eta_member(js_t *s, const char *k, void *ctx)
{
    ais_ship_t *u = ctx;
    double v = 0;
    bool got = false;
    if (!number(s, &v, &got)) return false;
    if (!got) return true;
    const long n = lround(v);
    if (!strcmp(k, "Month")) u->eta_mon = n >= 1 && n <= 12 ? (uint8_t)n : 0;
    else if (!strcmp(k, "Day")) u->eta_day = n >= 1 && n <= 31 ? (uint8_t)n : 0;
    else if (!strcmp(k, "Hour")) u->eta_h = n >= 0 && n <= 23 ? (uint8_t)n : 24;
    else if (!strcmp(k, "Minute")) u->eta_m = n >= 0 && n <= 59 ? (uint8_t)n : 60;
    return true;
}

static bool bool_val(js_t *s, bool *v)
{
    const char c = peek(s);
    if (c == 't' || c == 'f') {
        *v = c == 't';
        return skip(s, 0);
    }
    double d = 0;
    bool got = false;
    if (!number(s, &d, &got)) return false;
    *v = got && d != 0;
    return true;
}

static bool report_a(js_t *s, const char *k, void *ctx)
{
    body_t *b = ctx;
    if (!strcmp(k, "Name")) return atext(s, b->name_a, sizeof b->name_a);
    if (!strcmp(k, "Valid")) { b->has_va = true; return bool_val(s, &b->valid_a); }
    return skip(s, 0);
}

static bool report_b(js_t *s, const char *k, void *ctx)
{
    body_t *b = ctx;
    if (!strcmp(k, "CallSign")) return atext(s, b->call_b, sizeof b->call_b);
    if (!strcmp(k, "ShipType")) return number(s, &b->type_b, &b->has_type_b);
    if (!strcmp(k, "Dimension")) { b->has_dim_b = true; return members(s, dim_member, b->dim_b); }
    if (!strcmp(k, "Valid")) { b->has_vb = true; return bool_val(s, &b->valid_b); }
    return skip(s, 0);
}

static bool body_member(js_t *s, const char *k, void *ctx)
{
    body_t *b = ctx;
    ais_ship_t *u = b->u;
    double v = 0;
    bool got = false;
    if (!strcmp(k, "UserID")) {
        if (!number(s, &v, &got)) return false;
        if (got && v > 0 && v < 1e9 && !u->mmsi) u->mmsi = (uint32_t)v;
        return true;
    }
    if (!strcmp(k, "Latitude"))  return number(s, &b->lat, &b->has_lat);
    if (!strcmp(k, "Longitude")) return number(s, &b->lon, &b->has_lon);
    if (!strcmp(k, "Sog")) {
        if (!number(s, &v, &got)) return false;
        if (got) { u->sog_dk = sog_of(v); *b->f |= AIS_F_SOG; }
        return true;
    }
    if (!strcmp(k, "Cog")) {
        if (!number(s, &v, &got)) return false;
        if (got) { u->cog_dd = cog_of(v); *b->f |= AIS_F_COG; }
        return true;
    }
    if (!strcmp(k, "TrueHeading")) {
        if (!number(s, &v, &got)) return false;
        if (got) { u->hdg = hdg_of(v); *b->f |= AIS_F_HDG; }
        return true;
    }
    if (!strcmp(k, "NavigationalStatus")) {
        if (!number(s, &v, &got)) return false;
        if (got) { u->nav = v >= 0 && v <= 15 ? (uint8_t)v : AIS_NAV_NONE; *b->f |= AIS_F_NAV; }
        return true;
    }
    if (!strcmp(k, "Name")) {
        if (!atext(s, u->name, sizeof u->name)) return false;
        if (u->name[0]) *b->f |= AIS_F_NAME;
        return true;
    }
    if (!strcmp(k, "CallSign")) {
        if (!atext(s, u->callsign, sizeof u->callsign)) return false;
        if (u->callsign[0]) *b->f |= AIS_F_CALL;
        return true;
    }
    if (!strcmp(k, "Destination")) {
        if (!atext(s, u->dest, sizeof u->dest)) return false;
        *b->f |= AIS_F_DEST;
        return true;
    }
    if (!strcmp(k, "Type") && b->t != T_ATON) {     /* an aid's Type is its own kind of aid */
        if (!number(s, &v, &got)) return false;
        if (got) { u->type = v >= 0 && v <= 99 ? (uint8_t)v : 0; *b->f |= AIS_F_TYPE; }
        return true;
    }
    if (!strcmp(k, "ImoNumber")) {
        if (!number(s, &v, &got)) return false;
        if (got) { u->imo = v > 0 && v < 1e10 ? (uint32_t)v : 0; *b->f |= AIS_F_IMO; }
        return true;
    }
    if (!strcmp(k, "MaximumStaticDraught")) {
        if (!number(s, &v, &got)) return false;
        if (got) { u->draught_dm = (uint8_t)fmax(0, fmin(255, lround(v * 10))); *b->f |= AIS_F_DRAUGHT; }
        return true;
    }
    if (!strcmp(k, "Dimension")) { b->has_dim = true; return members(s, dim_member, b->dim); }
    if (!strcmp(k, "Eta")) {
        if (!members(s, eta_member, u)) return false;
        *b->f |= AIS_F_ETA;
        return true;
    }
    if (!strcmp(k, "PartNumber")) { b->has_part = true; return bool_val(s, &b->part_b); }
    if (!strcmp(k, "ReportA")) return members(s, report_a, b);
    if (!strcmp(k, "ReportB")) return members(s, report_b, b);
    return skip(s, 0);
}

typedef struct {
    ais_ship_t *u;
    uint32_t   *f;
    int         t;               /* the Message's own key */
    bool        body_read;
    char        meta_name[21];
    double      meta_mmsi;
    bool        has_meta_mmsi;
    bool        compressed;      /* a confirmation's Message, whichever comes first */
    bool        error;
    char       *why;
    size_t      wn;
    char        mtype[40];
} msg_t;

static bool meta_member(js_t *s, const char *k, void *ctx)
{
    msg_t *m = ctx;
    if (!strcmp(k, "MMSI")) return number(s, &m->meta_mmsi, &m->has_meta_mmsi);
    if (!strcmp(k, "ShipName")) return atext(s, m->meta_name, sizeof m->meta_name);
    return skip(s, 0);
}

static void body_end(body_t *b)
{
    ais_ship_t *u = b->u;
    if (b->has_lat && b->has_lon && ok_lat(b->lat) && ok_lon(b->lon) && !(b->lat == 0 && b->lon == 0)) {
        u->pos = true;
        u->lat = b->lat;
        u->lon = b->lon;
        u->seen_s = 0;
        *b->f |= AIS_F_POS;
    }
    if (b->has_dim && (b->dim[0] + b->dim[1] > 0 || b->dim[2] + b->dim[3] > 0)) {
        dims(u, b->dim[0], b->dim[1], b->dim[2], b->dim[3]);
        *b->f |= AIS_F_DIM;
    }
    if (b->t == T_STATIC_B) {
        /* Part A carries the name, part B the rest; each says if it is
         * the one sent, else PartNumber does. */
        const bool a = b->has_va ? b->valid_a : (b->has_part && !b->part_b);
        const bool bb = b->has_vb ? b->valid_b : (b->has_part && b->part_b);
        if (a && b->name_a[0]) { memcpy(u->name, b->name_a, sizeof u->name); *b->f |= AIS_F_NAME; }
        if (bb) {
            if (b->call_b[0]) { memcpy(u->callsign, b->call_b, sizeof u->callsign); *b->f |= AIS_F_CALL; }
            if (b->has_type_b) { u->type = b->type_b >= 0 && b->type_b <= 99 ? (uint8_t)b->type_b : 0; *b->f |= AIS_F_TYPE; }
            if (b->has_dim_b && (b->dim_b[0] + b->dim_b[1] > 0 || b->dim_b[2] + b->dim_b[3] > 0)) {
                dims(u, b->dim_b[0], b->dim_b[1], b->dim_b[2], b->dim_b[3]);
                *b->f |= AIS_F_DIM;
            }
        }
    }
}

static bool message_member(js_t *s, const char *k, void *ctx)
{
    msg_t *m = ctx;
    const int t = type_of(k);
    if (t == T_NONE) {
        if (!strcmp(k, "CompressionEnabled")) return bool_val(s, &m->compressed);
        return skip(s, 0);
    }
    body_t b = { .u = m->u, .f = m->f, .t = t };
    if (!members(s, body_member, &b)) return false;
    body_end(&b);
    m->t = t;
    m->body_read = true;
    return true;
}

static bool top_member(js_t *s, const char *k, void *ctx)
{
    msg_t *m = ctx;
    if (!strcmp(k, "MessageType")) return text(s, m->mtype, sizeof m->mtype);
    if (!strcmp(k, "MetaData") || !strcmp(k, "Metadata")) return members(s, meta_member, m);
    if (!strcmp(k, "Message")) return peek(s) == '{' ? members(s, message_member, m) : skip(s, 0);
    if (!strcmp(k, "error") || !strcmp(k, "Error")) {
        m->error = true;
        if (m->why && m->wn) return text(s, m->why, m->wn);
        return skip(s, 0);
    }
    return skip(s, 0);
}

int ais_stream_parse(const char *js, size_t len, ais_ship_t *u, uint32_t *fields, bool *compressed,
                     char *why, size_t wn)
{
    uint32_t f = 0;
    ais_ship_init(u, 0);
    if (why && wn) why[0] = 0;
    msg_t m = { .u = u, .f = &f, .why = why, .wn = wn };
    js_t s = { js, js + len };
    if (!members(&s, top_member, &m)) return AIS_MSG_BAD;
    if (fields) *fields = 0;
    if (m.error) return AIS_MSG_ERROR;
    if (!strcmp(m.mtype, "SubscriptionConfirmation")) {
        if (compressed) *compressed = m.compressed;
        return AIS_MSG_CONFIRM;
    }
    if (!m.body_read) return m.mtype[0] ? AIS_MSG_OTHER : AIS_MSG_BAD;
    if (!u->mmsi && m.has_meta_mmsi && m.meta_mmsi > 0 && m.meta_mmsi < 1e9) u->mmsi = (uint32_t)m.meta_mmsi;
    if (!u->mmsi) return AIS_MSG_OTHER;
    /* Its name from the stream's own record of it, until a message of its
     * own names it. */
    if (!(f & AIS_F_NAME) && m.meta_name[0]) {
        memcpy(u->name, m.meta_name, sizeof u->name);
        f |= AIS_F_NAME;
    }
    switch (m.t) {
    case T_POS_A: case T_STATIC:              u->cls = AIS_CLS_A; break;
    case T_POS_B: case T_POS_BX: case T_STATIC_B: u->cls = AIS_CLS_B; break;
    case T_ATON:                              u->cls = AIS_CLS_ATON; break;
    case T_BASE:                              u->cls = AIS_CLS_BASE; break;
    case T_SAR:                               u->cls = AIS_CLS_SAR_AIRCRAFT; break;
    default:                                  u->cls = AIS_CLS_OTHER; break;
    }
    f |= AIS_F_CLS;
    if (fields) *fields = f;
    return AIS_MSG_SHIP;
}

void ais_box(double lat, double lon, float nm, double *lat1, double *lon1, double *lat2, double *lon2)
{
    const double dlat = nm / 60.0;
    double c = cos(lat * 3.14159265358979323846 / 180.0);
    if (c < 0.05) c = 0.05;
    const double dlon = fmin(180.0, nm / (60.0 * c));
    *lat1 = fmax(-90.0, lat - dlat);
    *lat2 = fmin(90.0, lat + dlat);
    *lon1 = lon - dlon;
    *lon2 = lon + dlon;
    if (*lon1 < -180) *lon1 = -180;
    if (*lon2 > 180) *lon2 = 180;
}

bool ais_stream_sub(const char *key, double lat, double lon, float nm, char *out, size_t n)
{
    if (!key || !*key || !out || !n) return false;
    char k[80];
    size_t j = 0;
    for (const char *p = key; *p; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '"' || c == '\\' || c >= 0x7F) return false;   /* no key has them */
        if (j + 1 >= sizeof k) return false;
        k[j++] = (char)c;
    }
    k[j] = 0;
    double a1, o1, a2, o2;
    ais_box(lat, lon, nm, &a1, &o1, &a2, &o2);
    const int w = snprintf(out, n,
        "{\"APIKey\":\"%s\",\"BoundingBoxes\":[[[%.4f,%.4f],[%.4f,%.4f]]],"
        "\"FilterMessageTypes\":[\"PositionReport\",\"StandardClassBPositionReport\","
        "\"ExtendedClassBPositionReport\",\"ShipStaticData\",\"StaticDataReport\","
        "\"AidsToNavigationReport\",\"BaseStationReport\",\"StandardSearchAndRescueAircraftReport\"]}",
        k, a1, o1, a2, o2);
    return w > 0 && (size_t)w < n;
}

/* --- AIS-catcher -------------------------------------------------------------------- */

typedef struct {
    ais_ship_t *u;
    double lat, lon;
    bool has_lat, has_lon;
    double d[4];
    bool has_d;
} cat_t;

static bool cat_member(js_t *s, const char *k, void *ctx)
{
    cat_t *c = ctx;
    ais_ship_t *u = c->u;
    double v = 0;
    bool got = false;
    if (!strcmp(k, "shipname"))    return atext(s, u->name, sizeof u->name);
    if (!strcmp(k, "callsign"))    return atext(s, u->callsign, sizeof u->callsign);
    if (!strcmp(k, "destination")) return atext(s, u->dest, sizeof u->dest);
    if (!number(s, &v, &got)) return false;        /* the rest are numbers, or null, or skipped */
    if (!got) return true;
    if (!strcmp(k, "mmsi"))              u->mmsi = v > 0 && v < 1e9 ? (uint32_t)v : 0;
    else if (!strcmp(k, "lat"))          { c->lat = v; c->has_lat = true; }
    else if (!strcmp(k, "lon"))          { c->lon = v; c->has_lon = true; }
    else if (!strcmp(k, "speed"))        u->sog_dk = sog_of(v);
    else if (!strcmp(k, "cog"))          u->cog_dd = cog_of(v);
    else if (!strcmp(k, "heading"))      u->hdg = hdg_of(v);
    else if (!strcmp(k, "status"))       u->nav = v >= 0 && v <= 15 ? (uint8_t)v : AIS_NAV_NONE;
    else if (!strcmp(k, "shiptype"))     u->type = v >= 0 && v <= 99 ? (uint8_t)v : 0;
    else if (!strcmp(k, "imo"))          u->imo = v > 0 && v < 1e10 ? (uint32_t)v : 0;
    else if (!strcmp(k, "draught"))      u->draught_dm = (uint8_t)fmax(0, fmin(255, lround(v * 10)));
    else if (!strcmp(k, "to_bow"))       { c->d[0] = v; c->has_d = true; }
    else if (!strcmp(k, "to_stern"))     { c->d[1] = v; c->has_d = true; }
    else if (!strcmp(k, "to_port"))      { c->d[2] = v; c->has_d = true; }
    else if (!strcmp(k, "to_starboard")) { c->d[3] = v; c->has_d = true; }
    else if (!strcmp(k, "eta_month"))    u->eta_mon = v >= 1 && v <= 12 ? (uint8_t)v : 0;
    else if (!strcmp(k, "eta_day"))      u->eta_day = v >= 1 && v <= 31 ? (uint8_t)v : 0;
    else if (!strcmp(k, "eta_hour"))     u->eta_h = v >= 0 && v <= 23 ? (uint8_t)v : 24;
    else if (!strcmp(k, "eta_minute"))   u->eta_m = v >= 0 && v <= 59 ? (uint8_t)v : 60;
    else if (!strcmp(k, "last_signal"))  u->seen_s = (float)fmax(0, v);
    else if (!strcmp(k, "mmsi_type")) {
        static const uint8_t C[] = { AIS_CLS_OTHER, AIS_CLS_A, AIS_CLS_B, AIS_CLS_BASE,
                                     AIS_CLS_SAR_AIRCRAFT, AIS_CLS_SART, AIS_CLS_ATON };
        u->cls = v >= 0 && v < (double)sizeof C ? C[(int)v] : AIS_CLS_OTHER;
    }
    return true;
}

static int by_dist(const void *l, const void *r)
{
    const float a = ((const ais_ship_t *)l)->dist_nm, b = ((const ais_ship_t *)r)->dist_nm;
    return (a > b) - (a < b);
}

int ais_catcher_parse(const char *js, size_t len, double lat0, double lon0, float max_nm,
                      ais_ship_t *out, int cap, int *total)
{
    js_t s = { js, js + len };
    int n = 0, all = 0;
    bool found = false;
    if (total) *total = 0;
    if (!eat(&s, '{') || eat(&s, '}')) return -1;
    for (;;) {
        char k[16];
        if (!str(&s, k, sizeof k) || !eat(&s, ':')) return -1;
        if (!strcmp(k, "ships") && peek(&s) == '[' && !found) {
            found = true;
            s.p++;
            if (!eat(&s, ']')) {
                for (;;) {
                    ais_ship_t u;
                    ais_ship_init(&u, 0);
                    cat_t c = { .u = &u };
                    if (!members(&s, cat_member, &c)) return -1;
                    all++;
                    if (c.has_d && (c.d[0] + c.d[1] > 0 || c.d[2] + c.d[3] > 0)) dims(&u, c.d[0], c.d[1], c.d[2], c.d[3]);
                    if (u.mmsi && c.has_lat && c.has_lon && ok_lat(c.lat) && ok_lon(c.lon) && !(c.lat == 0 && c.lon == 0)) {
                        u.pos = true;
                        u.lat = c.lat;
                        u.lon = c.lon;
                        adsb_dist_brg(lat0, lon0, u.lat, u.lon, &u.dist_nm, &u.brg);
                        if (u.dist_nm <= max_nm && cap > 0) {
                            if (n < cap) {
                                out[n++] = u;
                            } else {
                                int far = 0;
                                for (int i = 1; i < n; i++) if (out[i].dist_nm > out[far].dist_nm) far = i;
                                if (u.dist_nm < out[far].dist_nm) out[far] = u;
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

bool ais_catcher_url(const char *addr, char *out, size_t n)
{
    while (addr && *addr == ' ') addr++;
    if (!addr || !*addr) return false;
    size_t len = strlen(addr);
    while (len && addr[len - 1] == ' ') len--;
    const char *sep = strstr(addr, "://");
    const char *host = sep ? sep + 3 : addr;
    const char *slash = memchr(host, '/', len - (size_t)(host - addr));
    const bool bare = !slash || (size_t)(slash - addr) == len - 1;       /* none, or a lone "/" */
    const size_t keep = slash && bare ? (size_t)(slash - addr) : len;
    const int k = snprintf(out, n, "%s%.*s%s", sep ? "" : "http://", (int)keep, addr, bare ? "/ships.json" : "");
    return k > 0 && (size_t)k < n;
}

/* --- the sources ------------------------------------------------------------------- */

bool ais_src_ok(long s) { return s >= 0 && s < AIS_SRCS; }

const char *ais_src_name(uint8_t src)
{
    return src == AIS_SRC_CATCHER ? "your AIS-catcher" : "AISStream";
}

/* --- kinds --------------------------------------------------------------------------- */

static bool mmsi_in(uint32_t m, uint32_t lo, uint32_t span) { return m >= lo && m < lo + span; }

uint8_t ais_kind(const ais_ship_t *s)
{
    const uint32_t m = s->mmsi;
    if (ais_emergency_name(s)[0]) return AIS_K_EMERGENCY;
    if (s->cls == AIS_CLS_ATON || mmsi_in(m, 990000000, 10000000)) return AIS_K_ATON;
    if (s->cls == AIS_CLS_BASE || (m && m < 10000000)) return AIS_K_BASE;
    if (s->cls == AIS_CLS_SAR_AIRCRAFT || mmsi_in(m, 111000000, 1000000)) return AIS_K_SAR_AIRCRAFT;
    const uint8_t t = s->type;
    if (t == 30) return AIS_K_FISHING;
    if (t == 36 || t == 37) return AIS_K_PLEASURE;
    if ((t >= 31 && t <= 35) || (t >= 50 && t <= 55) || t == 58 || t == 59) return AIS_K_SPECIAL;
    if (t >= 40 && t <= 49) return AIS_K_HSC;
    if (t >= 60 && t <= 69) return AIS_K_PASSENGER;
    if (t >= 70 && t <= 79) return AIS_K_CARGO;
    if (t >= 80 && t <= 89) return AIS_K_TANKER;
    /* No type yet (it comes every six minutes) but the status says it: a ship
     * engaged in fishing, or under way sailing. */
    if (!t && s->nav == 7) return AIS_K_FISHING;
    if (!t && s->nav == 8) return AIS_K_PLEASURE;
    return AIS_K_OTHER;
}

const char *ais_type_name(const ais_ship_t *s)
{
    const char *e = ais_emergency_name(s);
    if (*e && mmsi_in(s->mmsi, 970000000, 5000000)) return e;
    switch (ais_kind(s)) {
    case AIS_K_ATON:         return "Aid to navigation";
    case AIS_K_BASE:         return "Base station";
    case AIS_K_SAR_AIRCRAFT: return "SAR aircraft";
    default: break;
    }
    const uint8_t t = s->type;
    if (t >= 20 && t <= 29) return "Wing in ground";
    if (t >= 40 && t <= 49) return "High-speed craft";
    if (t >= 60 && t <= 69) return "Passenger";
    if (t >= 70 && t <= 79) return "Cargo";
    if (t >= 80 && t <= 89) return "Tanker";
    if (t >= 90 && t <= 99) return "Other type";
    switch (t) {
    case 30: return "Fishing";
    case 31: case 32: return "Towing";
    case 33: return "Dredging";
    case 34: return "Diving";
    case 35: return "Military";
    case 36: return "Sailing";
    case 37: return "Pleasure craft";
    case 50: return "Pilot vessel";
    case 51: return "Search and rescue";
    case 52: return "Tug";
    case 53: return "Port tender";
    case 54: return "Anti-pollution";
    case 55: return "Law enforcement";
    case 58: return "Medical transport";
    case 59: return "Noncombatant";
    default: return "";
    }
}

const char *ais_nav_name(uint8_t nav)
{
    static const char *const N[16] = {
        "under way", "at anchor", "not under command", "restricted", "constrained by draught",
        "moored", "aground", "fishing", "under way sailing", "", "", "towing astern",
        "pushing ahead", "", "AIS-SART active", "",
    };
    return nav < 16 ? N[nav] : "";
}

bool ais_moving(const ais_ship_t *s) { return s->sog_dk >= 5; }

int ais_course(const ais_ship_t *s)
{
    if (s->cog_dd >= 0) return (s->cog_dd + 5) / 10 % 360;
    return s->hdg >= 0 ? s->hdg % 360 : -1;
}

/* --- alerts ------------------------------------------------------------------------------ */

const char *ais_emergency_name(const ais_ship_t *s)
{
    const uint32_t m = s->mmsi;
    if (mmsi_in(m, 970000000, 1000000)) return "AIS-SART";
    if (mmsi_in(m, 972000000, 1000000)) return "MAN OVERBOARD";
    if (mmsi_in(m, 974000000, 1000000)) return "EPIRB";
    if (s->nav == 14) return "AIS-SART";
    return "";
}

static bool same_ci(const char *a, size_t n, const char *b, bool prefix)
{
    if (prefix ? strlen(b) < n : strlen(b) != n) return false;
    for (size_t i = 0; i < n; i++)
        if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i])) return false;
    return true;
}

bool ais_watched(const char *watch, const ais_ship_t *s)
{
    if (!watch || !*watch) return false;
    char mm[12];
    snprintf(mm, sizeof mm, "%09lu", (unsigned long)s->mmsi);
    const char *p = watch;
    while (*p) {
        while (*p && strchr(",;\r\n\t ", *p)) p++;
        const char *t = p;
        while (*p && !strchr(",;\r\n", *p)) p++;
        size_t n = (size_t)(p - t);
        while (n && (t[n - 1] == ' ' || t[n - 1] == '\t')) n--;
        if (!n) continue;
        const bool prefix = t[n - 1] == '*';
        if (prefix) n--;
        if (!n) continue;
        bool digits = true;
        for (size_t i = 0; i < n; i++) digits = digits && isdigit((unsigned char)t[i]);
        if (digits ? same_ci(t, n, mm, prefix) : (s->name[0] && same_ci(t, n, s->name, prefix))) return true;
    }
    return false;
}

uint8_t ais_alert(const ais_ship_t *s, const char *watch)
{
    if (ais_emergency_name(s)[0]) return AIS_AL_EMERGENCY;
    return ais_watched(watch, s) ? AIS_AL_WATCH : AIS_AL_NONE;
}

bool ais_show_ok(long s) { return s >= 0 && s < AIS_SHOWS; }

bool ais_shown(const ais_ship_t *s, uint8_t show, const char *watch)
{
    if (ais_alert(s, watch) != AIS_AL_NONE) return true;
    const uint8_t k = ais_kind(s);
    switch (show) {
    case AIS_SHOW_SHIPS:  return k != AIS_K_ATON && k != AIS_K_BASE;
    case AIS_SHOW_MOVING: return k != AIS_K_ATON && k != AIS_K_BASE && ais_moving(s);
    default:              return true;
    }
}

/* --- the ranges ---------------------------------------------------------------------------- */

static const uint16_t RANGES[ADSB_UNITS][AIS_RANGES] = {
    [ADSB_NM] = { 1, 2, 3, 5, 8, 12, 20, 30, 50 },
    [ADSB_KM] = { 2, 4, 6, 10, 15, 25, 40, 60, 90 },
    [ADSB_MI] = { 1, 2, 3, 5, 8, 12, 20, 30, 50 },
};

static uint8_t unit(uint8_t u) { return u < ADSB_UNITS ? u : ADSB_NM; }

uint16_t ais_range_val(uint8_t units, uint8_t i)
{
    return RANGES[unit(units)][i < AIS_RANGES ? i : AIS_RANGES - 1];
}

float ais_range_nm(uint8_t units, uint8_t i)
{
    return (float)ais_range_val(units, i) / adsb_nm_in(unit(units), 1.0f);    /* the unit's in a NM */
}

uint8_t ais_range_step(uint8_t i, int32_t detents)
{
    int32_t r = (int32_t)(i < AIS_RANGES ? i : AIS_RANGES - 1) + detents;
    if (r < 0) r = 0;
    if (r > AIS_RANGES - 1) r = AIS_RANGES - 1;
    return (uint8_t)r;
}

/* --- the harbours -------------------------------------------------------------------------- */

#define PORT_HEAD 8
#define PORT_REC  28

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int32_t  i32(const uint8_t *p) { int32_t v;  memcpy(&v, p, 4); return v; }

bool ais_port_ok(const uint8_t *p, size_t n)
{
    return p && n >= PORT_HEAD && !memcmp(p, "AIP1", 4) && PORT_HEAD + (uint64_t)u32(p + 4) * PORT_REC == n;
}

size_t ais_port_count(const uint8_t *p) { return u32(p + 4); }

void ais_port_get(const uint8_t *p, size_t i, ais_port_t *out)
{
    const uint8_t *r = p + PORT_HEAD + i * PORT_REC;
    out->lat = i32(r);
    out->lon = i32(r + 4);
    memcpy(out->name, r + 8, 20);
    out->name[19] = 0;
}

size_t ais_port_from(const uint8_t *p, int32_t lat)
{
    size_t lo = 0, hi = ais_port_count(p);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (i32(p + PORT_HEAD + mid * PORT_REC) < lat) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}
