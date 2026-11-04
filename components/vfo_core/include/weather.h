/* The Weather firmware's arithmetic (VFO_RADIO=weather): Open-Meteo's
 * forecast asked for and read without a tree, the WMO weather codes as an
 * icon and a few words, and the units the page offers. Plain C, so
 * test/host checks it on the PC (test_weather). Weather data by
 * Open-Meteo.com, CC BY 4.0. */
#ifndef WEATHER_H
#define WEATHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WX_HOURS 24             /* the next hours asked for, from this one */
#define WX_DAYS  7              /* the days, today the first */

/* The icons the face draws (ui.c), each from shapes of its own. */
enum {
    WX_I_CLEAR,                 /* the sun, or by night the moon */
    WX_I_PARTLY,                /* ...half behind a cloud */
    WX_I_CLOUDY,
    WX_I_FOG,
    WX_I_DRIZZLE,
    WX_I_RAIN,
    WX_I_SLEET,                 /* freezing drizzle or rain */
    WX_I_SNOW,
    WX_I_SHOWERS,               /* the sun, a cloud and rain */
    WX_I_THUNDER,
    WX_ICONS
};

typedef struct {
    int64_t  t;                 /* the reading's time, Unix */
    float    temp_c, feels_c;
    float    wind_kmh, gust_kmh;
    float    pressure_hpa, precip_mm;
    uint16_t wind_deg;          /* where the wind comes from */
    uint8_t  humidity;          /* % */
    uint8_t  code;              /* WMO */
    bool     day;
} wx_now_t;

typedef struct {
    int64_t t;
    float   temp_c;
    uint8_t rain_pct;           /* the chance of rain, % */
    uint8_t code;
    bool    day;
} wx_hour_t;

typedef struct {
    int64_t t;                  /* the day's local midnight, Unix */
    int64_t sunrise, sunset;
    float   max_c, min_c;
    uint8_t rain_pct;           /* the day's highest chance */
    uint8_t code;
} wx_day_t;

typedef struct {
    wx_now_t  now;
    wx_hour_t hour[WX_HOURS];
    wx_day_t  day[WX_DAYS];
    int       n_hours, n_days;
    int32_t   utc_offset_s;     /* the place's, now */
    char      zone[40];         /* its IANA name: "Europe/Brussels" */
} wx_t;

/* The forecast's address for a place, in degrees. Its length, 0 if n is
 * too small. */
size_t wx_url(double lat, double lon, char *out, size_t n);

/* A forecast read: true with out filled when the current weather is in it;
 * the hours and days as many as came. js is NUL-terminated at len. */
bool wx_parse(const char *js, size_t len, wx_t *out);

/* A WMO code's icon (WX_I_*) and its words: "Partly cloudy". Unknown codes
 * are cloudy, and "--". */
uint8_t     wx_icon(uint8_t code);
const char *wx_text(uint8_t code);

/* The page's units. */
enum { WX_C, WX_F, WX_TEMP_UNITS };
enum { WX_KMH, WX_MS, WX_KN, WX_MPH, WX_BFT, WX_WIND_UNITS };
bool        wx_temp_unit_ok(long u);
bool        wx_wind_unit_ok(long u);
/* A temperature in the unit, rounded: never "-0". */
int         wx_temp(float c, uint8_t unit);
/* A wind in the unit, rounded; Beaufort's force 0 to 12. */
int         wx_wind(float kmh, uint8_t unit);
const char *wx_wind_unit(uint8_t unit);   /* "km/h", "m/s", "kn", "mph", "Bft" */
/* Where the wind comes from, of 16 points: "NNE". */
const char *wx_compass(uint16_t deg);

#endif /* WEATHER_H */
