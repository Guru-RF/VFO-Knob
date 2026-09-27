/* DRV2605L haptic driver -- registers only, zero policy.
 *
 * Effect selection, rate limiting and the velocity-aware governor all live in
 * haptic_engine/. This layer just does what it is told, quickly, with a finite
 * I2C timeout on every transaction: the touch controller shares this bus, and
 * touch is the PTT input, so a NACKing haptic chip must never stall it.
 * Haptics are expendable; touch is not.
 *
 * Register semantics per TI SLOS854D. Note DIAG_RESULT (STATUS bit 3) is
 * 0 = PASS -- several third-party drivers have this inverted.
 */
#ifndef DRV2605_H
#define DRV2605_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef enum {
    DRV_ACTUATOR_ERM = 0,
    DRV_ACTUATOR_LRA = 1,
} drv_actuator_t;

/* ROM waveform libraries. A..E are ERM libraries of increasing rated voltage;
 * 6 is the LRA library. Library B gives a 5-15 ms brake against A's 20-40 ms,
 * and braking is what turns a buzz into a click. */
typedef enum {
    DRV_LIB_EMPTY = 0,
    DRV_LIB_A     = 1,
    DRV_LIB_B     = 2,
    DRV_LIB_C     = 3,
    DRV_LIB_D     = 4,
    DRV_LIB_E     = 5,
    DRV_LIB_LRA   = 6,
} drv_library_t;

typedef struct {
    uint8_t rated_voltage;   /* reg 0x16 */
    uint8_t od_clamp;        /* reg 0x17 */
    uint8_t a_cal_comp;      /* reg 0x18 */
    uint8_t a_cal_bemf;      /* reg 0x19 */
    uint8_t feedback_ctrl;   /* reg 0x1A, carries BEMF_GAIN */
    bool    valid;
} drv_calibration_t;

typedef struct {
    i2c_master_dev_handle_t dev;
    drv_actuator_t          actuator;
    drv_library_t           library;
    uint8_t                 last_effect;  /* lets fire() skip a redundant write */
    bool                    faulted;      /* latched after repeated I2C errors  */
    uint8_t                 err_streak;
} drv2605_t;

esp_err_t drv2605_init(drv2605_t *d, i2c_master_bus_handle_t bus,
                       uint8_t addr, drv_actuator_t actuator);

/* Run auto-calibration. On success `cal` is filled and can be cached in NVS so
 * later boots skip this entirely -- it is the only slow, failure-prone step in
 * haptic bring-up. Returns ESP_ERR_INVALID_RESPONSE if DIAG_RESULT reports a
 * failure, which usually means the actuator type is wrong. */
esp_err_t drv2605_autocal(drv2605_t *d, drv_calibration_t *cal);

/* Restore a cached calibration instead of re-running it. */
esp_err_t drv2605_apply_cal(drv2605_t *d, const drv_calibration_t *cal);

esp_err_t drv2605_set_library(drv2605_t *d, drv_library_t lib);

/* Play one ROM effect (1..123). */
esp_err_t drv2605_fire(drv2605_t *d, uint8_t effect);

/* Real-time playback: drive the actuator at an explicit amplitude. Used by the
 * M3 actuator-identification test and by velocity-scaled clicks. */
esp_err_t drv2605_rtp_begin(drv2605_t *d);
esp_err_t drv2605_rtp_write(drv2605_t *d, uint8_t amplitude);
esp_err_t drv2605_rtp_end(drv2605_t *d);

esp_err_t drv2605_read_status(drv2605_t *d, uint8_t *status);
bool      drv2605_ok(const drv2605_t *d);

#endif /* DRV2605_H */
