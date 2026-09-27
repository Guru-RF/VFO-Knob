/* Round 360x360 VFO display.
 *
 * Geometry: 21.5% of a square layout falls off this glass, concentrated exactly
 * where rectangular conventions put the important things. Every widget is kept
 * inside r=168 (12 px of slack for bezel and lens misalignment).
 *
 * The frequency readout is NINE SEPARATE LABELS on a fixed pitch, not one
 * label. A single label invalidates the whole 322x76 band -- about 49 kB and
 * 2.5 ms of QSPI -- on every detent; per-digit touches roughly 6 kB. That is
 * the single most important implementation decision on this screen.
 */
#ifndef VFO_UI_H
#define VFO_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t ui_init(void);

typedef struct {
    int64_t  freq_hz;
    int32_t  step_hz;
    uint8_t  accel_mult;
    const char *mode;
    int32_t  filt_lo, filt_hi;
    float    smeter_dbm;
    bool     tx;
    bool     link_ok;
    bool     slice_locked;
    uint32_t tot_remain_ms;
    bool     may_key;
} ui_state_t;

void ui_update(const ui_state_t *st);

/* Step chosen by tapping a frequency digit. Returns 0 if nothing changed. */
int32_t ui_take_step_request(void);

/* A tap landed on the PTT pill. Consumed by the caller. */
bool ui_take_ptt_tap(void);

#endif /* VFO_UI_H */
