/* AIS Radar's arithmetic (components/vfo_core/src/ais.c): AISStream.io's
 * messages read without a tree -- positions of either class, a ship's
 * static data and a class B's two parts, an aid, the confirmation and an
 * error, whatever order their members come in, the "not available" values
 * left unknown; what a message says laid over what was known; the
 * subscription and its box; an AIS-catcher's ship list, nearest first and
 * within range; the kinds, the names, the alerts and the watch list; the
 * ranges; and the harbours' table as committed (components/ais_client/
 * tables). Ships and numbers are made up. */
#include "tiny.h"
#include "ais.h"
#include "adsb.h"

#include <math.h>

#ifndef AIS_TABLES
#error "AIS_TABLES: the directory of ports.bin"
#endif

#define MID_LAT 51.160
#define MID_LON 2.760

static int parse(const char *js, ais_ship_t *u, uint32_t *f)
{
    bool c = false;
    char why[64];
    return ais_stream_parse(js, strlen(js), u, f, &c, why, sizeof why);
}

static void test_stream(void)
{
    ais_ship_t u;
    uint32_t f = 0;
    bool comp = true;
    char why[64];

    CASE("a class A position, Message before its type, the stream's own name");
    static const char POS_A[] =
        "{\"Message\":{\"PositionReport\":{\"Cog\":308.4,\"CommunicationState\":81982,\"Latitude\":51.2101,"
        "\"Longitude\":2.8702,\"MessageID\":1,\"NavigationalStatus\":0,\"PositionAccuracy\":true,\"Raim\":false,"
        "\"RateOfTurn\":4,\"RepeatIndicator\":0,\"Sog\":12.3,\"Spare\":0,\"SpecialManoeuvreIndicator\":0,"
        "\"Timestamp\":31,\"TrueHeading\":305,\"UserID\":205123000,\"Valid\":true}},"
        "\"MessageType\":\"PositionReport\",\"MetaData\":{\"MMSI\":205123000,\"MMSI_String\":205123000,"
        "\"ShipName\":\"NORDIC STAR         \",\"latitude\":51.2101,\"longitude\":2.8702,"
        "\"time_utc\":\"2026-11-21 10:22:32.318353 +0000 UTC\"}}";
    CHECK_EQ(parse(POS_A, &u, &f), AIS_MSG_SHIP);
    CHECK_EQ(u.mmsi, 205123000);
    CHECK(u.pos);
    CHECK(fabs(u.lat - 51.2101) < 1e-9 && fabs(u.lon - 2.8702) < 1e-9);
    CHECK_EQ(u.sog_dk, 123);
    CHECK_EQ(u.cog_dd, 3084);
    CHECK_EQ(u.hdg, 305);
    CHECK_EQ(u.nav, 0);
    CHECK_STR(u.name, "NORDIC STAR");
    CHECK_EQ(u.cls, AIS_CLS_A);
    CHECK_EQ(f & (AIS_F_POS | AIS_F_SOG | AIS_F_COG | AIS_F_HDG | AIS_F_NAV | AIS_F_NAME | AIS_F_CLS),
             AIS_F_POS | AIS_F_SOG | AIS_F_COG | AIS_F_HDG | AIS_F_NAV | AIS_F_NAME | AIS_F_CLS);
    CHECK(!(f & (AIS_F_TYPE | AIS_F_DIM | AIS_F_DEST)));

    CASE("a class B, its type first, nothing available: unknown, not nought");
    static const char POS_B[] =
        "{\"MessageType\":\"StandardClassBPositionReport\",\"MetaData\":{\"MMSI\":244000111,\"ShipName\":\"\"},"
        "\"Message\":{\"StandardClassBPositionReport\":{\"Cog\":360,\"Latitude\":91,\"Longitude\":181,"
        "\"Sog\":102.3,\"TrueHeading\":511,\"UserID\":244000111,\"Valid\":true}}}";
    CHECK_EQ(parse(POS_B, &u, &f), AIS_MSG_SHIP);
    CHECK_EQ(u.mmsi, 244000111);
    CHECK(!u.pos);
    CHECK(!(f & AIS_F_POS));
    CHECK_EQ(u.sog_dk, -1);
    CHECK_EQ(u.cog_dd, -1);
    CHECK_EQ(u.hdg, -1);
    CHECK_EQ(u.cls, AIS_CLS_B);
    CHECK(!(f & AIS_F_NAME));

    CASE("static data: name, call sign, type, size, destination, ETA, draught, IMO");
    static const char STATIC[] =
        "{\"Message\":{\"ShipStaticData\":{\"AisVersion\":2,\"CallSign\":\"ONAB   \",\"Destination\":\"BE ZEE@@@@@@\","
        "\"Dimension\":{\"A\":150,\"B\":30,\"C\":14,\"D\":14},\"Dte\":false,\"Eta\":{\"Day\":22,\"Hour\":6,"
        "\"Minute\":30,\"Month\":11},\"FixType\":1,\"ImoNumber\":9123456,\"MaximumStaticDraught\":7.4,"
        "\"MessageID\":5,\"Name\":\"NORDIC STAR@@@@@@@@@\",\"RepeatIndicator\":0,\"Spare\":false,"
        "\"Type\":70,\"UserID\":205123000,\"Valid\":true}},\"MessageType\":\"ShipStaticData\","
        "\"MetaData\":{\"MMSI\":205123000,\"ShipName\":\"OLD NAME\",\"latitude\":51.2,\"longitude\":2.8}}";
    CHECK_EQ(parse(STATIC, &u, &f), AIS_MSG_SHIP);
    CHECK_STR(u.name, "NORDIC STAR");
    CHECK_STR(u.callsign, "ONAB");
    CHECK_STR(u.dest, "BE ZEE");
    CHECK_EQ(u.type, 70);
    CHECK_EQ(u.len_m, 180);
    CHECK_EQ(u.beam_m, 28);
    CHECK_EQ(u.imo, 9123456);
    CHECK_EQ(u.draught_dm, 74);
    CHECK_EQ(u.eta_mon, 11);
    CHECK_EQ(u.eta_day, 22);
    CHECK_EQ(u.eta_h, 6);
    CHECK_EQ(u.eta_m, 30);
    CHECK(!u.pos);
    CHECK(!(f & AIS_F_POS));               /* the stream's own position is not taken */
    CHECK(f & AIS_F_ETA);

    CASE("a class B's part A names it; its part B gives the rest");
    static const char PART_A[] =
        "{\"MessageType\":\"StaticDataReport\",\"MetaData\":{\"MMSI\":244000111},\"Message\":{\"StaticDataReport\":"
        "{\"MessageID\":24,\"PartNumber\":false,\"ReportA\":{\"Name\":\"SEA SPRITE\",\"Valid\":true},"
        "\"ReportB\":{\"CallSign\":\"\",\"Dimension\":{\"A\":0,\"B\":0,\"C\":0,\"D\":0},\"ShipType\":0,"
        "\"Valid\":false,\"VenderIDModel\":0},\"UserID\":244000111,\"Valid\":true}}}";
    CHECK_EQ(parse(PART_A, &u, &f), AIS_MSG_SHIP);
    CHECK_STR(u.name, "SEA SPRITE");
    CHECK(f & AIS_F_NAME);
    CHECK(!(f & (AIS_F_TYPE | AIS_F_CALL | AIS_F_DIM)));
    static const char PART_B[] =
        "{\"MessageType\":\"StaticDataReport\",\"MetaData\":{\"MMSI\":244000111},\"Message\":{\"StaticDataReport\":"
        "{\"MessageID\":24,\"PartNumber\":true,\"ReportA\":{\"Name\":\"\",\"Valid\":false},"
        "\"ReportB\":{\"CallSign\":\"PD1234\",\"Dimension\":{\"A\":8,\"B\":4,\"C\":2,\"D\":2},\"ShipType\":36,"
        "\"Valid\":true},\"UserID\":244000111,\"Valid\":true}}}";
    CHECK_EQ(parse(PART_B, &u, &f), AIS_MSG_SHIP);
    CHECK(!(f & AIS_F_NAME));
    CHECK_STR(u.callsign, "PD1234");
    CHECK_EQ(u.type, 36);
    CHECK_EQ(u.len_m, 12);
    CHECK_EQ(u.beam_m, 4);

    CASE("an aid to navigation: its Type is no ship type");
    static const char ATON[] =
        "{\"MessageType\":\"AidsToNavigationReport\",\"MetaData\":{\"MMSI\":992051234},\"Message\":"
        "{\"AidsToNavigationReport\":{\"Name\":\"WESTHINDER\",\"Type\":9,\"Latitude\":51.38,\"Longitude\":2.43,"
        "\"UserID\":992051234,\"VirtualAtoN\":false}}}";
    CHECK_EQ(parse(ATON, &u, &f), AIS_MSG_SHIP);
    CHECK_EQ(u.cls, AIS_CLS_ATON);
    CHECK_EQ(u.type, 0);
    CHECK(u.pos);
    CHECK_EQ(ais_kind(&u), AIS_K_ATON);
    CHECK_STR(ais_type_name(&u), "Aid to navigation");

    CASE("the confirmation, compressed or not, either order");
    static const char CONF1[] = "{\"MessageType\":\"SubscriptionConfirmation\",\"Message\":{\"CompressionEnabled\":true}}";
    static const char CONF2[] = "{\"Message\":{\"CompressionEnabled\":false},\"MessageType\":\"SubscriptionConfirmation\"}";
    CHECK_EQ(ais_stream_parse(CONF1, sizeof CONF1 - 1, &u, &f, &comp, why, sizeof why), AIS_MSG_CONFIRM);
    CHECK(comp);
    CHECK_EQ(ais_stream_parse(CONF2, sizeof CONF2 - 1, &u, &f, &comp, why, sizeof why), AIS_MSG_CONFIRM);
    CHECK(!comp);

    CASE("an error, said in words; a message of no use; broken text");
    static const char ERR[] = "{\"error\":\"Api Key Is Not Valid\"}";
    CHECK_EQ(ais_stream_parse(ERR, sizeof ERR - 1, &u, &f, &comp, why, sizeof why), AIS_MSG_ERROR);
    CHECK_STR(why, "Api Key Is Not Valid");
    static const char OTHER[] = "{\"MessageType\":\"Interrogation\",\"Message\":{\"Interrogation\":{\"UserID\":1}}}";
    CHECK_EQ(parse(OTHER, &u, &f), AIS_MSG_OTHER);
    CHECK_EQ(parse("{\"MessageType\":\"PositionReport\",\"Message\":{", &u, &f), AIS_MSG_BAD);
    CHECK_EQ(parse("[]", &u, &f), AIS_MSG_BAD);
    /* Cut anywhere, it is refused, never read past. */
    for (size_t i = 0; i < sizeof POS_A - 2; i++)
        if (ais_stream_parse(POS_A, i, &u, &f, &comp, why, sizeof why) == AIS_MSG_SHIP) { CHECK(!"cut read whole"); break; }
}

static void test_merge(void)
{
    CASE("what a message holds laid over what was known: a position keeps the name");
    ais_ship_t s, u;
    uint32_t f;
    ais_ship_init(&s, 205123000);
    CHECK_EQ(s.sog_dk, -1);
    CHECK_EQ(s.nav, AIS_NAV_NONE);
    static const char STATIC[] =
        "{\"MessageType\":\"ShipStaticData\",\"Message\":{\"ShipStaticData\":{\"Name\":\"NORDIC STAR\","
        "\"Type\":80,\"UserID\":205123000,\"Destination\":\"ANTWERP\"}}}";
    CHECK_EQ(ais_stream_parse(STATIC, sizeof STATIC - 1, &u, &f, NULL, NULL, 0), AIS_MSG_SHIP);
    ais_merge(&s, &u, f);
    static const char POS[] =
        "{\"MessageType\":\"PositionReport\",\"Message\":{\"PositionReport\":{\"Sog\":0.2,\"Cog\":12,"
        "\"Latitude\":51.15,\"Longitude\":2.72,\"NavigationalStatus\":5,\"UserID\":205123000}}}";
    CHECK_EQ(ais_stream_parse(POS, sizeof POS - 1, &u, &f, NULL, NULL, 0), AIS_MSG_SHIP);
    ais_merge(&s, &u, f);
    CHECK_STR(s.name, "NORDIC STAR");
    CHECK_STR(s.dest, "ANTWERP");
    CHECK_EQ(s.type, 80);
    CHECK(s.pos);
    CHECK_EQ(s.nav, 5);
    CHECK(!ais_moving(&s));
    CHECK_EQ(ais_kind(&s), AIS_K_TANKER);
    CHECK_STR(ais_nav_name(s.nav), "moored");
    CHECK_EQ(ais_course(&s), 12);
}

static void test_sub(void)
{
    char b[600];
    CASE("the subscription: the key, a box round the circle, the types read here");
    CHECK(ais_stream_sub("0123abcd", MID_LAT, MID_LON, 12, b, sizeof b));
    CHECK(strstr(b, "\"APIKey\":\"0123abcd\""));
    CHECK(strstr(b, "\"BoundingBoxes\":[[[50.9600,2.4411],[51.3600,3.0789]]]"));
    CHECK(strstr(b, "\"PositionReport\""));
    CHECK(strstr(b, "\"ShipStaticData\""));
    CHECK(strstr(b, "\"StaticDataReport\""));
    CHECK(!ais_stream_sub("", MID_LAT, MID_LON, 12, b, sizeof b));
    CHECK(!ais_stream_sub("a\"b", MID_LAT, MID_LON, 12, b, sizeof b));
    CHECK(!ais_stream_sub("0123abcd", MID_LAT, MID_LON, 12, b, 40));

    CASE("the box: wider in longitude as the meridians close, held at the poles and the date line");
    double a1, o1, a2, o2;
    ais_box(60, 179.9, 30, &a1, &o1, &a2, &o2);
    CHECK(fabs(a1 - 59.5) < 1e-9 && fabs(a2 - 60.5) < 1e-9);
    CHECK(fabs(o1 - (179.9 - 1.0)) < 1e-6);
    CHECK_EQ((long)o2, 180);
    ais_box(89.9, 0, 30, &a1, &o1, &a2, &o2);
    CHECK(a2 <= 90);
}

static void test_catcher(void)
{
    CASE("an AIS-catcher's list: with a position, within range, nearest first, the unknown left out");
    static const char LIST[] =
        "{\"count\":4,\"station\":{\"lat\":51.1,\"lon\":2.7,\"mmsi\":0,\"gps\":false},\"ships\":["
        "{\"mmsi\":205123000,\"lat\":51.2101,\"lon\":2.8702,\"distance\":5.1,\"bearing\":50,\"level\":-21.5,"
        "\"count\":120,\"ppm\":1.2,\"group_mask\":1,\"approx\":false,\"heading\":305,\"cog\":308.4,\"speed\":12.3,"
        "\"to_bow\":150,\"to_stern\":30,\"to_starboard\":14,\"to_port\":14,\"shiptype\":70,\"mmsi_type\":1,"
        "\"shipclass\":2,\"validated\":1,\"msg_type\":34,\"channels\":3,\"country\":\"BE\",\"status\":0,"
        "\"draught\":7.4,\"eta_month\":11,\"eta_day\":22,\"eta_hour\":6,\"eta_minute\":30,\"imo\":9123456,"
        "\"callsign\":\"ONAB\",\"shipname\":\"NORDIC STAR\",\"destination\":\"BE ZEE\",\"eni\":\"\","
        "\"vendorid\":\"\",\"repeat\":0,\"last_signal\":3,\"last_group\":1},"
        "{\"mmsi\":244000111,\"lat\":null,\"lon\":null,\"distance\":null,\"bearing\":null,\"count\":2,"
        "\"shiptype\":36,\"mmsi_type\":2,\"shipclass\":3,\"status\":15,\"callsign\":\"\",\"shipname\":\"SEA SPRITE\","
        "\"destination\":\"\",\"last_signal\":40},"
        "{\"mmsi\":992051234,\"lat\":51.17,\"lon\":2.77,\"shiptype\":0,\"mmsi_type\":6,\"shipclass\":12,"
        "\"status\":15,\"callsign\":\"\",\"shipname\":\"BUOY [V]\",\"destination\":\"\",\"last_signal\":100},"
        "{\"mmsi\":227000999,\"lat\":49.0,\"lon\":-2.0,\"speed\":10,\"shiptype\":60,\"mmsi_type\":1,"
        "\"status\":0,\"callsign\":\"\",\"shipname\":\"FAR AWAY\",\"destination\":\"\",\"last_signal\":1}"
        "],\"error\":false}\n\n";
    ais_ship_t a[8];
    int total = -1;
    const int n = ais_catcher_parse(LIST, sizeof LIST - 1, MID_LAT, MID_LON, 30, a, 8, &total);
    CHECK_EQ(n, 2);
    CHECK_EQ(total, 4);
    CHECK_EQ(a[0].mmsi, 992051234);
    CHECK_EQ(a[0].cls, AIS_CLS_ATON);
    CHECK_EQ(a[0].sog_dk, -1);
    CHECK_EQ(a[0].hdg, -1);
    const ais_ship_t *r = &a[1];
    CHECK_EQ(r->mmsi, 205123000);
    CHECK_STR(r->name, "NORDIC STAR");
    CHECK_STR(r->callsign, "ONAB");
    CHECK_STR(r->dest, "BE ZEE");
    CHECK_EQ(r->sog_dk, 123);
    CHECK_EQ(r->cog_dd, 3084);
    CHECK_EQ(r->hdg, 305);
    CHECK_EQ(r->len_m, 180);
    CHECK_EQ(r->beam_m, 28);
    CHECK_EQ(r->type, 70);
    CHECK_EQ(r->cls, AIS_CLS_A);
    CHECK_EQ(r->imo, 9123456);
    CHECK_EQ(r->draught_dm, 74);
    CHECK_EQ(r->eta_day, 22);
    CHECK((int)lroundf(r->seen_s) == 3);
    CHECK(r->dist_nm > 5 && r->dist_nm < 5.3f);
    CHECK(r->brg > 40 && r->brg < 60);

    CASE("full: the farthest gives way; no ship list, or cut, refused");
    CHECK_EQ(ais_catcher_parse(LIST, sizeof LIST - 1, MID_LAT, MID_LON, 30, a, 1, &total), 1);
    CHECK_EQ(a[0].mmsi, 992051234);
    CHECK_EQ(ais_catcher_parse("{\"count\":0}", 11, MID_LAT, MID_LON, 30, a, 8, &total), -1);
    CHECK_EQ(ais_catcher_parse("{\"ships\":[],\"error\":false}", 26, MID_LAT, MID_LON, 30, a, 8, &total), 0);
    CHECK_EQ(ais_catcher_parse(LIST, sizeof LIST / 2, MID_LAT, MID_LON, 30, a, 8, &total), -1);

    CASE("its address: a scheme and the list's path when not given");
    char u[96];
    CHECK(ais_catcher_url("192.168.1.20:8100", u, sizeof u));
    CHECK_STR(u, "http://192.168.1.20:8100/ships.json");
    CHECK(ais_catcher_url(" http://ais.local:8100/ ", u, sizeof u));
    CHECK_STR(u, "http://ais.local:8100/ships.json");
    CHECK(ais_catcher_url("https://example.org/ais/api/ships.json", u, sizeof u));
    CHECK_STR(u, "https://example.org/ais/api/ships.json");
    CHECK(!ais_catcher_url("  ", u, sizeof u));
    CHECK(!ais_catcher_url("192.168.1.20:8100", u, 10));
}

static void test_kinds(void)
{
    ais_ship_t s;
    CASE("kinds by the AIS type, and by the MMSI where it says");
    static const struct { uint8_t type; uint8_t kind; const char *name; } T[] = {
        { 0, AIS_K_OTHER, "" }, { 30, AIS_K_FISHING, "Fishing" }, { 31, AIS_K_SPECIAL, "Towing" },
        { 36, AIS_K_PLEASURE, "Sailing" }, { 37, AIS_K_PLEASURE, "Pleasure craft" },
        { 40, AIS_K_HSC, "High-speed craft" }, { 50, AIS_K_SPECIAL, "Pilot vessel" },
        { 51, AIS_K_SPECIAL, "Search and rescue" }, { 52, AIS_K_SPECIAL, "Tug" },
        { 60, AIS_K_PASSENGER, "Passenger" }, { 69, AIS_K_PASSENGER, "Passenger" },
        { 70, AIS_K_CARGO, "Cargo" }, { 79, AIS_K_CARGO, "Cargo" }, { 80, AIS_K_TANKER, "Tanker" },
        { 90, AIS_K_OTHER, "Other type" }, { 21, AIS_K_OTHER, "Wing in ground" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        ais_ship_init(&s, 205000000);
        s.type = T[i].type;
        CHECK_EQ(ais_kind(&s), T[i].kind);
        CHECK_STR(ais_type_name(&s), T[i].name);
    }
    ais_ship_init(&s, 205240000);            /* no type yet: the status says it */
    s.nav = 7;
    CHECK_EQ(ais_kind(&s), AIS_K_FISHING);
    CHECK_STR(ais_type_name(&s), "");
    s.nav = 8;
    CHECK_EQ(ais_kind(&s), AIS_K_PLEASURE);
    s.nav = 5;
    CHECK_EQ(ais_kind(&s), AIS_K_OTHER);
    s.nav = 7; s.type = 70;                  /* the type, once it comes, wins */
    CHECK_EQ(ais_kind(&s), AIS_K_CARGO);
    ais_ship_init(&s, 2050480);              /* 002050480: a coast station */
    CHECK_EQ(ais_kind(&s), AIS_K_BASE);
    CHECK_STR(ais_type_name(&s), "Base station");
    ais_ship_init(&s, 111205100);
    CHECK_EQ(ais_kind(&s), AIS_K_SAR_AIRCRAFT);

    CASE("emergencies: SART, MOB, EPIRB by the MMSI, and a SART's status");
    ais_ship_init(&s, 970123456);
    CHECK_EQ(ais_kind(&s), AIS_K_EMERGENCY);
    CHECK_STR(ais_emergency_name(&s), "AIS-SART");
    CHECK_STR(ais_type_name(&s), "AIS-SART");
    CHECK_EQ(ais_alert(&s, ""), AIS_AL_EMERGENCY);
    ais_ship_init(&s, 972001234);
    CHECK_STR(ais_emergency_name(&s), "MAN OVERBOARD");
    ais_ship_init(&s, 974001234);
    CHECK_STR(ais_emergency_name(&s), "EPIRB");
    ais_ship_init(&s, 205123000);
    s.type = 70;
    CHECK_EQ(ais_alert(&s, NULL), AIS_AL_NONE);
    s.nav = 14;
    CHECK_EQ(ais_alert(&s, NULL), AIS_AL_EMERGENCY);
    CHECK_STR(ais_type_name(&s), "Cargo");

    CASE("the watch list: MMSIs, names with blanks, any case, a trailing *");
    ais_ship_init(&s, 205123000);
    strcpy(s.name, "NORDIC STAR");
    CHECK(ais_watched("nordic star", &s));
    CHECK(ais_watched("244000111, Nordic Star ;x", &s));
    CHECK(ais_watched("205123000", &s));
    CHECK(ais_watched("205*", &s));
    CHECK(ais_watched("nord*", &s));
    CHECK(!ais_watched("nordic", &s));
    CHECK(!ais_watched("20512300", &s));
    CHECK(!ais_watched("*", &s));
    CHECK(!ais_watched("", &s));
    CHECK_EQ(ais_alert(&s, "NORDIC STAR"), AIS_AL_WATCH);
    ais_ship_init(&s, 2050480);
    CHECK(ais_watched("002050480", &s));

    CASE("Show: all, ships alone, those under way; an alert always");
    ais_ship_init(&s, 992051234);
    CHECK(ais_shown(&s, AIS_SHOW_ALL, ""));
    CHECK(!ais_shown(&s, AIS_SHOW_SHIPS, ""));
    ais_ship_init(&s, 205123000);
    s.sog_dk = 4;
    CHECK(ais_shown(&s, AIS_SHOW_SHIPS, ""));
    CHECK(!ais_shown(&s, AIS_SHOW_MOVING, ""));
    CHECK(ais_shown(&s, AIS_SHOW_MOVING, "205123000"));
    s.sog_dk = 5;
    CHECK(ais_shown(&s, AIS_SHOW_MOVING, ""));
    CHECK(ais_show_ok(AIS_SHOW_MOVING) && !ais_show_ok(AIS_SHOWS) && !ais_show_ok(-1));

    CASE("the course: over ground, else the heading");
    ais_ship_init(&s, 205123000);
    CHECK_EQ(ais_course(&s), -1);
    s.hdg = 90;
    CHECK_EQ(ais_course(&s), 90);
    s.cog_dd = 3596;
    CHECK_EQ(ais_course(&s), 0);
}

static void test_ranges(void)
{
    CASE("the ranges: nine a unit, 12 NM the sixth, the dial held at the ends");
    CHECK_EQ(ais_range_val(ADSB_NM, 0), 1);
    CHECK_EQ(ais_range_val(ADSB_NM, 5), 12);
    CHECK_EQ(ais_range_val(ADSB_NM, AIS_RANGES - 1), 50);
    CHECK_EQ(ais_range_val(ADSB_KM, AIS_RANGES - 1), 90);
    CHECK_EQ(ais_range_val(ADSB_NM, 200), 50);
    CHECK(fabsf(ais_range_nm(ADSB_KM, 0) - 2 / 1.852f) < 1e-4f);
    CHECK(fabsf(ais_range_nm(ADSB_NM, 5) - 12) < 1e-6f);
    CHECK_EQ(ais_range_step(5, 1), 6);
    CHECK_EQ(ais_range_step(0, -3), 0);
    CHECK_EQ(ais_range_step(8, 4), 8);
    for (uint8_t u = 0; u < ADSB_UNITS; u++)
        for (uint8_t i = 1; i < AIS_RANGES; i++) CHECK(ais_range_val(u, i) > ais_range_val(u, i - 1));
}

static uint8_t *load(const char *name, size_t *n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", AIS_TABLES, name);
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

static void test_ports(void)
{
    CASE("the harbours: the committed table, sorted by latitude, the coast near the middle in it");
    size_t n = 0;
    uint8_t *p = load("ports.bin", &n);
    CHECK(p != NULL);
    if (!p) return;
    CHECK(ais_port_ok(p, n));
    CHECK(!ais_port_ok(p, n - 1));
    const size_t c = ais_port_count(p);
    CHECK(c > 500);
    ais_port_t a, b;
    bool sorted = true;
    for (size_t i = 1; i < c; i++) {
        ais_port_get(p, i - 1, &a);
        ais_port_get(p, i, &b);
        sorted = sorted && a.lat <= b.lat && b.name[0];
    }
    CHECK(sorted);
    /* Within 25 NM of the middle: Oostende at least. */
    bool ostend = false;
    for (size_t i = ais_port_from(p, (int32_t)((MID_LAT - 0.5) * 1e4)); i < c; i++) {
        ais_port_get(p, i, &a);
        if (a.lat > (MID_LAT + 0.5) * 1e4) break;
        float d;
        adsb_dist_brg(MID_LAT, MID_LON, a.lat / 1e4, a.lon / 1e4, &d, NULL);
        if (d < 25 && (strstr(a.name, "Ostend") || strstr(a.name, "Oostende"))) ostend = true;
    }
    CHECK(ostend);
    CHECK_EQ(ais_port_from(p, -900000), 0);
    CHECK_EQ(ais_port_from(p, 900001), c);
    free(p);
}

int main(void)
{
    test_stream();
    test_merge();
    test_sub();
    test_catcher();
    test_kinds();
    test_ranges();
    test_ports();
    printf("%s: %d checks, %d failed\n", __FILE__, t_run, t_fail);
    return t_fail ? 1 : 0;
}
