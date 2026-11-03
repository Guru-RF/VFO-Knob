/* ADS-B Radar's arithmetic (components/vfo_core/src/adsb.c): a feed's
 * aircraft read without a tree -- both feeds' arrays, the fields that are
 * numbers or words, the ones nested or missing, nearest first and within
 * range, a cut-off feed refused; distance and bearing; the ranges in each
 * unit and the dial's steps, held at the ends; altitude bands; the filter,
 * the alerts and the emergencies; adsbdb's route; and the map's tables as
 * they are committed (components/adsb_client/tables), their runs near a
 * view and the airports by code and latitude. */
#include "tiny.h"
#include "adsb.h"

#include <math.h>

#ifndef ADSB_TABLES
#error "ADSB_TABLES: the directory of map.bin and airports.bin"
#endif

#define BRU_LAT 50.9014
#define BRU_LON 4.4844

static const char FEED[] =
    "{\"now\":1730640000.1,\"total\":5,\"msg\":\"No error\",\"ac\":["
    /* Near, climbing, a route-worthy callsign with its padding. */
    "{\"hex\":\"4ca7b5\",\"type\":\"adsb_icao\",\"flight\":\"RYR12AB \",\"r\":\"EI-DCL\",\"t\":\"B738\","
    "\"alt_baro\":12000,\"alt_geom\":12300,\"gs\":321.4,\"track\":271.9,\"baro_rate\":1536,"
    "\"squawk\":\"2215\",\"emergency\":\"none\",\"category\":\"A3\",\"nav_modes\":[\"autopilot\",\"tcas\"],"
    "\"lat\":50.95,\"lon\":4.40,\"mlat\":[],\"tisb\":[],\"seen_pos\":0.4,\"seen\":0.1,\"dbFlags\":0},"
    /* No position: counted, not kept. */
    "{\"hex\":\"3c6444\",\"flight\":\"DLH4AB\",\"alt_baro\":36000,\"seen\":12.0},"
    /* On the ground, military, its heading from true_heading. */
    "{\"hex\":\"ae1234\",\"flight\":\"RCH123\",\"t\":\"C17\",\"alt_baro\":\"ground\",\"gs\":12,"
    "\"true_heading\":90.4,\"lat\":50.90,\"lon\":4.49,\"dbFlags\":1,\"seen\":2},"
    /* An emergency, far, the geometric altitude only, a rate from geom_rate, escapes. */
    "{\"hex\":\"~abc\",\"flight\":\"BAW9\\\"X\",\"t\":\"A388\",\"alt_geom\":38000,\"geom_rate\":-2048,"
    "\"squawk\":\"7700\",\"emergency\":\"general\",\"lat\":51.47,\"lon\":-0.46,\"gs\":4.5e2},"
    /* Past the range asked for. */
    "{\"hex\":\"a00001\",\"flight\":\"UAL1\",\"alt_baro\":41000,\"lat\":40.6,\"lon\":-73.8,\"dbFlags\":2}"
    "],\"ctime\":1730640000,\"ptime\":12}";

static void test_parse(void)
{
    static adsb_ac_t a[8];
    int total = -1;

    CASE("the internet feeds' \"ac\": kept with a position, within range, nearest first");
    int n = adsb_parse(FEED, sizeof FEED - 1, BRU_LAT, BRU_LON, 250, a, 8, &total);
    CHECK_EQ(n, 3);
    CHECK_EQ(total, 5);
    CHECK_STR(a[0].hex, "ae1234");
    CHECK_STR(a[1].hex, "4ca7b5");
    CHECK_STR(a[2].hex, "~abc");

    CASE("the fields: words trimmed, numbers rounded, nested arrays stepped over");
    const adsb_ac_t *r = &a[1];
    CHECK_STR(r->flight, "RYR12AB");
    CHECK_STR(r->reg, "EI-DCL");
    CHECK_STR(r->type, "B738");
    CHECK_STR(r->squawk, "2215");
    CHECK_STR(r->emerg, "");
    CHECK_EQ(r->alt_ft, 12000);
    CHECK_EQ(r->gs_kt, 321);
    CHECK_EQ(r->track, 272);
    CHECK_EQ(r->vrate_fpm, 1536);
    CHECK(!r->ground);
    CHECK(fabsf(r->seen_s - 0.4f) < 1e-4f);
    CHECK(r->dist_nm > 3.5f && r->dist_nm < 4.5f);

    CASE("on the ground: altitude 0, the heading from true_heading, military");
    CHECK(a[0].ground);
    CHECK_EQ(a[0].alt_ft, 0);
    CHECK_EQ(a[0].track, 90);
    CHECK_EQ(a[0].db_flags, ADSB_DB_MILITARY);
    CHECK_EQ(a[0].vrate_fpm, ADSB_VR_NONE);

    CASE("the geometric altitude when there is no barometric; geom_rate; an escape; 4.5e2");
    CHECK_EQ(a[2].alt_ft, 38000);
    CHECK_EQ(a[2].vrate_fpm, -2048);
    CHECK_STR(a[2].flight, "BAW9\"X");
    CHECK_EQ(a[2].gs_kt, 450);
    CHECK_STR(a[2].emerg, "general");

    CASE("readsb's own \"aircraft\", and a cap that keeps the nearest");
    static const char LOCAL[] =
        "{ \"now\" : 1.5, \"messages\": 99, \"aircraft\" : [ "
        "{\"hex\":\"a\",\"lat\":51.0,\"lon\":4.4844},"
        "{\"hex\":\"b\",\"lat\":50.95,\"lon\":4.4844},"
        "{\"hex\":\"c\",\"lat\":51.2,\"lon\":4.4844} ] }";
    n = adsb_parse(LOCAL, sizeof LOCAL - 1, BRU_LAT, BRU_LON, 250, a, 2, &total);
    CHECK_EQ(n, 2);
    CHECK_EQ(total, 3);
    CHECK_STR(a[0].hex, "b");
    CHECK_STR(a[1].hex, "a");
    CHECK_EQ(a[0].alt_ft, ADSB_ALT_NONE);
    CHECK_EQ(a[0].track, -1);
    CHECK_EQ(a[0].gs_kt, -1);

    CASE("an empty feed, a cut-off one, something else");
    CHECK_EQ(adsb_parse("{\"ac\":[]}", 9, 0, 0, 10, a, 2, NULL), 0);
    CHECK_EQ(adsb_parse(FEED, 200, BRU_LAT, BRU_LON, 250, a, 8, NULL), -1);
    CHECK_EQ(adsb_parse("{\"msg\":\"rate limited\"}", 22, 0, 0, 10, a, 2, NULL), -1);
    CHECK_EQ(adsb_parse("<html>", 6, 0, 0, 10, a, 2, NULL), -1);
    CHECK_EQ(adsb_parse("", 0, 0, 0, 10, a, 2, NULL), -1);
}

static void test_geo(void)
{
    float nm, brg;
    CASE("Brussels to Heathrow: 189 NM at 282 degrees");
    adsb_dist_brg(BRU_LAT, BRU_LON, 51.4706, -0.461941, &nm, &brg);
    CHECK(fabsf(nm - 189.2f) < 0.2f);
    CHECK(fabsf(brg - 282.3f) < 0.2f);
    CASE("a tenth of a degree north is 6 NM at 0; east at 90");
    adsb_dist_brg(50.9, 4.5, 51.0, 4.5, &nm, &brg);
    CHECK(fabsf(nm - 6.0f) < 0.01f && brg < 0.01f);
    adsb_dist_brg(50.9, 4.5, 50.9, 4.6, &nm, &brg);
    CHECK(fabsf(nm - 3.79f) < 0.01f && fabsf(brg - 90.0f) < 0.1f);

    CASE("the view: 50 NM across 170 px; north up, east right, round the date line");
    adsb_view_t v;
    float x, y;
    adsb_view_set(&v, BRU_LAT, BRU_LON, 50, 170);
    adsb_project(&v, BRU_LAT + 50.0 / 60.0, BRU_LON, &x, &y);
    CHECK(fabsf(x) < 0.01f && fabsf(y + 170) < 0.01f);
    adsb_project(&v, BRU_LAT, BRU_LON + 0.5, &x, &y);
    CHECK(x > 60 && x < 66 && fabsf(y) < 0.01f);
    adsb_view_set(&v, 0, 179.9, 50, 170);
    adsb_project(&v, 0, -179.9, &x, &y);
    CHECK(x > 0 && x < 70);
}

static void test_feeds(void)
{
    char u[160];
    CASE("the internet feeds: their addresses, the radius in whole NM, 1 to 250");
    CHECK(adsb_feed_url(ADSB_SRC_AIRPLANES_LIVE, "", 50.9014, 4.4844, 49.2f, u, sizeof u));
    CHECK_STR(u, "https://api.airplanes.live/v2/point/50.9014/4.4844/50");
    CHECK(adsb_feed_url(ADSB_SRC_ADSB_LOL, NULL, -33.9, 151.2, 300, u, sizeof u));
    CHECK_STR(u, "https://api.adsb.lol/v2/point/-33.9000/151.2000/250");
    CHECK(adsb_feed_url(ADSB_SRC_ADSB_FI, "", 60.3172, 24.9633, 0.2f, u, sizeof u));
    CHECK_STR(u, "https://opendata.adsb.fi/api/v3/lat/60.3172/lon/24.9633/dist/1");

    CASE("a receiver of one's own: its address as given, or made whole");
    CHECK(adsb_feed_url(ADSB_SRC_LOCAL, "http://192.168.1.28:8080/data/aircraft.json", 0, 0, 50, u, sizeof u));
    CHECK_STR(u, "http://192.168.1.28:8080/data/aircraft.json");
    CHECK(adsb_feed_url(ADSB_SRC_LOCAL, " 192.168.1.28:8080 ", 0, 0, 50, u, sizeof u));
    CHECK_STR(u, "http://192.168.1.28:8080/data/aircraft.json");
    CHECK(adsb_feed_url(ADSB_SRC_LOCAL, "https://adsb.example.org", 0, 0, 50, u, sizeof u));
    CHECK_STR(u, "https://adsb.example.org/data/aircraft.json");
    CHECK(adsb_feed_url(ADSB_SRC_LOCAL, "pi.local/tar1090/data/aircraft.json", 0, 0, 50, u, sizeof u));
    CHECK_STR(u, "http://pi.local/tar1090/data/aircraft.json");
    CHECK(!adsb_feed_url(ADSB_SRC_LOCAL, "", 0, 0, 50, u, sizeof u));
    CHECK(!adsb_feed_url(ADSB_SRC_LOCAL, "   ", 0, 0, 50, u, sizeof u));
    CHECK(!adsb_feed_url(ADSB_SRC_LOCAL, NULL, 0, 0, 50, u, sizeof u));

    CASE("too long to fit; their names; an unknown feed is adsb.fi");
    CHECK(!adsb_feed_url(ADSB_SRC_AIRPLANES_LIVE, "", 50.9, 4.4, 50, u, 20));
    CHECK_STR(adsb_src_name(ADSB_SRC_LOCAL), "your receiver");
    CHECK_STR(adsb_src_name(9), "adsb.fi");
    CHECK(adsb_src_ok(3) && !adsb_src_ok(4));
    CHECK(adsb_feed_url(9, "", 1, 2, 10, u, sizeof u));
    CHECK_STR(u, "https://opendata.adsb.fi/api/v3/lat/1.0000/lon/2.0000/dist/10");
}

static void test_ranges(void)
{
    CASE("ten ranges a unit, 250 NM at the most");
    CHECK_EQ(adsb_range_val(ADSB_NM, 0), 5);
    CHECK_EQ(adsb_range_val(ADSB_NM, 9), 250);
    CHECK_EQ(adsb_range_val(ADSB_KM, 4), 100);
    CHECK_EQ(adsb_range_val(ADSB_MI, 9), 250);
    for (uint8_t u = 0; u < ADSB_UNITS; u++)
        for (uint8_t i = 0; i < ADSB_RANGES; i++) {
            CHECK(adsb_range_nm(u, i) <= 250.0f);
            if (i) CHECK(adsb_range_val(u, i) > adsb_range_val(u, i - 1));
        }
    CHECK(fabsf(adsb_range_nm(ADSB_KM, 4) - 54.0f) < 0.1f);
    CHECK(fabsf(adsb_nm_in(ADSB_MI, 100) - 115.08f) < 0.01f);
    CHECK_STR(adsb_unit_name(ADSB_KM), "km");
    CHECK_STR(adsb_unit_name(7), "NM");
    CHECK(adsb_units_ok(2) && !adsb_units_ok(3) && !adsb_units_ok(-1));

    CASE("the dial: a range a detent, held at either end");
    CHECK_EQ(adsb_range_step(4, 1), 5);
    CHECK_EQ(adsb_range_step(4, -3), 1);
    CHECK_EQ(adsb_range_step(1, -5), 0);
    CHECK_EQ(adsb_range_step(8, 4), 9);
    CHECK_EQ(adsb_range_step(42, 0), 9);
}

static void test_bands_and_alerts(void)
{
    adsb_ac_t a;
    memset(&a, 0, sizeof a);

    CASE("the altitude bands");
    a.alt_ft = ADSB_ALT_NONE;                      CHECK_EQ(adsb_band(&a), ADSB_BAND_GROUND);
    a.alt_ft = 1999;                               CHECK_EQ(adsb_band(&a), ADSB_BAND_2K);
    a.alt_ft = 2000;                               CHECK_EQ(adsb_band(&a), ADSB_BAND_10K);
    a.alt_ft = 29999;                              CHECK_EQ(adsb_band(&a), ADSB_BAND_30K);
    a.alt_ft = 40000;                              CHECK_EQ(adsb_band(&a), ADSB_BAND_HIGH);
    a.ground = true;                               CHECK_EQ(adsb_band(&a), ADSB_BAND_GROUND);
    a.ground = false;

    CASE("a type list: commas or spaces, any case, a trailing * for a family");
    CHECK(adsb_type_listed("C17, a400  EUFI", "A400"));
    CHECK(adsb_type_listed("c17", "C17"));
    CHECK(adsb_type_listed("B74*", "B748"));
    CHECK(!adsb_type_listed("B74*", "B7"));
    CHECK(!adsb_type_listed("C17", "C172"));
    CHECK(!adsb_type_listed("", "C17") && !adsb_type_listed("C17", "") && !adsb_type_listed(NULL, "C17"));

    CASE("emergencies: the squawks and the feed's words");
    strcpy(a.squawk, "7700");  CHECK_STR(adsb_emergency_name(&a), "EMERGENCY");
    strcpy(a.squawk, "7600");  CHECK_STR(adsb_emergency_name(&a), "NO RADIO");
    strcpy(a.squawk, "7500");  CHECK_STR(adsb_emergency_name(&a), "HIJACK");
    strcpy(a.squawk, "7000");  CHECK_STR(adsb_emergency_name(&a), "");
    strcpy(a.emerg, "minfuel"); CHECK_STR(adsb_emergency_name(&a), "MINIMUM FUEL");
    strcpy(a.emerg, "lifeguard"); CHECK_STR(adsb_emergency_name(&a), "MEDICAL");
    strcpy(a.emerg, "whatever"); CHECK_STR(adsb_emergency_name(&a), "EMERGENCY");
    a.emerg[0] = 0;

    CASE("what is shown: the filter, the ground, and alerts over both");
    strcpy(a.type, "A320");
    a.alt_ft = 30000;
    CHECK(adsb_shown(&a, ADSB_SHOW_ALL, false, ""));
    CHECK(!adsb_shown(&a, ADSB_SHOW_MILITARY, false, ""));
    CHECK(adsb_shown(&a, ADSB_SHOW_MILITARY, false, "A32*"));
    CHECK_EQ(adsb_alert(&a, "A32*"), ADSB_AL_TYPE);
    a.db_flags = ADSB_DB_MILITARY;
    CHECK(adsb_shown(&a, ADSB_SHOW_MILITARY, false, ""));
    CHECK(!adsb_shown(&a, ADSB_SHOW_INTERESTING, false, ""));
    a.ground = true;
    CHECK(!adsb_shown(&a, ADSB_SHOW_ALL, false, ""));
    CHECK(!adsb_shown(&a, ADSB_SHOW_ALL, false, "A320"));
    CHECK(adsb_shown(&a, ADSB_SHOW_ALL, true, ""));
    strcpy(a.squawk, "7700");
    CHECK(adsb_shown(&a, ADSB_SHOW_INTERESTING, false, ""));
    CHECK_EQ(adsb_alert(&a, "A320"), ADSB_AL_EMERGENCY);
    CHECK(adsb_show_ok(2) && !adsb_show_ok(3));
}

static void test_route(void)
{
    char from[24], to[24];
    CASE("adsbdb: the towns, else the codes");
    static const char R[] =
        "{\"response\":{\"flightroute\":{\"callsign\":\"BAW123\",\"airline\":{\"name\":\"British Airways\","
        "\"icao\":\"BAW\"},\"origin\":{\"country_name\":\"United Kingdom\",\"elevation\":83,\"iata_code\":"
        "\"LHR\",\"icao_code\":\"EGLL\",\"latitude\":51.4706,\"municipality\":\"London\",\"name\":"
        "\"London Heathrow Airport\"},\"destination\":{\"iata_code\":\"JFK\",\"icao_code\":\"KJFK\","
        "\"municipality\":\"\"}}}}";
    CHECK(adsb_route_parse(R, sizeof R - 1, from, sizeof from, to, sizeof to));
    CHECK_STR(from, "London");
    CHECK_STR(to, "JFK");
    CASE("a long town cut to fit");
    static const char L[] =
        "{\"response\":{\"flightroute\":{\"origin\":{\"municipality\":\"Llanfairpwllgwyngyll-on-Sea\"},"
        "\"destination\":{\"icao_code\":\"EBBR\"}}}}";
    char small[8];
    CHECK(adsb_route_parse(L, sizeof L - 1, small, sizeof small, to, sizeof to));
    CHECK_STR(small, "Llanfai");
    CHECK_STR(to, "EBBR");
    CASE("an unknown callsign; nothing at all");
    static const char U[] = "{\"response\":\"unknown callsign\"}";
    CHECK(!adsb_route_parse(U, sizeof U - 1, from, sizeof from, to, sizeof to));
    CHECK_STR(from, "");
    CHECK(!adsb_route_parse("", 0, from, sizeof from, to, sizeof to));
}

static uint8_t *load(const char *name, size_t *n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", ADSB_TABLES, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*n);
    if (p && fread(p, 1, *n, f) != *n) { free(p); p = NULL; }
    fclose(f);
    return p;
}

static void test_tables(void)
{
    size_t mn = 0, an = 0;
    uint8_t *m = load("map.bin", &mn), *ap = load("airports.bin", &an);

    CASE("the committed tables are whole");
    CHECK(m && adsb_map_ok(m, mn));
    CHECK(ap && adsb_apt_ok(ap, an));
    if (!m || !ap) { free(m); free(ap); return; }
    CHECK(!adsb_map_ok(m, mn - 1));
    CHECK(!adsb_apt_ok(ap, an + 16));
    CHECK(adsb_map_runs(m) > 1000);
    CHECK(adsb_apt_count(ap) > 4000);

    CASE("the airports: by code, sorted by latitude");
    long i = adsb_apt_find(ap, "ebbr");
    CHECK(i >= 0);
    adsb_apt_t a;
    adsb_apt_get(ap, (size_t)i, &a);
    CHECK_STR(a.code, "EBBR");
    CHECK(a.large);
    CHECK(abs(a.lat - 509014) < 10 && abs(a.lon - 44844) < 10);
    CHECK_EQ(adsb_apt_find(ap, "ZZZZ"), -1);
    CHECK_EQ(adsb_apt_find(ap, "EBB"), -1);
    size_t k = adsb_apt_from(ap, 500000);
    adsb_apt_t b;
    adsb_apt_get(ap, k, &b);
    CHECK(b.lat >= 500000);
    if (k) { adsb_apt_get(ap, k - 1, &b); CHECK(b.lat < 500000); }
    CHECK_EQ(adsb_apt_from(ap, 99 * 10000), adsb_apt_count(ap));

    CASE("the map round Brussels at 50 NM: the coast and the borders, and only near ones");
    adsb_view_t v;
    adsb_view_set(&v, BRU_LAT, BRU_LON, 50, 170);
    size_t near = 0, coast = 0, border = 0, pts_in = 0;
    for (size_t r = 0; r < adsb_map_runs(m); r++) {
        adsb_run_t run;
        if (!adsb_map_run(m, r, &v, 50, &run)) continue;
        near++;
        if (run.kind == ADSB_RUN_COAST) coast++; else border++;
        int32_t la = run.lat, lo = run.lon;
        for (uint16_t s = 0; s <= run.steps; s++) {
            float x, y;
            adsb_project(&v, la / 1e4, lo / 1e4, &x, &y);
            if (x * x + y * y <= 170.0f * 170.0f) pts_in++;
            if (s == run.steps) break;
            int32_t dla, dlo;
            adsb_run_step(&run, s, &dla, &dlo);
            la += dla;
            lo += dlo;
        }
    }
    CHECK(near > 0 && near < 60);
    CHECK(coast > 0 && border > 0);
    CHECK(pts_in > 20);
    free(m);
    free(ap);
}

int main(void)
{
    test_parse();
    test_geo();
    test_feeds();
    test_ranges();
    test_bands_and_alerts();
    test_route();
    test_tables();
    printf("%d checks, %d failed\n", t_run, t_fail);
    return t_fail ? 1 : 0;
}
