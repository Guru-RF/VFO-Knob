/* The Weather firmware's arithmetic (components/vfo_core/src/weather.c): the
 * forecast's address; a reply in Open-Meteo's shape read -- its units'
 * objects carry the same keys, a chance of rain may be null -- and every
 * prefix of it read without a fault; what is not a forecast refused; the
 * WMO codes' icons and words; the units, Beaufort and the compass. The reply
 * is made up, in the shape api.open-meteo.com answered on 2026-11-04. */
#include "tiny.h"
#include "weather.h"

#include <math.h>
#include <stdlib.h>

static const char REPLY[] =
    "{\"latitude\":50.854,\"longitude\":4.35,\"generationtime_ms\":0.37,\"utc_offset_seconds\":3600,"
    "\"timezone\":\"Europe/Brussels\",\"timezone_abbreviation\":\"GMT+1\",\"elevation\":27.0,"
    "\"current_units\":{\"time\":\"unixtime\",\"interval\":\"seconds\",\"temperature_2m\":\"\xC2\xB0" "C\","
    "\"relative_humidity_2m\":\"%\",\"weather_code\":\"wmo code\"},"
    "\"current\":{\"time\":1793801700,\"interval\":900,\"temperature_2m\":-2.6,\"relative_humidity_2m\":63,"
    "\"apparent_temperature\":-7.4,\"is_day\":0,\"precipitation\":0.20,\"weather_code\":71,"
    "\"pressure_msl\":1027.4,\"wind_speed_10m\":19.1,\"wind_direction_10m\":58,\"wind_gusts_10m\":38.5},"
    "\"hourly_units\":{\"time\":\"unixtime\",\"temperature_2m\":\"\xC2\xB0" "C\"},"
    "\"hourly\":{\"time\":[1793800800,1793804400,1793808000,1793811600,1793815200,1793818800,"
    "1793822400,1793826000,1793829600,1793833200,1793836800,1793840400,1793844000,1793847600,"
    "1793851200,1793854800,1793858400,1793862000,1793865600,1793869200,1793872800,1793876400,"
    "1793880000,1793883600],"
    "\"temperature_2m\":[9.6,9.9,9.5,8.8,7.8,7.2,6.7,6.2,5.7,5.4,5.0,4.7,4.5,4.1,3.7,3.4,3.5,3.3,4.0,"
    "5.3,6.9,8.6,10.0,11.0],"
    "\"precipitation_probability\":[0,5,null,10,20,35,60,80,75,40,10,0,0,0,0,0,0,0,0,0,0,0,0,0],"
    "\"weather_code\":[3,0,0,2,3,61,63,65,80,51,3,0,0,0,0,0,0,0,0,0,0,0,0,0],"
    "\"is_day\":[1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1]},"
    "\"daily_units\":{\"time\":\"unixtime\",\"sunrise\":\"unixtime\"},"
    "\"daily\":{\"time\":[1793746800,1793833200,1793919600,1794006000,1794092400,1794178800,1794265200],"
    "\"weather_code\":[3,0,3,51,3,3,95],\"temperature_2m_max\":[9.9,11.6,10.1,8.3,10.1,8.6,10.6],"
    "\"temperature_2m_min\":[4.7,3.3,4.4,2.7,4.2,1.4,-0.4],"
    "\"sunrise\":[1793774364,1793860869,1793947373,1794033877,1794120381,1794206885,1794293388],"
    "\"sunset\":[1793808740,1793895039,1793981340,1794067642,1794153947,1794240253,1794326561],"
    "\"precipitation_probability_max\":[0,0,15,6,16,16,42]}}";

static bool near(double a, double b) { return fabs(a - b) < 1e-3; }

static void test_url(void)
{
    char u[600];
    CASE("the address: the place to four decimals, 24 hours and 7 days, Unix times");
    const size_t n = wx_url(50.8503, 4.3517, u, sizeof u);
    CHECK(n > 0 && n == strlen(u));
    static const char HEAD[] = "https://api.open-meteo.com/v1/forecast?latitude=50.8503&longitude=4.3517&";
    CHECK(!strncmp(u, HEAD, sizeof HEAD - 1));
    CHECK(strstr(u, "&forecast_hours=24") && strstr(u, "&forecast_days=7") && strstr(u, "&timeformat=unixtime"));
    CHECK(strstr(u, "&timezone=auto") && strstr(u, "precipitation_probability_max"));
    CHECK(wx_url(-33.8688, 151.2093, u, sizeof u) && strstr(u, "latitude=-33.8688&longitude=151.2093"));
    CASE("...too long for the buffer: none");
    CHECK_EQ(wx_url(50.85, 4.35, u, 64), 0);
}

static void test_parse(void)
{
    wx_t w;
    CASE("the reply: now, as it is, the units' objects passed over");
    CHECK(wx_parse(REPLY, sizeof REPLY - 1, &w));
    CHECK(near(w.now.temp_c, -2.6) && near(w.now.feels_c, -7.4));
    CHECK_EQ(w.now.t, 1793801700);
    CHECK_EQ(w.now.humidity, 63);
    CHECK_EQ(w.now.code, 71);
    CHECK(!w.now.day);
    CHECK(near(w.now.precip_mm, 0.2) && near(w.now.pressure_hpa, 1027.4));
    CHECK(near(w.now.wind_kmh, 19.1) && near(w.now.gust_kmh, 38.5));
    CHECK_EQ(w.now.wind_deg, 58);
    CHECK_EQ(w.utc_offset_s, 3600);
    CHECK_STR(w.zone, "Europe/Brussels");

    CASE("...the next 24 hours, a null chance of rain none");
    CHECK_EQ(w.n_hours, 24);
    CHECK_EQ(w.hour[0].t, 1793800800);
    CHECK_EQ(w.hour[23].t, 1793883600);
    CHECK(near(w.hour[23].temp_c, 11.0));
    CHECK_EQ(w.hour[2].rain_pct, 0);
    CHECK_EQ(w.hour[7].rain_pct, 80);
    CHECK_EQ(w.hour[7].code, 65);
    CHECK(w.hour[0].day && !w.hour[3].day && w.hour[17].day);

    CASE("...the 7 days, today the first");
    CHECK_EQ(w.n_days, 7);
    CHECK_EQ(w.day[0].t, 1793746800);
    CHECK(near(w.day[1].max_c, 11.6) && near(w.day[6].min_c, -0.4));
    CHECK_EQ(w.day[6].code, 95);
    CHECK_EQ(w.day[6].rain_pct, 42);
    CHECK_EQ(w.day[0].sunrise, 1793774364);
    CHECK_EQ(w.day[0].sunset, 1793808740);
}

static void test_cut(void)
{
    CASE("every prefix of the reply: read without a fault, never more than it holds");
    const size_t n = sizeof REPLY - 1;
    char *b = malloc(n + 1);
    int whole = 0;
    for (size_t len = 0; len <= n; len++) {
        memcpy(b, REPLY, len);
        b[len] = 0;
        wx_t w;
        const bool ok = wx_parse(b, len, &w);
        CHECK(w.n_hours >= 0 && w.n_hours <= WX_HOURS && w.n_days >= 0 && w.n_days <= WX_DAYS);
        if (ok && w.n_hours == WX_HOURS && w.n_days == WX_DAYS) whole++;
        if (len < 300) CHECK(!ok);                     /* before "current" is whole */
    }
    CHECK(whole >= 1);
    free(b);

    CASE("not a forecast: refused");
    wx_t w;
    CHECK(!wx_parse("", 0, &w));
    CHECK(!wx_parse("[1,2,3]", 7, &w));
    static const char ERR[] = "{\"error\":true,\"reason\":\"Latitude must be in range of -90 to 90\xC2\xB0.\"}";
    CHECK(!wx_parse(ERR, sizeof ERR - 1, &w));
    static const char NOCUR[] = "{\"current_units\":{\"temperature_2m\":\"C\"},\"current\":{\"time\":1}}";
    CHECK(!wx_parse(NOCUR, sizeof NOCUR - 1, &w));
    static const char INNER[] = "{\"hourly\":{\"current\":{\"temperature_2m\":5}}}";
    CHECK(!wx_parse(INNER, sizeof INNER - 1, &w));    /* a "current" further in is not now */
    CHECK(!wx_parse(NULL, 0, &w));
}

static void test_codes(void)
{
    CASE("the WMO codes: an icon and words for each, cloudy and -- for the unknown");
    for (int c = 0; c <= 99; c++) {
        CHECK(wx_icon((uint8_t)c) < WX_ICONS);
        CHECK(wx_text((uint8_t)c) && wx_text((uint8_t)c)[0]);
    }
    CHECK_EQ(wx_icon(0), WX_I_CLEAR);
    CHECK_EQ(wx_icon(1), WX_I_CLEAR);
    CHECK_EQ(wx_icon(2), WX_I_PARTLY);
    CHECK_EQ(wx_icon(3), WX_I_CLOUDY);
    CHECK_EQ(wx_icon(48), WX_I_FOG);
    CHECK_EQ(wx_icon(53), WX_I_DRIZZLE);
    CHECK_EQ(wx_icon(57), WX_I_SLEET);
    CHECK_EQ(wx_icon(66), WX_I_SLEET);
    CHECK_EQ(wx_icon(65), WX_I_RAIN);
    CHECK_EQ(wx_icon(77), WX_I_SNOW);
    CHECK_EQ(wx_icon(86), WX_I_SNOW);
    CHECK_EQ(wx_icon(81), WX_I_SHOWERS);
    CHECK_EQ(wx_icon(99), WX_I_THUNDER);
    CHECK_EQ(wx_icon(42), WX_I_CLOUDY);
    CHECK_STR(wx_text(2), "Partly cloudy");
    CHECK_STR(wx_text(96), "Thunderstorm, hail");
    CHECK_STR(wx_text(42), "--");
}

static void test_units(void)
{
    CASE("temperatures: rounded, never -0; Fahrenheit");
    CHECK_EQ(wx_temp(9.7f, WX_C), 10);
    CHECK_EQ(wx_temp(-2.6f, WX_C), -3);
    CHECK_EQ(wx_temp(-0.4f, WX_C), 0);
    CHECK_EQ(wx_temp(0.0f, WX_F), 32);
    CHECK_EQ(wx_temp(-40.0f, WX_F), -40);
    CHECK_EQ(wx_temp(37.0f, WX_F), 99);
    CHECK(wx_temp_unit_ok(WX_C) && wx_temp_unit_ok(WX_F) && !wx_temp_unit_ok(2) && !wx_temp_unit_ok(-1));

    CASE("winds: km/h, m/s, knots, mph");
    CHECK_EQ(wx_wind(36.0f, WX_KMH), 36);
    CHECK_EQ(wx_wind(36.0f, WX_MS), 10);
    CHECK_EQ(wx_wind(36.0f, WX_KN), 19);
    CHECK_EQ(wx_wind(36.0f, WX_MPH), 22);
    CHECK_EQ(wx_wind(-3.0f, WX_KMH), 0);
    CHECK_STR(wx_wind_unit(WX_KN), "kn");
    CHECK_STR(wx_wind_unit(9), "km/h");
    CHECK(wx_wind_unit_ok(WX_BFT) && !wx_wind_unit_ok(WX_WIND_UNITS));

    CASE("Beaufort: each force from its least wind, 12 from 118 km/h on");
    static const struct { float kmh; int b; } B[] = {
        { 0.4f, 0 }, { 1.0f, 1 }, { 5.4f, 1 }, { 6.0f, 2 }, { 19.1f, 3 }, { 20.0f, 4 }, { 38.4f, 5 },
        { 38.5f, 6 }, { 61.0f, 7 }, { 62.0f, 8 }, { 88.0f, 9 }, { 102.0f, 10 }, { 117.0f, 11 },
        { 118.0f, 12 }, { 250.0f, 12 },
    };
    for (size_t i = 0; i < sizeof B / sizeof *B; i++) CHECK_EQ(wx_wind(B[i].kmh, WX_BFT), B[i].b);

    CASE("the compass: 16 points, each its half-step either side");
    CHECK_STR(wx_compass(0), "N");
    CHECK_STR(wx_compass(11), "N");
    CHECK_STR(wx_compass(12), "NNE");
    CHECK_STR(wx_compass(58), "ENE");
    CHECK_STR(wx_compass(180), "S");
    CHECK_STR(wx_compass(225), "SW");
    CHECK_STR(wx_compass(348), "NNW");
    CHECK_STR(wx_compass(349), "N");
    CHECK_STR(wx_compass(720), "N");
}

int main(void)
{
    test_url();
    test_parse();
    test_cut();
    test_codes();
    test_units();
    printf("%d checks, %d failed\n", t_run, t_fail);
    return t_fail ? 1 : 0;
}
