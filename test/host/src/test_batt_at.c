/* A device's battery from its AT commands (companion/main/batt.c): Apple's
 * AT+XAPL and AT+IPHONEACCEV, and the hands-free profile's AT+BIEV, as the
 * stack hands the second chip a command it does not know -- without its
 * "AT", or with it -- and everything else, which is answered ERROR. The
 * lines are what a JLab speaker and a Jabra headset send, and what Apple's
 * and Android's own readers take. */
#include "tiny.h"
#include "batt.h"

static batt_at_t at(const char *line)
{
    batt_at_t a;
    memset(&a, 0x5A, sizeof a);                /* nothing left over from before */
    batt_at_parse(line, &a);
    return a;
}

static void test_xapl(void)
{
    batt_at_t a;

    CASE("XAPL, as the stack hands it over");
    a = at("+XAPL=0067-24A9-0101,6");
    CHECK(a.cmd == BATT_AT_XAPL && a.ok);
    CHECK_STR(a.id, "0067-24A9-0101");
    CHECK_EQ(a.features, BATT_XAPL_BATTERY | BATT_XAPL_DOCK);
    CHECK_EQ(a.pct, -1);                       /* no battery in it: a promise of reports */

    CASE("XAPL, with its AT, in small letters");
    a = at("AT+XAPL=05AC-1234-0100,10");
    CHECK(a.cmd == BATT_AT_XAPL && a.ok && a.features == 10);
    a = at("at+xapl=009e-400c-0001,2");
    CHECK(a.cmd == BATT_AT_XAPL && a.ok && a.features == BATT_XAPL_BATTERY);
    CHECK_STR(a.id, "009e-400c-0001");

    CASE("XAPL, odd spacing");
    a = at("  +XAPL = 0067-24A9-0101 ,  6  ");
    CHECK(a.cmd == BATT_AT_XAPL && a.ok && a.features == 6);
    CHECK_STR(a.id, "0067-24A9-0101");
    a = at("AT +XAPL=\t0067-24A9-0101\t,\t2");
    CHECK(a.ok && a.features == 2);

    CASE("XAPL, no battery among its features: still answered");
    a = at("+XAPL=0A12-0001-0100,8");
    CHECK(a.ok && !(a.features & BATT_XAPL_BATTERY));

    CASE("XAPL, a name too long for the log: cut");
    a = at("+XAPL=0123456789012345678901234567890123456789,2");
    CHECK(a.ok && strlen(a.id) == sizeof a.id - 1);

    CASE("XAPL, ill-formed: not answered as an iPhone");
    static const char *const BAD[] = {
        "+XAPL", "+XAPL=", "+XAPL=?", "+XAPL?", "+XAPL=0067-24A9-0101", "+XAPL=,2",
        "+XAPL= ,2", "+XAPL=0067-24A9-0101,", "+XAPL=0067-24A9-0101,x", "+XAPL=0067-24A9-0101,2,3",
        "+XAPL=0067-24A9-0101,-2", "+XAPL=0067-24A9-0101,2 x", "+XAPL=a,1234567",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        a = at(BAD[i]);
        CHECK(a.cmd == BATT_AT_XAPL && !a.ok && a.pct == -1);
    }
}

static void test_accev(void)
{
    batt_at_t a;

    CASE("IPHONEACCEV, the battery alone: 0-9 for 10-100 %");
    a = at("+IPHONEACCEV=1,1,7");
    CHECK(a.cmd == BATT_AT_ACCEV && a.ok && a.pct == 80);
    CHECK_EQ(at("+IPHONEACCEV=1,1,0").pct, 10);
    CHECK_EQ(at("+IPHONEACCEV=1,1,9").pct, 100);
    CHECK_EQ(at("+IPHONEACCEV=1,1,4").pct, 50);

    CASE("IPHONEACCEV, several pairs: the battery wherever it is");
    a = at("+IPHONEACCEV=2,1,5,2,0");
    CHECK(a.ok && a.pct == 60);
    a = at("AT+IPHONEACCEV=2,2,1,1,9");
    CHECK(a.ok && a.pct == 100);
    a = at("+IPHONEACCEV=4,3,0,4,1,2,1,1,2");
    CHECK(a.ok && a.pct == 30);
    a = at("+IPHONEACCEV=2,1,3,1,8");              /* said twice: the first */
    CHECK(a.ok && a.pct == 40);

    CASE("IPHONEACCEV, no battery in it: answered, nothing taken");
    a = at("+IPHONEACCEV=1,2,1");
    CHECK(a.ok && a.pct == -1);
    a = at("+IPHONEACCEV=0");
    CHECK(a.ok && a.pct == -1);

    CASE("IPHONEACCEV, odd spacing, small letters");
    a = at(" +IPHONEACCEV = 2 , 1 , 3 , 2 , 1 ");
    CHECK(a.ok && a.pct == 40);
    a = at("at+iphoneaccev=1,1,6");
    CHECK(a.cmd == BATT_AT_ACCEV && a.ok && a.pct == 70);

    CASE("IPHONEACCEV, a battery out of range: well formed, but no charge");
    static const char *const RANGE[] = {
        "+IPHONEACCEV=1,1,10", "+IPHONEACCEV=1,1,-1", "+IPHONEACCEV=1,1,100", "+IPHONEACCEV=1,1,999999",
    };
    for (size_t i = 0; i < sizeof RANGE / sizeof *RANGE; i++) {
        a = at(RANGE[i]);
        CHECK(a.ok && a.pct == -1);
    }
    a = at("+IPHONEACCEV=2,1,12,1,6");             /* the second in range */
    CHECK(a.ok && a.pct == 70);

    CASE("IPHONEACCEV, ill-formed: ERROR, nothing taken");
    static const char *const BAD[] = {
        "+IPHONEACCEV", "+IPHONEACCEV=", "+IPHONEACCEV=?", "+IPHONEACCEV?", "+IPHONEACCEV=x",
        "+IPHONEACCEV=2,1,5", "+IPHONEACCEV=1,1,5,2,0", "+IPHONEACCEV=1,1", "+IPHONEACCEV=1,1,",
        "+IPHONEACCEV=1,,5", "+IPHONEACCEV=1,1,5,", "+IPHONEACCEV=1,1,5x", "+IPHONEACCEV=1;1;5",
        "+IPHONEACCEV=-1", "+IPHONEACCEV=1,1,1234567", "+IPHONEACCEV=99999,1,5",
        "+IPHONEACCEV=1 1 5",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        a = at(BAD[i]);
        CHECK(a.cmd == BATT_AT_ACCEV && !a.ok && a.pct == -1);
        if (a.ok) fprintf(stderr, "  taken: %s\n", BAD[i]);
    }
}

static void test_biev(void)
{
    batt_at_t a;

    CASE("BIEV, the battery indicator: 0-100 %");
    a = at("+BIEV=2,80");
    CHECK(a.cmd == BATT_AT_BIEV && a.ok && a.pct == 80);
    CHECK_EQ(at("AT+BIEV=2,0").pct, 0);
    CHECK_EQ(at("+biev = 2 , 100").pct, 100);

    CASE("BIEV, another indicator, or out of range: ERROR");
    static const char *const BAD[] = {
        "+BIEV=1,1", "+BIEV=2,101", "+BIEV=2,-1", "+BIEV=2", "+BIEV=2,", "+BIEV=,80", "+BIEV=2,80,1",
        "+BIEV=?", "+BIEV", "+BIEV=3,50",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        a = at(BAD[i]);
        CHECK(a.cmd == BATT_AT_BIEV && !a.ok && a.pct == -1);
    }
}

static void test_others(void)
{
    batt_at_t a;

    CASE("everything else: another command, never OK");
    static const char *const OTHER[] = {
        "+BIND=1,2", "+BIND=?", "+BIND?", "+XAPLX=0067-24A9-0101,2", "+XAP=0067,2", "+IPHONEACCEVX=1,1,5",
        "+IPHONEACCE=1,1,5", "+XAPL2=1,2", "+CSRSF=0,0,0,1,0,0,0", "+XEVENT=BATTERY,6,10,1,0", "",
        "AT", "+", "garbage", "XAPL=0067-24A9-0101,2", "ATXAPL=1,2", "\x01\x02\xff", "+\xff\xfe=1,2",
        "+VERYLONGNAMEINDEEDTHATGOESON=1",
    };
    for (size_t i = 0; i < sizeof OTHER / sizeof *OTHER; i++) {
        a = at(OTHER[i]);
        CHECK(a.cmd == BATT_AT_OTHER && !a.ok && a.pct == -1);
    }
    a = at(NULL);
    CHECK(a.cmd == BATT_AT_OTHER && !a.ok && a.pct == -1);

    CASE("garbage after a good start, and a long line");
    char longl[600];
    memset(longl, '1', sizeof longl - 1);
    longl[sizeof longl - 1] = 0;
    memcpy(longl, "+IPHONEACCEV=", 13);
    a = at(longl);
    CHECK(a.cmd == BATT_AT_ACCEV && !a.ok);
    memcpy(longl, "+XAPL=", 6);
    a = at(longl);
    CHECK(a.cmd == BATT_AT_XAPL && !a.ok);
    longl[300] = ',';
    longl[301] = '2';
    longl[302] = 0;
    a = at(longl);
    CHECK(a.ok && a.features == 2 && strlen(a.id) == sizeof a.id - 1);
}

T_MAIN({
    test_xapl();
    test_accev();
    test_biev();
    test_others();
})
