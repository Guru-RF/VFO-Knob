/* PTT state machine -- TOGGLE.
 *
 * Tap to key, tap to unkey. Deliberate to enter (the tap must land on the
 * pill), forgiving to exit (a tap anywhere in the lower region unkeys) -- you
 * should never have to aim carefully to STOP transmitting.
 *
 * Toggle gives up the one safety property momentary PTT has for free: a finger
 * held down is a continuous assertion of intent, and a frozen touch coordinate
 * exposes a hung controller. Neither exists here, so the weight falls on the
 * time-out timer, the pong-based link watchdog, and the teardown ladder.
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
#define PTT_TOT_DEFAULT_MS 120000u
#define PTT_TOT_MIN_MS      30000u
#define PTT_TOT_MAX_MS     600000u
#define PTT_TOT_WARN_MS     10000u   /* haptic warning before expiry      */
#define PTT_KEYED_NAG_MS    10000u   /* "still transmitting" reminder     */

/* Teardown ladder deadlines. */
#define PTT_RUNG1_MS  600u   /* trx:false; on the existing socket */
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
    PTT_AB_TOT,
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
#define PERMIT_TOT_CLEAR     (1u << 5)
#define PERMIT_PONG_FRESH    (1u << 6)
#define PERMIT_NO_RECONCILE  (1u << 7)
#define PERMIT_BAND          (1u << 8)   /* v1: pinned set */
#define PERMIT_MODE          (1u << 9)   /* v1: pinned set */
#define PERMIT_ALL           0x3FFu

typedef struct {
    bool     send_key;
    bool     send_unkey;
    bool     close_socket;      /* rung 2 */
    bool     destroy_socket;    /* rung 3 */
    bool     restart;           /* rung 4 */
    uint8_t  haptic;            /* DRV2605 ROM effect id, 0 = none */
    uint8_t  haptic_prio;
    bool     refused;           /* surface "PTT REFUSED" */
    bool     entered_tx;
    bool     left_tx;
} ptt_out_t;

typedef struct {
    ptt_state_t state;
    uint8_t     rung;           /* 0 = not in the ladder */
    ptt_abort_t reason;
    uint32_t    t_state_ms;     /* when the current state was entered */
    uint32_t    t_key_ms;       /* when TX was confirmed              */
    uint32_t    t_rung_ms;
    uint32_t    t_last_nag_ms;
    uint32_t    tot_ms;
    bool        tot_latched;    /* needs a fresh press to re-arm      */
    bool        tot_warned;
    uint32_t    refusals;
} ptt_fsm_t;

void ptt_fsm_init(ptt_fsm_t *f, uint32_t tot_ms);

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
uint32_t ptt_tot_remaining_ms(const ptt_fsm_t *f, uint32_t now_ms);
const char *ptt_state_name(ptt_state_t s);
const char *ptt_abort_name(ptt_abort_t r);

#endif /* VFO_PTT_FSM_H */
