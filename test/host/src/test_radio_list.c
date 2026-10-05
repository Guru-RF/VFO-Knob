/* radio.h's radio_list_item(): a name out of a radio's list of antennas, as
 * the page, the log and the clients read them -- and in step with
 * flex_parse.c's counting and finding, which place the same names. */
#include "tiny.h"
#include "flex_parse.h"
#include "radio.h"

T_MAIN({
    CASE("items");
    char v[8];
    const char *l = "ANT1,ANT2,RX_A,RX_B,XVTA,XVTB";
    CHECK(radio_list_item(l, 0, v, sizeof v));
    CHECK_STR(v, "ANT1");
    CHECK(radio_list_item(l, 2, v, sizeof v));
    CHECK_STR(v, "RX_A");
    CHECK(radio_list_item(l, 5, v, sizeof v));
    CHECK_STR(v, "XVTB");
    CHECK(!radio_list_item(l, 6, v, sizeof v));
    CHECK_STR(v, "");
    CHECK(!radio_list_item(l, -1, v, sizeof v));
    CHECK(!radio_list_item("", 0, v, sizeof v));
    CHECK(!radio_list_item(NULL, 0, v, sizeof v));

    CASE("cut to fit");
    char s[4];
    CHECK(radio_list_item("LONGNAME,B", 0, s, sizeof s));
    CHECK_STR(s, "LON");
    CHECK(radio_list_item("LONGNAME,B", 1, s, sizeof s));
    CHECK_STR(s, "B");

    CASE("empty names skipped, as flex_list_* skip them");
    const char *e = ",ANT1,,ANT2,";
    CHECK_EQ(flex_list_count(e), 2);
    CHECK(radio_list_item(e, 1, v, sizeof v));
    CHECK_STR(v, "ANT2");
    CHECK_EQ(flex_list_find(e, "ANT2"), 1);
    for (int i = 0; i < flex_list_count(l); i++) {
        CHECK(radio_list_item(l, i, v, sizeof v));
        CHECK_EQ(flex_list_find(l, v), i);
    }
})
