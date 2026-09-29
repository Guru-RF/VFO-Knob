#include "ptt_fsm.h"

#include <string.h>

/* DRV2605 ROM library effect ids used by the PTT path.
 *
 * Nothing plays while the transmitter might be on the air -- from the tap
 * that keys it until the radio confirms receive. The motor sits millimetres
 * from the knob's microphone, and a click there went out with the operator's
 * voice. So there is no click on keying and no still-keyed reminder; what is
 * left is felt only once the microphone is off: a refusal, and the unkey. */
#define EFF_UNKEYED       4   /* Sharp Click                                 */
#define EFF_REFUSED      12   /* Triple Click                                */
#define PRIO_PTT        255

static inline uint32_t since(uint32_t now, uint32_t then) { return now - then; }

static void enter(ptt_fsm_t *f, ptt_state_t s, uint32_t now)
{
    f->state      = s;
    f->t_state_ms = now;
}

/* Enter the teardown ladder at `rung` and emit that rung's action.
 *
 * Rung 1 asks nicely. Rungs 2-4 do not: AetherSDR calls abortTciPtt() when a
 * client that owns PTT disconnects, so tearing down our own socket is a more
 * reliable unkey than any command we can send. */
static void start_ladder(ptt_fsm_t *f, uint8_t rung, uint32_t now, ptt_out_t *out)
{
    enter(f, PTT_RELEASING, now);
    f->rung      = rung;
    f->t_rung_ms = now;
    switch (rung) {
    case 1:  out->send_unkey     = true; break;
    case 2:  out->close_socket   = true; break;
    case 3:  out->destroy_socket = true; break;
    default: out->restart        = true; break;
    }
}

void ptt_fsm_init(ptt_fsm_t *f)
{
    if (!f) return;
    memset(f, 0, sizeof *f);
    f->state = PTT_IDLE;
}

bool ptt_is_tx(const ptt_fsm_t *f)
{
    /* Confirmed on air. Callers wanting "possibly radiating" -- for example to
     * suppress vfo: sends -- should test state != PTT_IDLE instead. */
    return f && f->state == PTT_ON;
}

void ptt_fsm_event(ptt_fsm_t *f, ptt_ev_t ev, uint32_t now_ms,
                   uint32_t permit, ptt_out_t *out)
{
    if (!f || !out) return;
    memset(out, 0, sizeof *out);

    switch (ev) {

    case PTT_EV_TAP_KEY:
        if (f->state != PTT_IDLE) break;          /* use TAP_UNKEY to stop */

        if ((permit & PERMIT_ALL) != PERMIT_ALL) {
            f->refusals++;
            out->haptic = EFF_REFUSED; out->haptic_prio = PRIO_PTT;
            out->refused = true;
            out->missing = PERMIT_ALL & ~permit;
            break;
        }
        enter(f, PTT_REQ_ON, now_ms);
        f->reason      = PTT_AB_NONE;
        out->send_key  = true;
        break;

    case PTT_EV_TAP_UNKEY:
        if (f->state == PTT_ON || f->state == PTT_REQ_ON) {
            f->reason = PTT_AB_OPERATOR;
            start_ladder(f, 1, now_ms, out);
        }
        break;

    case PTT_EV_CONFIRM_TRUE:
        if (f->state == PTT_REQ_ON) {
            enter(f, PTT_ON, now_ms);
            f->rung          = 0;
            out->entered_tx  = true;         /* on the air: the red screen says so */
        }
        /* In IDLE this is someone else keying -- the PC operator, or our own
         * dead previous session. The caller raises the alarm; we must NOT try
         * to unkey it, because a non-owner's trx:false only touches its own
         * producer handle (TciServer.cpp:2429). */
        break;

    case PTT_EV_CONFIRM_FALSE:
        /* One frame, three meanings, disambiguated purely by our own state.
         * Seven distinct server-side refusal causes all collapse to this. */
        if (f->state == PTT_REQ_ON) {
            f->refusals++;
            enter(f, PTT_IDLE, now_ms);
            out->refused     = true;
            out->haptic      = EFF_REFUSED;
            out->haptic_prio = PRIO_PTT;
        } else if (f->state == PTT_ON) {
            f->reason        = PTT_AB_REMOTE;
            enter(f, PTT_IDLE, now_ms);
            out->left_tx     = true;
            out->haptic      = EFF_REFUSED;
            out->haptic_prio = PRIO_PTT;
        } else if (f->state == PTT_RELEASING) {
            f->rung          = 0;
            enter(f, PTT_IDLE, now_ms);
            out->left_tx     = true;
            out->haptic      = EFF_UNKEYED;
            out->haptic_prio = PRIO_PTT;
        }
        break;

    case PTT_EV_TICK:
        if (f->state == PTT_REQ_ON) {
            if (since(now_ms, f->t_state_ms) >= PTT_CONFIRM_MS) {
                f->refusals++;
                enter(f, PTT_IDLE, now_ms);
                out->refused     = true;
                out->haptic      = EFF_REFUSED;
                out->haptic_prio = PRIO_PTT;
            }
        } else if (f->state == PTT_RELEASING) {
            uint32_t el = since(now_ms, f->t_rung_ms);
            if (f->rung == 1 && el >= PTT_RUNG1_MS) start_ladder(f, 2, now_ms, out);
            else if (f->rung == 2 && el >= PTT_RUNG2_MS) start_ladder(f, 3, now_ms, out);
            else if (f->rung == 3 && el >= PTT_RUNG3_MS) start_ladder(f, 4, now_ms, out);
        }
        break;

    case PTT_EV_ABORT:
        /* Reason and entry rung are chosen by ptt_fsm_abort(). */
        break;
    }
}

void ptt_fsm_abort(ptt_fsm_t *f, ptt_abort_t reason, uint32_t now_ms,
                   ptt_out_t *out)
{
    if (!f || !out) return;
    memset(out, 0, sizeof *out);

    if (f->state == PTT_IDLE) return;
    if (f->state == PTT_RELEASING) return;   /* already tearing down */

    f->reason = reason;

    uint8_t rung;
    switch (reason) {
    case PTT_AB_PONG_STALE:
        /* Do not spend rung 1 asking. If any path to the server survives, the
         * close frame gets there and abortTciPtt() runs in milliseconds; if it
         * does not, rung 1 was never going to arrive either. */
        rung = 2;
        break;
    case PTT_AB_LINK_DOWN:
        /* Neither a command nor a clean close can reach anyone. Drop straight
         * to tearing the socket down at the stack level. */
        rung = 3;
        break;
    default:
        rung = 1;
        break;
    }
    /* Still on the air, so no haptic: the operator feels the unkey, or the
     * refusal, once the radio says it is back on receive. */
    start_ladder(f, rung, now_ms, out);
}

const char *ptt_state_name(ptt_state_t s)
{
    switch (s) {
    case PTT_IDLE:      return "IDLE";
    case PTT_REQ_ON:    return "REQ_ON";
    case PTT_ON:        return "ON";
    case PTT_RELEASING: return "RELEASING";
    }
    return "?";
}

const char *ptt_abort_name(ptt_abort_t r)
{
    switch (r) {
    case PTT_AB_NONE:        return "none";
    case PTT_AB_OPERATOR:    return "operator";
    case PTT_AB_PONG_STALE:  return "link stale";
    case PTT_AB_LINK_DOWN:   return "link down";
    case PTT_AB_TOUCH_FAULT: return "touch fault";
    case PTT_AB_REMOTE:      return "remote";
    }
    return "?";
}
