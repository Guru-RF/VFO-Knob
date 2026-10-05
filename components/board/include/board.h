#ifndef BOARD_H
#define BOARD_H

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "knob_batt.h"

/* Brings up the shared I2C bus and the few pins that must be in a known state
 * from boot. Call once, first. */
esp_err_t board_init(void);

i2c_master_bus_handle_t board_i2c(void);

/* The 5 V rail's voltage through BATT_ADC: the cable's on USB, the battery's
 * (as the base board passes it on) without. board_power_mv() is the rail in
 * mV, or -1 if the ADC is not up -- nor is it without the chip's calibration;
 * it takes a few hundred microseconds, from one task at a time --
 * board_power_poll()'s. */
esp_err_t board_power_init(void);
int       board_power_mv(void);

/* The knob's own power, from the rail (knob_batt.h): on USB, or on the
 * battery and its charge. board_power_poll() reads the rail and feeds the
 * gauge, at most once a second, from one task -- the ui task's loop -- and
 * is true when what is shown changed; board_power_get() hands any task
 * what it last read. */
typedef struct {
    uint8_t src;            /* KNOB_PWR_*: UNKNOWN until the first readings settle */
    int8_t  pct;            /* the charge shown, 0-100 in fives; -1 none (USB, unknown) */
    int16_t mv;             /* the rail's last reading, mV; -1 none */
    int16_t smooth_mv;      /* on the battery, the rail smoothed, mV; -1 otherwise */
} board_power_t;
bool board_power_poll(void);
void board_power_get(board_power_t *out);

#endif /* BOARD_H */
