/* PTT state machine -- TOGGLE.
 *
 * Tap to key, tap to unkey: anywhere on the slab below the blue line, both
 * ways. You should never have to aim carefully to STOP transmitting.
 *
 * Toggle gives up the one safety property momentary PTT has for free: a finger
 * held down is a continuous assertion of intent, and a frozen touch coordinate
 * exposes a hung controller. Neither exists here, so the weight falls on the
 * pong-based link watchdog, the teardown ladder, and the radio's own transmit
 * time-out (Radio Setup -> TX -> Timeout in AetherSDR). The knob had a time-out
 * timer of its own; it duplicated the radio's and is gone.
 *
 * The ladder matters more than the commands. AetherSDR calls abortTciPtt() on
 * client disconnect (TciServer.cpp:975), so DESTROYING OUR OWN SOCKET IS A MORE
 * RELIABLE UNKEY THAN ANY COMMAND WE CAN SEND. Hence rungs 2-4.
 *
 * Pure C: no IDF, no time source of its own, no I/O. The caller supplies now_ms
 * and performs the actions in ptt_out_t. That makes every refusal and timeout
 * path host-testable.
 */
#ifndef VFO_PTT_FSM_H
#define VFO_PTT_FSM_H

#include <stdbool.h>
#include <stdint.h>

/* The server's own PTT confirmation deadline is 1250ms (TciServer.cpp:2675)
 * and it waits on a radio that is in no hurry. Anything tighter fires a
 * spurious fault on essentially every key. */
#define PTT_CONFIRM_MS      1500u
#define PTT_KEYED_NAG_MS    10000u   /* "still transmitting" reminder     */

/* Teardown ladder deadlines. Rung 1 is how long an unkey may take to be
 * confirmed before the socket is torn down, and it was 600 ms. Through
 * SmartLink the confirmation is a round trip to a radio across the internet:
 * 200-240 ms usually, but 629 ms, 1.2 s and more than 1.5 s were measured
 * while the link was congested. Each overrun tore the socket down after a
 * perfectly good over, although AetherSDR had passed the unkey to the radio
 * at once; tearing down only helps if it had not. */
#define PTT_RUNG1_MS 3000u   /* trx:false; on the existing socket */
#define PTT_RUNG2_MS  500u   /* websocket close(1001)             */
#define PTT_RUNG3_MS 1000u   /* destroy -> FIN/RST                */

typedef enum {
    PTT_IDLE = 0,
    PTT_REQ_ON,     /* key sent, awaiting confirmation */
    PTT_ON,
    PTT_RELEASING,  /* in the teardown ladder          */
} ptt_state_t;

typedef enum {
    PTT_AB_NONE = 0,
    PTT_AB_OPERATOR,
    PTT_AB_PONG_STALE,
    PTT_AB_LINK_DOWN,
    PTT_AB_TOUCH_FAULT,
    PTT_AB_REMOTE,        /* server said false while we thought we were ON */
} ptt_abort_t;

typedef enum {
    PTT_EV_TAP_KEY = 0,   /* tap on the arm target      */
    PTT_EV_TAP_UNKEY,     /* tap anywhere in exit region*/
    PTT_EV_CONFIRM_TRUE,  /* inbound trx:<n>,true;      */
    PTT_EV_CONFIRM_FALSE, /* inbound trx:<n>,false;     */
    PTT_EV_TICK,
    PTT_EV_ABORT,         /* out.reason supplied by caller via ptt_fsm_abort */
} ptt_ev_t;

/* Every permit bit must be set before a key is accepted. Band and mode bits
 * exist but are pinned set in v1 (no band plan yet) so adding the table later
 * does not touch this FSM. */
#define PERMIT_LINK          (1u << 0)
#define PERMIT_TRX           (1u << 1)
#define PERMIT_TX_ENABLE     (1u << 2)
#define PERMIT_NO_OVERLAY    (1u << 3)
#define PERMIT_NO_FAULT      (1u << 4)
#define PERMIT_PONG_FRESH    (1u << 5)
#define PERMIT_NO_RECONCILE  (1u << 6)
#define PERMIT_BAND          (1u << 7)   /* v1: pinned set */
#define PERMIT_MODE          (1u << 8)   /* v1: pinned set */
#define PERMIT_ALL           0x1FFu

typedef struct {
    bool     send_key;
    bool     send_unkey;
    bool     close_socket;      /* rung 2 */
    bool     destroy_socket;    /* rung 3 */
    bool     restart;           /* rung 4 */
    uint8_t  haptic;            /* DRV2605 ROM effect id, 0 = none */
    uint8_t  haptic_prio;
    bool     refused;           /* surface "PTT REFUSED" */
    uint32_t missing;           /* ...for want of these PERMIT_* bits; 0 when
                                   the server refused or never confirmed */
    bool     entered_tx;
    bool     left_tx;
} ptt_out_t;

typedef struct {
    ptt_state_t state;
    uint8_t     rung;           /* 0 = not in the ladder */
    ptt_abort_t reason;
    uint32_t    t_state_ms;     /* when the current state was entered */
    uint32_t    t_rung_ms;
    uint32_t    t_last_nag_ms;
    uint32_t    refusals;
} ptt_fsm_t;

void ptt_fsm_init(ptt_fsm_t *f);

/* Drive the machine. `permit` is ignored for every event except PTT_EV_TAP_KEY.
 * `out` is always fully initialised. */
void ptt_fsm_event(ptt_fsm_t *f, ptt_ev_t ev, uint32_t now_ms,
                   uint32_t permit, ptt_out_t *out);

/* Convenience: abort with a specific reason. A stale pong SKIPS RUNG 1 and goes
 * straight to closing the socket -- if any path to the server survives, the FIN
 * reaches it and abortTciPtt() runs in milliseconds, which beats a command that
 * may never be dispatched. */
void ptt_fsm_abort(ptt_fsm_t *f, ptt_abort_t reason, uint32_t now_ms,
                   ptt_out_t *out);

bool ptt_is_tx(const ptt_fsm_t *f);
const char *ptt_state_name(ptt_state_t s);
const char *ptt_abort_name(ptt_abort_t r);

#endif /* VFO_PTT_FSM_H */
