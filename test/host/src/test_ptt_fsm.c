/* PTT toggle FSM -- every refusal and timeout path.
 *
 * This is one of the two pieces most likely to be subtly wrong and hardest to
 * debug on real hardware with a transmitter attached, which is exactly why it
 * is pure C and tested here first. */
#include "tiny.h"
#include "ptt_fsm.h"

static void test_normal_cycle(void)
{
    CASE("key and unkey");
    ptt_fsm_t f; ptt_out_t o;
    ptt_fsm_init(&f);

    /* Nothing vibrates on the air: the motor is next to the microphone. */
    ptt_fsm_event(&f, PTT_EV_TAP_KEY, 1000, PERMIT_ALL, &o);
    CHECK(o.send_key);
    CHECK_EQ(f.state, PTT_REQ_ON);
    CHECK_EQ(o.haptic, 0);

    ptt_fsm_event(&f, PTT_EV_CONFIRM_TRUE, 1120, PERMIT_ALL, &o);
    CHECK_EQ(f.state, PTT_ON);
    CHECK(o.entered_tx);
    CHECK_EQ(o.haptic, 0);
    CHECK(ptt_is_tx(&f));

    ptt_fsm_event(&f, PTT_EV_TAP_UNKEY, 5000, PERMIT_ALL, &o);
    CHECK(o.send_unkey);
    CHECK_EQ(f.state, PTT_RELEASING);
    CHECK_EQ(f.rung, 1);
    CHECK_EQ(o.haptic, 0);

    ptt_fsm_event(&f, PTT_EV_CONFIRM_FALSE, 5060, PERMIT_ALL, &o);
    CHECK_EQ(f.state, PTT_IDLE);
    CHECK(o.left_tx);
    CHECK_EQ(o.haptic, 4);               /* back on receive: felt, not heard */
    CHECK(!ptt_is_tx(&f));
}

static void test_refusals(void)
{
    CASE("refused by the server");
    ptt_fsm_t f; ptt_out_t o;
    ptt_fsm_init(&f);

    ptt_fsm_event(&f, PTT_EV_TAP_KEY, 1000, PERMIT_ALL, &o);
    /* trx:false while REQ_ON is a refusal -- and it is the ONLY signal the
     * protocol gives us. Seven server-side causes collapse to this frame. */
    ptt_fsm_event(&f, PTT_EV_CONFIRM_FALSE, 1200, PERMIT_ALL, &o);
    CHECK_EQ(f.state, PTT_IDLE);
    CHECK(o.refused);
    CHECK_EQ(o.haptic, 12);
    CHECK_EQ(f.refusals, 1);
    CHECK_EQ(o.missing, 0);              /* the server's no, not ours */

    CASE("never confirmed");
    ptt_fsm_init(&f);
    ptt_fsm_event(&f, PTT_EV_TAP_KEY, 1000, PERMIT_ALL, &o);
    ptt_fsm_event(&f, PTT_EV_TICK, 1000 + PTT_CONFIRM_MS - 1, PERMIT_ALL, &o);
    CHECK_EQ(f.state, PTT_REQ_ON);       /* must not fire early: the server's
                                            own deadline is 1250ms */
    ptt_fsm_event(&f, PTT_EV_TICK, 1000 + PTT_CONFIRM_MS, PERMIT_ALL, &o);
    CHECK_EQ(f.state, PTT_IDLE);
    CHECK(o.refused);

    CASE("blocked by permits");
    ptt_fsm_init(&f);
    ptt_fsm_event(&f, PTT_EV_TAP_KEY, 1000, PERMIT_ALL & ~PERMIT_LINK, &o);
    CHECK(!o.send_key);
    CHECK(o.refused);
    CHECK_EQ(o.missing, PERMIT_LINK);    /* says what was not ready */
    CHECK_EQ(f.state, PTT_IDLE);
}

static void test_no_timeout(void)
{
    /* The transmit time-out is the radio's (Radio Setup -> TX -> Timeout in
     * AetherSDR). The knob no longer keeps one of its own. */
    CASE("a long over is not cut short");
    ptt_fsm_t f; ptt_out_t o;
    ptt_fsm_init(&f);
    ptt_fsm_event(&f, PTT_EV_TAP_KEY, 0, PERMIT_ALL, &o);
    ptt_fsm_event(&f, PTT_EV_CONFIRM_TRUE, 100, PERMIT_ALL, &o);
    for (uint32_t t = 100; t <= 100 + 30u * 60 * 1000; t += 5000) {
        ptt_fsm_event(&f, PTT_EV_TICK, t, PERMIT_ALL, &o);
        CHECK(!o.send_unkey);
    }
    CHECK_EQ(f.state, PTT_ON);
}

static void test_silent_on_air(void)
{
    /* No still-keyed reminder, however long the over, and no haptic when a
     * fault starts the ladder mid-over: the radio is still on the air. */
    CASE("silent while keyed");
    ptt_fsm_t f; ptt_out_t o;
    ptt_fsm_init(&f);
    ptt_fsm_event(&f, PTT_EV_TAP_KEY, 0, PERMIT_ALL, &o);
    ptt_fsm_event(&f, PTT_EV_CONFIRM_TRUE, 0, PERMIT_ALL, &o);
    for (uint32_t t = 0; t <= 5u * 60 * 1000; t += 1000) {
        ptt_fsm_event(&f, PTT_EV_TICK, t, PERMIT_ALL, &o);
        CHECK_EQ(o.haptic, 0);
    }
    CHECK_EQ(f.state, PTT_ON);
    ptt_fsm_abort(&f, PTT_AB_PONG_STALE, 400000, &o);
    CHECK_EQ(o.haptic, 0);
    CHECK_EQ(f.state, PTT_RELEASING);
}

static void test_ladder(void)
{
    CASE("pong stale skips rung 1");
    ptt_fsm_t f; ptt_out_t o;
    ptt_fsm_init(&f);
    ptt_fsm_event(&f, PTT_EV_TAP_KEY, 0, PERMIT_ALL, &o);
    ptt_fsm_event(&f, PTT_EV_CONFIRM_TRUE, 0, PERMIT_ALL, &o);

    /* Closing the socket beats asking politely: AetherSDR unkeys on client
     * disconnect, and if trx:false could get through, so could the close. */
    ptt_fsm_abort(&f, PTT_AB_PONG_STALE, 1000, &o);
    CHECK(o.close_socket);
    CHECK(!o.send_unkey);
    CHECK_EQ(f.rung, 2);

    CASE("link down goes straight to destroy");
    ptt_fsm_init(&f);
    ptt_fsm_event(&f, PTT_EV_TAP_KEY, 0, PERMIT_ALL, &o);
    ptt_fsm_event(&f, PTT_EV_CONFIRM_TRUE, 0, PERMIT_ALL, &o);
    ptt_fsm_abort(&f, PTT_AB_LINK_DOWN, 1000, &o);
    CHECK(o.destroy_socket);
    CHECK_EQ(f.rung, 3);

    CASE("ladder escalates to reboot");
    ptt_fsm_init(&f);
    ptt_fsm_event(&f, PTT_EV_TAP_KEY, 0, PERMIT_ALL, &o);
    ptt_fsm_event(&f, PTT_EV_CONFIRM_TRUE, 0, PERMIT_ALL, &o);
    ptt_fsm_event(&f, PTT_EV_TAP_UNKEY, 1000, PERMIT_ALL, &o);
    CHECK_EQ(f.rung, 1);

    uint32_t t = 1000 + PTT_RUNG1_MS;
    ptt_fsm_event(&f, PTT_EV_TICK, t, PERMIT_ALL, &o);
    CHECK(o.close_socket); CHECK_EQ(f.rung, 2);

    t += PTT_RUNG2_MS;
    ptt_fsm_event(&f, PTT_EV_TICK, t, PERMIT_ALL, &o);
    CHECK(o.destroy_socket); CHECK_EQ(f.rung, 3);

    t += PTT_RUNG3_MS;
    ptt_fsm_event(&f, PTT_EV_TICK, t, PERMIT_ALL, &o);
    CHECK(o.restart);
    /* Rebooting to unkey a transmitter is drastic and correct: it releases the
     * socket at the stack level and we come back with PTT not permitted. */

    CASE("a slow unkey confirmation is not a fault");
    /* 1226 ms, measured through a congested SmartLink. Rung 1 used to give
     * up at 600. */
    ptt_fsm_init(&f);
    ptt_fsm_event(&f, PTT_EV_TAP_KEY, 0, PERMIT_ALL, &o);
    ptt_fsm_event(&f, PTT_EV_CONFIRM_TRUE, 0, PERMIT_ALL, &o);
    ptt_fsm_event(&f, PTT_EV_TAP_UNKEY, 1000, PERMIT_ALL, &o);
    ptt_fsm_event(&f, PTT_EV_TICK, 1000 + 1226, PERMIT_ALL, &o);
    CHECK(!o.close_socket);
    CHECK_EQ(f.rung, 1);
    ptt_fsm_event(&f, PTT_EV_CONFIRM_FALSE, 1000 + 1226, PERMIT_ALL, &o);
    CHECK_EQ(f.state, PTT_IDLE);
    CHECK(o.left_tx);
}

static void test_remote_and_idle(void)
{
    CASE("server unkeys us unasked");
    ptt_fsm_t f; ptt_out_t o;
    ptt_fsm_init(&f);
    ptt_fsm_event(&f, PTT_EV_TAP_KEY, 0, PERMIT_ALL, &o);
    ptt_fsm_event(&f, PTT_EV_CONFIRM_TRUE, 0, PERMIT_ALL, &o);
    ptt_fsm_event(&f, PTT_EV_CONFIRM_FALSE, 2000, PERMIT_ALL, &o);
    CHECK_EQ(f.state, PTT_IDLE);
    CHECK_EQ(f.reason, PTT_AB_REMOTE);
    CHECK(o.left_tx);

    CASE("trx:true while idle is not ours");
    ptt_fsm_init(&f);
    ptt_fsm_event(&f, PTT_EV_CONFIRM_TRUE, 1000, PERMIT_ALL, &o);
    CHECK_EQ(f.state, PTT_IDLE);         /* caller raises the alarm; we must
                                            not try to unkey someone else */
    CHECK(!o.send_unkey);

    CASE("aborts are ignored when idle");
    ptt_fsm_abort(&f, PTT_AB_PONG_STALE, 2000, &o);
    CHECK(!o.close_socket);
    CHECK_EQ(f.state, PTT_IDLE);

    CASE("cancel a pending key");
    ptt_fsm_init(&f);
    ptt_fsm_event(&f, PTT_EV_TAP_KEY, 0, PERMIT_ALL, &o);
    ptt_fsm_event(&f, PTT_EV_TAP_UNKEY, 200, PERMIT_ALL, &o);
    CHECK_EQ(f.state, PTT_RELEASING);
    CHECK(o.send_unkey);
}

T_MAIN({
    test_normal_cycle();
    test_refusals();
    test_no_timeout();
    test_silent_on_air();
    test_ladder();
    test_remote_and_idle();
})
