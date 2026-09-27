/* TCI v2.0 client for AetherSDR.
 *
 * Connects out to ws://<host>:50001 and speaks the same protocol AetherSDR
 * already serves to WSJT-X. Design notes that matter:
 *
 *  - Tuning is OPTIMISTIC. The wire round trip is 30-80 ms with an unbounded
 *    tail (it waits on someone else's Qt event loop), while the glass and the
 *    motor are 10-20 ms. Neither may wait for the wire, so the display leads
 *    and the anti-echo classifier reconciles afterwards.
 *
 *  - There is NO outbound queue, deliberately. The server closes the socket
 *    after 64 queued commands, so a queue would turn an enthusiastic flick
 *    into a disconnection. Instead the sender polls for a difference and is
 *    hard-capped at 20 Hz, which makes that failure structurally impossible
 *    rather than merely unlikely.
 *
 *  - The greeting is authoritative on every (re)connect. Pushing our stale
 *    pre-dropout frequency at a rig the operator has since retuned would be
 *    the rudest possible bug.
 */
#ifndef TCI_CLIENT_H
#define TCI_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    TCI_LINK_DOWN = 0,
    TCI_LINK_CONNECTING,
    TCI_LINK_GREETING,
    TCI_LINK_READY,
    TCI_LINK_DEGRADED,   /* after a backlog close: send rate halved for 60 s */
} tci_link_t;

typedef struct {
    tci_link_t link;
    uint8_t    ptt_state;      /* ptt_state_t */
    uint8_t    ptt_rung;       /* teardown ladder position, 0 = not in it */
    uint8_t    ptt_reason;     /* ptt_abort_t */
    uint32_t   tot_remain_ms;
    uint32_t   permit;         /* PERMIT_* bitmask; all bits = may key */
    uint32_t   ptt_refusals;
    int32_t    pong_age_ms;
    int64_t    f_display;      /* what the glass shows -- optimistic */
    int64_t    f_server;       /* newest authoritative value; never drawn */
    char       mode[8];
    int32_t    filt_lo, filt_hi;
    int32_t    rit_hz;
    float      smeter_dbm;
    float      tx_mic_dbm, tx_fwd_w, tx_swr, tx_alc;
    /* NOTE: AetherSDR currently sends the same cached value for peak as for
     * forward ("peak ~ avg for now"), so these read identically until that is
     * implemented upstream. Wired to the peak field anyway, so it becomes
     * correct without a change here. */
    float      tx_peak_w;
    bool       slice_locked;
    bool       tx;
    uint8_t    my_trx;
    uint8_t    n_trx;
    /* Counters for the status shade; all of these are diagnostics you want
     * when something feels wrong but nothing is obviously broken. */
    uint32_t   connects, closes, reconciles, rejects;
    uint32_t   unknown_cmds, sends, echoes;
    char       last_close[48];
} tci_status_t;

esp_err_t tci_client_start(const char *host, uint16_t port);

/* Called from the knob task. Moves f_display immediately; the wire catches up.
 * Returns the new f_display so the caller can base haptic decisions (MHz
 * rollover, decade decimation) on the SAME frequency the radio is being told
 * about -- a second local copy silently drifts, and then the rollover accent
 * fires on a boundary the operator never crossed. */
int64_t tci_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz);

/* Adopt a step size (tapping a digit, later). Re-latches the anchor. */
void tci_set_step(int32_t step_hz);

/* Stops the RX audio stream while something else needs the link -- currently
 * a firmware upload, which shares the socket and the USB pipe with it. */
void tci_audio_suspend(bool suspend);

void tci_get_status(tci_status_t *out);
bool tci_is_ready(void);

/* PTT is TOGGLE: tap to key, tap to unkey. Deliberate to enter, forgiving to
 * exit -- you should never have to aim carefully to STOP transmitting. */
void tci_ptt_key(void);
void tci_ptt_unkey(void);
void tci_ptt_toggle(void);

/* Force an abort with a specific reason, for testing the teardown ladder. */
void tci_ptt_force_abort(uint8_t reason);

/* Configure the time-out timer, clamped to the FSM's limits. */
void tci_set_tot_ms(uint32_t ms);

/* --- setters for the on-screen editors ---------------------------------
 * All are fire-and-forget. modulation and rx_filter_band are confirmed by the
 * server's own change notifications; rit_offset is confirmed on NO path at
 * all, so the caller must read it back if it wants certainty. */
void tci_set_mode(const char *mode);
void tci_set_filter(int32_t lo, int32_t hi);
void tci_set_rit(int32_t hz);

/* Jump to a frequency (band change). Goes through the same optimistic model
 * and echo ring as knob tuning, so it cannot confuse the anti-echo logic. */
void tci_goto_freq(int64_t hz);

/* NOTE: there is deliberately no tune() here. TCI does expose
 * "tune:<trx>,<bool>;" and it works, but it emits a carrier and therefore
 * needs the same treatment PTT gets -- a timeout, a link-loss abort and a
 * permit mask. A tune button shipped without those left a carrier running at
 * full power. If it comes back, it comes back with all of it. */

#endif /* TCI_CLIENT_H */
