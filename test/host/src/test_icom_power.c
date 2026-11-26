/* An Icom radio's power over its network (components/vfo_core/src/
 * icom_power.c): when the icom firmware sends 18 01 to a radio in standby,
 * how long it waits for it, and when it may put the radio back there. */
#include "tiny.h"
#include "icom_power.h"

int main(void)
{
    icom_wake_t w;

    CASE("a radio that is on: its frequency answered, nothing sent");
    {
        icom_wake_reset(&w);
        CHECK_EQ(icom_wake_step(&w, 1000, true, 1000, false, ICOM_WAKE_AUTO), ICOM_WAKE_WAIT);
        CHECK_EQ(icom_wake_step(&w, 1400, true, 1000, true, ICOM_WAKE_AUTO), ICOM_WAKE_WAIT);
        CHECK_EQ(icom_wake_step(&w, 9000, true, 1000, true, ICOM_WAKE_AUTO), ICOM_WAKE_WAIT);
        CHECK(!w.sent);
        CHECK(!icom_wake_pending(&w, true));
    }

    CASE("no CI-V yet: nothing to send it on");
    {
        icom_wake_reset(&w);
        CHECK_EQ(icom_wake_step(&w, 50000, false, 0, false, ICOM_WAKE_AUTO), ICOM_WAKE_WAIT);
        CHECK_EQ(icom_wake_step(&w, 50000, false, 0, false, ICOM_WAKE_ASKED), ICOM_WAKE_WAIT);
        CHECK(!w.sent);
    }

    CASE("in standby: CI-V silent, 18 01 once after ICOM_WAKE_AFTER_MS");
    {
        icom_wake_reset(&w);
        const uint32_t open = 3000;
        CHECK_EQ(icom_wake_step(&w, open + ICOM_WAKE_AFTER_MS - 1, true, open, false, ICOM_WAKE_AUTO),
                 ICOM_WAKE_WAIT);
        CHECK_EQ(icom_wake_step(&w, open + ICOM_WAKE_AFTER_MS, true, open, false, ICOM_WAKE_AUTO),
                 ICOM_WAKE_SEND);
        CHECK(icom_wake_pending(&w, false));
        /* ...once: the radio is booting, not asked again. */
        CHECK_EQ(icom_wake_step(&w, open + ICOM_WAKE_AFTER_MS + 5, true, open, false, ICOM_WAKE_AUTO),
                 ICOM_WAKE_WAIT);
        CHECK_EQ(icom_wake_step(&w, open + 20000, true, open, false, ICOM_WAKE_AUTO), ICOM_WAKE_WAIT);
        /* It came up: done, nothing pending. */
        CHECK_EQ(icom_wake_step(&w, open + 21000, true, open, true, ICOM_WAKE_AUTO), ICOM_WAKE_WAIT);
        CHECK(!icom_wake_pending(&w, true));
    }

    CASE("it never comes up: given up ICOM_WAKE_BOOT_MS after 18 01");
    {
        icom_wake_reset(&w);
        CHECK_EQ(icom_wake_step(&w, 10000, true, 0, false, ICOM_WAKE_AUTO), ICOM_WAKE_SEND);
        CHECK_EQ(icom_wake_step(&w, 10000 + ICOM_WAKE_BOOT_MS - 1, true, 0, false, ICOM_WAKE_AUTO),
                 ICOM_WAKE_WAIT);
        CHECK_EQ(icom_wake_step(&w, 10000 + ICOM_WAKE_BOOT_MS, true, 0, false, ICOM_WAKE_AUTO),
                 ICOM_WAKE_GIVE_UP);
    }

    CASE("the operator asked: 18 01 as soon as CI-V is open");
    {
        icom_wake_reset(&w);
        CHECK_EQ(icom_wake_step(&w, 700, true, 700, false, ICOM_WAKE_ASKED), ICOM_WAKE_SEND);
        CHECK_EQ(icom_wake_step(&w, 800, true, 700, false, ICOM_WAKE_ASKED), ICOM_WAKE_WAIT);
    }

    CASE("found in standby after the first session: left asleep, the operator asked");
    {
        icom_wake_reset(&w);
        CHECK_EQ(icom_wake_step(&w, 3000, true, 0, false, ICOM_WAKE_NEVER), ICOM_WAKE_WAIT);
        CHECK_EQ(icom_wake_step(&w, ICOM_WAKE_AFTER_MS, true, 0, false, ICOM_WAKE_NEVER),
                 ICOM_WAKE_ASLEEP);
        CHECK(!w.sent);
        CHECK(!icom_wake_pending(&w, false));
        /* ...on and answering, nothing is said about it. */
        CHECK_EQ(icom_wake_step(&w, 60000, true, 0, true, ICOM_WAKE_NEVER), ICOM_WAKE_WAIT);
    }

    CASE("the timer wraps: still counted right");
    {
        icom_wake_reset(&w);
        const uint32_t open = 0xFFFFFF00u;
        CHECK_EQ(icom_wake_step(&w, open + 100, true, open, false, ICOM_WAKE_AUTO), ICOM_WAKE_WAIT);
        CHECK_EQ(icom_wake_step(&w, open + ICOM_WAKE_AFTER_MS, true, open, false, ICOM_WAKE_AUTO),
                 ICOM_WAKE_SEND);
        CHECK_EQ(icom_wake_step(&w, open + ICOM_WAKE_AFTER_MS + ICOM_WAKE_BOOT_MS, true, open,
                                false, ICOM_WAKE_AUTO), ICOM_WAKE_GIVE_UP);
    }

    CASE("a new session starts over");
    {
        icom_wake_reset(&w);
        CHECK_EQ(icom_wake_step(&w, 5000, true, 0, false, ICOM_WAKE_AUTO), ICOM_WAKE_SEND);
        icom_wake_reset(&w);
        CHECK(!w.sent);
        CHECK_EQ(icom_wake_step(&w, 6000, true, 6000, false, ICOM_WAKE_AUTO), ICOM_WAKE_WAIT);
    }

    CASE("the Power OFF Setting, as 1A 05 reads it");
    {
        CHECK_EQ(icom_off_setting(0x00), ICOM_OFF_SHUTDOWN);
        CHECK_EQ(icom_off_setting(0x01), ICOM_OFF_STANDBY);
        CHECK_EQ(icom_off_setting(0x02), ICOM_OFF_UNKNOWN);
        CHECK_EQ(icom_off_setting(0xFF), ICOM_OFF_UNKNOWN);
    }

    CASE("standby only where the radio keeps its network, with the link up, off the air");
    {
        CHECK(icom_standby_allowed(ICOM_OFF_STANDBY, true, false));
        CHECK(!icom_standby_allowed(ICOM_OFF_SHUTDOWN, true, false));
        CHECK(!icom_standby_allowed(ICOM_OFF_UNKNOWN, true, false));
        CHECK(!icom_standby_allowed(ICOM_OFF_STANDBY, false, false));
        CHECK(!icom_standby_allowed(ICOM_OFF_STANDBY, true, true));
    }

    printf("%s: %d checks, %d failed\n", __FILE__, t_run, t_fail);
    return t_fail ? 1 : 0;
}
