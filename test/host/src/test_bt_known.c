/* The Bluetooth devices this knob has connected (components/bt_link/bt_known.c):
 * newest first, the eight at most, the oldest falling off; a name once heard
 * kept when the next report has none; nothing changed, nothing to save; and
 * what comes back from NVS, whatever a firmware before left there. */
#include "tiny.h"
#include "bt_known.h"

static const uint8_t A[6] = { 1, 2, 3, 4, 5, 6 }, B[6] = { 7, 8, 9, 10, 11, 12 }, NONE[6];

static const uint8_t *dev(uint8_t i)
{
    static uint8_t d[6] = { 0xAA, 0, 0, 0, 0, 0 };
    d[5] = i;
    return d;
}

static void test_order(void)
{
    CASE("newest first, and the one in front as it was changes nothing");
    bt_known_t t;
    bt_known_load(&t, NULL, 0);
    CHECK_EQ(t.n, 0);
    CHECK(bt_known_seen(&t, A, "JBL", 0));
    CHECK(!bt_known_seen(&t, A, "JBL", 0));
    CHECK(bt_known_seen(&t, B, "Speaker", 1));
    CHECK_EQ(t.n, 2);
    CHECK(!memcmp(t.rec[0].bda, B, 6));
    CHECK(!memcmp(t.rec[1].bda, A, 6));
    CHECK_EQ(t.rec[0].kind, 1);
    CASE("back to the first: in front, not twice");
    CHECK(bt_known_seen(&t, A, "JBL", 0));
    CHECK_EQ(t.n, 2);
    CHECK(!memcmp(t.rec[0].bda, A, 6));
    CHECK(!memcmp(t.rec[1].bda, B, 6));
    CHECK(!strcmp(t.rec[1].name, "Speaker"));
}

static void test_names(void)
{
    CASE("a name heard stays when the next report has none");
    bt_known_t t;
    bt_known_load(&t, NULL, 0);
    CHECK(bt_known_seen(&t, A, "", 0));
    CHECK(bt_known_name(&t, A) == NULL);
    CHECK(bt_known_seen(&t, A, "JBL", 0));
    CHECK(!strcmp(bt_known_name(&t, A), "JBL"));
    CHECK(!bt_known_seen(&t, A, "", 0));
    CHECK(!bt_known_seen(&t, A, NULL, 0));
    CHECK(!strcmp(t.rec[0].name, "JBL"));
    CHECK(bt_known_name(&t, B) == NULL);
    CASE("a new name, or a new kind, is a change");
    CHECK(bt_known_seen(&t, A, "JBL Live", 0));
    CHECK(bt_known_seen(&t, A, "JBL Live", 1));
    CHECK_EQ(t.rec[0].kind, 1);
    CASE("the longest name that fits, terminated");
    char lng[64];
    memset(lng, 'x', sizeof lng - 1);
    lng[sizeof lng - 1] = 0;
    CHECK(bt_known_seen(&t, B, lng, 0));
    CHECK_EQ(strlen(t.rec[0].name), sizeof t.rec[0].name - 1);
    CHECK(!bt_known_seen(&t, B, lng, 0));
}

static void test_no_address(void)
{
    CASE("no address: nothing on the list");
    bt_known_t t;
    bt_known_load(&t, NULL, 0);
    CHECK(!bt_known_seen(&t, NONE, "nobody", 0));
    CHECK_EQ(t.n, 0);
}

static void test_full(void)
{
    CASE("a ninth: the oldest goes, the rest in their order");
    bt_known_t t;
    bt_known_load(&t, NULL, 0);
    for (uint8_t i = 0; i < 9; i++) CHECK(bt_known_seen(&t, dev(i), "d", 0));
    CHECK_EQ(t.n, BT_KNOWN_DEVICES);
    CHECK_EQ(t.rec[0].bda[5], 8);
    CHECK_EQ(t.rec[BT_KNOWN_DEVICES - 1].bda[5], 1);
    CHECK(bt_known_name(&t, dev(0)) == NULL);
    CASE("full, one on the list again: nobody goes");
    CHECK(bt_known_seen(&t, dev(3), "d", 0));
    CHECK_EQ(t.n, BT_KNOWN_DEVICES);
    CHECK_EQ(t.rec[0].bda[5], 3);
    CHECK_EQ(t.rec[1].bda[5], 8);
    CHECK_EQ(t.rec[BT_KNOWN_DEVICES - 1].bda[5], 1);
}

static void test_forget(void)
{
    CASE("forgotten: off the list, the rest closed up");
    bt_known_t t;
    bt_known_load(&t, NULL, 0);
    for (uint8_t i = 0; i < 3; i++) bt_known_seen(&t, dev(i), "d", 0);
    CHECK(bt_known_forget(&t, dev(1)));
    CHECK(!bt_known_forget(&t, dev(1)));
    CHECK_EQ(t.n, 2);
    CHECK_EQ(t.rec[0].bda[5], 2);
    CHECK_EQ(t.rec[1].bda[5], 0);
    CHECK(bt_known_forget(&t, dev(2)));
    CHECK(bt_known_forget(&t, dev(0)));
    CHECK_EQ(t.n, 0);
}

static void test_load(void)
{
    CASE("back from NVS: the same, in order, spare kept");
    bt_known_t t, u;
    bt_known_load(&t, NULL, 0);
    bt_known_seen(&t, A, "JBL", 0);
    bt_known_seen(&t, B, "Speaker", 1);
    t.rec[0].spare = 0x5A;
    bt_known_load(&u, t.rec, (size_t)t.n * sizeof t.rec[0]);
    CHECK_EQ(u.n, 2);
    CHECK(!memcmp(u.rec, t.rec, 2 * sizeof t.rec[0]));
    bt_known_seen(&u, B, "Speaker 2", 1);
    CHECK_EQ(u.rec[0].spare, 0x5A);

    CASE("no address, a device twice, a part of a record: left out");
    bt_known_rec_t r[4];
    memset(r, 0, sizeof r);
    memcpy(r[1].bda, A, 6);
    memcpy(r[2].bda, A, 6);
    memcpy(r[3].bda, B, 6);
    bt_known_load(&u, r, sizeof r - 1);
    CHECK_EQ(u.n, 1);
    CHECK(!memcmp(u.rec[0].bda, A, 6));

    CASE("an unterminated name comes back terminated");
    memset(r, 0, sizeof r);
    memcpy(r[0].bda, A, 6);
    memset(r[0].name, 'x', sizeof r[0].name);
    bt_known_load(&u, r, sizeof r[0]);
    CHECK_EQ(strlen(u.rec[0].name), sizeof r[0].name - 1);

    CASE("more than the eight: the eight newest");
    bt_known_rec_t m[BT_KNOWN_DEVICES + 2];
    memset(m, 0, sizeof m);
    for (int i = 0; i < BT_KNOWN_DEVICES + 2; i++) memcpy(m[i].bda, dev((uint8_t)i), 6);
    bt_known_load(&u, m, sizeof m);
    CHECK_EQ(u.n, BT_KNOWN_DEVICES);
    CHECK_EQ(u.rec[BT_KNOWN_DEVICES - 1].bda[5], BT_KNOWN_DEVICES - 1);
}

T_MAIN(test_order(); test_names(); test_no_address(); test_full(); test_forget(); test_load())
