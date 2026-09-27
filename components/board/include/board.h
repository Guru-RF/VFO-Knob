#ifndef BOARD_H
#define BOARD_H

#include "esp_err.h"
#include "driver/i2c_master.h"

/* Brings up the shared I2C bus and the few pins that must be in a known state
 * from boot. Call once, first. */
esp_err_t board_init(void);

i2c_master_bus_handle_t board_i2c(void);

#endif /* BOARD_H */
