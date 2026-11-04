#include "weather.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t wx_url(double lat, double lon, char *out, size_t n)
{
    const int k = snprintf(out, n,
                           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,"
                           "precipitation,weather_code,pressure_msl,wind_speed_10m,wind_direction_10m,"
                           "wind_gusts_10m"
                           "&hourly=temperature_2m,precipitation_probability,weather_code,is_day"
                           "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,"
                           "precipitation_probability_max"
                           "&timezone=auto&forecast_days=%d&forecast_hours=%d&timeformat=unixtime",
                           lat, lon, WX_DAYS, WX_HOURS);
    return k > 0 && (size_t)k < n ? (size_t)k : 0;
}

/* ---- the reader: Open-Meteo's members found by walking each object's top
 * level, so a key of the same name further in is never taken for it. ---- */

static const char *ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return p;
}

/* Past the string opening at p: one past its closing quote. */
static const char *skip_str(const char *p, const char *end)
{
    for (p++; p < end; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p + 1;
    }
    return end;
}

/* Past the object or array opening at p; end if it never closes. */
static const char *close_of(const char *p, const char *end)
{
    int depth = 0;
    while (p < end) {
        const char c = *p;
        if (c == '"') { p = skip_str(p, end); continue; }
        if (c == '{' || c == '[') depth++;
        else if ((c == '}' || c == ']') && --depth == 0) return p + 1;
        p++;
    }
    return end;
}

/* The value of the object's member `key`: its first character; NULL when
 * it has none, or is not an object. */
static const char *member(const char *obj, const char *end, const char *key)
{
    if (!obj || obj >= end || *obj != '{') return NULL;
    const size_t kl = strlen(key);
    const char *p = obj + 1;
    for (;;) {
        p = ws(p, end);
        if (p >= end || *p != '"') return NULL;
        const char *k = p + 1, *ke = skip_str(p, end);
        const bool hit = ke > k && (size_t)(ke - 1 - k) == kl && !memcmp(k, key, kl);
        p = ws(ke, end);
        if (p >= end || *p != ':') return NULL;
        p = ws(p + 1, end);
        if (p >= end) return NULL;
        if (hit) return p;
        if (*p == '"') p = skip_str(p, end);
        else if (*p == '{' || *p == '[') p = close_of(p, end);
        else while (p < end && *p != ',' && *p != '}') p++;
        p = ws(p, end);
        if (p >= end || *p != ',') return NULL;
        p++;
    }
}

static bool num(const char *p, const char *end, double *v)
{
    if (!p || p >= end) return false;
    char *e;
    const double d = strtod(p, &e);
    if (e == p || e > end || !isfinite(d)) return false;
    *v = d;
    return true;
}

/* The member as a number, or dflt. */
static double get(const char *obj, const char *end, const char *key, double dflt)
{
    double v;
    return num(member(obj, end, key), end, &v) ? v : dflt;
}

/* The member's array of numbers, up to cap: how many, a null NAN; -1 when it
 * is not an array of numbers. */
static int nums(const char *obj, const char *end, const char *key, double *out, int cap)
{
    const char *p = member(obj, end, key);
    if (!p || *p != '[') return -1;
    const char *e = close_of(p, end);
    int n = 0;
    for (p++; n < cap;) {
        p = ws(p, e);
        if (p >= e || *p == ']') break;
        double v = NAN;
        if (e - p >= 4 && !memcmp(p, "null", 4)) {
            p += 4;
        } else {
            char *q;
            v = strtod(p, &q);
            if (q == p || q > e) return -1;
            p = q;
        }
        out[n++] = v;
        p = ws(p, e);
        if (p < e && *p == ',') p++;
    }
    return n;
}

static uint8_t pct(double v) { return isnan(v) || v < 0 ? 0 : v > 100 ? 100 : (uint8_t)lround(v); }
static uint8_t wmo(double v) { return isnan(v) || v < 0 || v > 99 ? 3 : (uint8_t)v; }

static int least(int a, int b) { return a < b ? a : b; }

bool wx_parse(const char *js, size_t len, wx_t *out)
{
    memset(out, 0, sizeof *out);
    if (!js) return false;
    const char *end = js + len;
    const char *root = ws(js, end);
    if (root >= end || *root != '{') return false;

    out->utc_offset_s = (int32_t)get(root, end, "utc_offset_seconds", 0);
    const char *z = member(root, end, "timezone");
    if (z && *z == '"') {
        const char *ze = skip_str(z, end) - 1;
        snprintf(out->zone, sizeof out->zone, "%.*s", (int)(ze > z ? ze - z - 1 : 0), z + 1);
    }

    const char *c = member(root, end, "current");
    if (!c || *c != '{') return false;
    const char *ce = close_of(c, end);
    double v;
    if (!num(member(c, ce, "temperature_2m"), ce, &v)) return false;
    wx_now_t *w = &out->now;
    w->temp_c       = (float)v;
    w->t            = (int64_t)get(c, ce, "time", 0);
    w->feels_c      = (float)get(c, ce, "apparent_temperature", v);
    w->humidity     = pct(get(c, ce, "relative_humidity_2m", NAN));
    w->day          = get(c, ce, "is_day", 1) != 0;
    w->precip_mm    = (float)get(c, ce, "precipitation", 0);
    w->code         = wmo(get(c, ce, "weather_code", NAN));
    w->pressure_hpa = (float)get(c, ce, "pressure_msl", 0);
    w->wind_kmh     = (float)get(c, ce, "wind_speed_10m", 0);
    w->gust_kmh     = (float)get(c, ce, "wind_gusts_10m", 0);
    const double wd = get(c, ce, "wind_direction_10m", 0);
    w->wind_deg     = (uint16_t)((long)lround(wd < 0 ? 0 : wd) % 360);

    double t[WX_HOURS], a[WX_HOURS], b[WX_HOURS], k[WX_HOURS], d[WX_HOURS];
    const char *h = member(root, end, "hourly");
    if (h && *h == '{') {
        const char *he = close_of(h, end);
        int n = least(nums(h, he, "time", t, WX_HOURS), nums(h, he, "temperature_2m", a, WX_HOURS));
        const int nr = nums(h, he, "precipitation_probability", b, WX_HOURS);
        const int nc = nums(h, he, "weather_code", k, WX_HOURS);
        const int nd = nums(h, he, "is_day", d, WX_HOURS);
        for (int i = 0; i < n; i++) {
            if (isnan(t[i]) || isnan(a[i])) break;
            wx_hour_t *o = &out->hour[out->n_hours++];
            o->t        = (int64_t)t[i];
            o->temp_c   = (float)a[i];
            o->rain_pct = i < nr ? pct(b[i]) : 0;
            o->code     = i < nc ? wmo(k[i]) : 3;
            o->day      = i < nd ? d[i] != 0 : true;
        }
    }

    double dt[WX_DAYS], mx[WX_DAYS], mn[WX_DAYS], sr[WX_DAYS], ss[WX_DAYS], rp[WX_DAYS], dc[WX_DAYS];
    const char *dy = member(root, end, "daily");
    if (dy && *dy == '{') {
        const char *de = close_of(dy, end);
        int n = least(nums(dy, de, "time", dt, WX_DAYS),
                      least(nums(dy, de, "temperature_2m_max", mx, WX_DAYS),
                            nums(dy, de, "temperature_2m_min", mn, WX_DAYS)));
        const int nc = nums(dy, de, "weather_code", dc, WX_DAYS);
        const int nr = nums(dy, de, "precipitation_probability_max", rp, WX_DAYS);
        const int nsr = nums(dy, de, "sunrise", sr, WX_DAYS), nss = nums(dy, de, "sunset", ss, WX_DAYS);
        for (int i = 0; i < n; i++) {
            if (isnan(dt[i]) || isnan(mx[i]) || isnan(mn[i])) break;
            wx_day_t *o = &out->day[out->n_days++];
            o->t        = (int64_t)dt[i];
            o->max_c    = (float)mx[i];
            o->min_c    = (float)mn[i];
            o->code     = i < nc ? wmo(dc[i]) : 3;
            o->rain_pct = i < nr ? pct(rp[i]) : 0;
            o->sunrise  = i < nsr && !isnan(sr[i]) ? (int64_t)sr[i] : 0;
            o->sunset   = i < nss && !isnan(ss[i]) ? (int64_t)ss[i] : 0;
        }
    }
    return true;
}

/* ---- the WMO codes ---- */

uint8_t wx_icon(uint8_t c)
{
    switch (c) {
    case 0: case 1:                     return WX_I_CLEAR;
    case 2:                             return WX_I_PARTLY;
    case 45: case 48:                   return WX_I_FOG;
    case 51: case 53: case 55:          return WX_I_DRIZZLE;
    case 56: case 57: case 66: case 67: return WX_I_SLEET;
    case 61: case 63: case 65:          return WX_I_RAIN;
    case 71: case 73: case 75: case 77:
    case 85: case 86:                   return WX_I_SNOW;
    case 80: case 81: case 82:          return WX_I_SHOWERS;
    case 95: case 96: case 99:          return WX_I_THUNDER;
    default:                            return WX_I_CLOUDY;
    }
}

const char *wx_text(uint8_t c)
{
    switch (c) {
    case 0:  return "Clear sky";
    case 1:  return "Mainly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: return "Fog";
    case 48: return "Freezing fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Heavy drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: return "Snow showers";
    case 86: return "Heavy snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm, hail";
    default: return "--";
    }
}

/* ---- the units ---- */

bool wx_temp_unit_ok(long u) { return u >= 0 && u < WX_TEMP_UNITS; }
bool wx_wind_unit_ok(long u) { return u >= 0 && u < WX_WIND_UNITS; }

int wx_temp(float c, uint8_t unit)
{
    const float v = unit == WX_F ? c * 9.0f / 5.0f + 32.0f : c;
    return (int)lroundf(v);
}

int wx_wind(float kmh, uint8_t unit)
{
    if (kmh < 0) kmh = 0;
    switch (unit) {
    case WX_MS:  return (int)lroundf(kmh / 3.6f);
    case WX_KN:  return (int)lroundf(kmh / 1.852f);
    case WX_MPH: return (int)lroundf(kmh / 1.609344f);
    case WX_BFT: {
        /* Each force's least wind, km/h. */
        static const uint8_t FROM[12] = { 1, 6, 12, 20, 29, 39, 50, 62, 75, 89, 103, 118 };
        const long k = lroundf(kmh);
        int b = 0;
        while (b < 12 && k >= FROM[b]) b++;
        return b;
    }
    default:     return (int)lroundf(kmh);
    }
}

const char *wx_wind_unit(uint8_t unit)
{
    static const char *const U[WX_WIND_UNITS] = { "km/h", "m/s", "kn", "mph", "Bft" };
    return unit < WX_WIND_UNITS ? U[unit] : U[WX_KMH];
}

const char *wx_compass(uint16_t deg)
{
    static const char *const P[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                       "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
    return P[((deg % 360) * 2 + 22) / 45 % 16];
}
