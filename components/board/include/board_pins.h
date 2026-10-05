/* Waveshare ESP32-S3-Knob-Touch-LCD-1.8 pin map.
 *
 * THE ONLY FILE IN THIS PROJECT THAT MAY CONTAIN GPIO NUMBERS.
 *
 * Verified against the vendor BSP sources, not guessed. Anything still
 * uncertain is marked and resolved by a named bring-up milestone.
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include "driver/gpio.h"

/* --- display: ST77916 over QSPI ------------------------------------------
 * The panel is an IPS with a real backlight on GPIO47, which is why it is
 * ST77916 and not the SH8601 AMOLED controller some vendor sample code uses
 * as a generic QSPI transport. Confirmed at M4 by ramping BL and watching for
 * a brightness change.
 *
 * Waveshare ships TWO panel revisions with different init tables; read ID
 * register 0x04 at init and select.                                        */
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_H_RES         360
#define BOARD_LCD_V_RES         360
#define BOARD_PIN_LCD_CS        GPIO_NUM_14
#define BOARD_PIN_LCD_PCLK      GPIO_NUM_13
#define BOARD_PIN_LCD_DATA0     GPIO_NUM_15
#define BOARD_PIN_LCD_DATA1     GPIO_NUM_16
#define BOARD_PIN_LCD_DATA2     GPIO_NUM_17
#define BOARD_PIN_LCD_DATA3     GPIO_NUM_18
#define BOARD_PIN_LCD_RST       GPIO_NUM_21
#define BOARD_PIN_LCD_BL        GPIO_NUM_47

/* These are GPIO-matrix pins, not SPI2 IO_MUX pins, and IDF only guarantees
 * IO_MUX-equivalent timing to 40 MHz. Marginality here is temperature
 * dependent -- it passes on the bench and fails in a warm shack -- so M4 and
 * M18 both include a warm soak. Fall back to 26 or 20 MHz on the first
 * artefact; with partial rendering a digit update is 0.3 ms at 40 MHz and
 * 0.6 ms at 20 MHz, so the reduction is nearly free. */
#define BOARD_LCD_PCLK_HZ       (40 * 1000 * 1000)

/* --- shared I2C bus ------------------------------------------------------
 * Touch and haptics share one bus. Worst-case utilisation is under 1.5%, so
 * this is not a constraint -- but every DRV2605 transaction must carry a
 * finite timeout, or a NACKing haptic chip becomes an unbounded stall on the
 * touch path, and touch is the PTT input. Haptics are expendable; touch is
 * not. */
#define BOARD_I2C_PORT          I2C_NUM_0
#define BOARD_PIN_I2C_SDA       GPIO_NUM_11
#define BOARD_PIN_I2C_SCL       GPIO_NUM_12
#define BOARD_I2C_HZ            400000

#define BOARD_I2C_ADDR_TOUCH    0x15    /* CST816  */
#define BOARD_I2C_ADDR_HAPTIC   0x5A    /* DRV2605 */

#define BOARD_PIN_TOUCH_RST     GPIO_NUM_10
#define BOARD_PIN_TOUCH_INT     GPIO_NUM_9

/* --- knob input --------------------------------------------------------
 * NOT a quadrature encoder. Two independent active-low momentary contacts,
 * one per direction: right pulses A, left pulses B, never both at once.
 * Confirmed on hardware -- quadrature states 00 and 10 are never visited.
 * Direction comes from which line pulsed, so PCNT quadrature decoding yields
 * a net count of exactly zero. See hal_encoder.h.                          */
#define BOARD_PIN_ENC_A         GPIO_NUM_8
#define BOARD_PIN_ENC_B         GPIO_NUM_7

/* --- audio (v2) ----------------------------------------------------------
 * GPIO0 selects the PCM5100A audio mux and must be driven HIGH. It is also
 * the BOOT strap, so it is NOT available as a user button -- and since the
 * only other button is inside the CNC case, THIS BOARD HAS NO USABLE BUTTON.
 * Touch is the only non-rotary input, which is what drives the whole
 * interaction design. */
#define BOARD_PIN_AUDIO_MUX_SEL GPIO_NUM_0

/* PCM5100A stereo DAC -> 3.5 mm jack, I2S standard mode. GPIO0 must be HIGH
 * for the ESP32-S3 (rather than the secondary ESP32) to drive it. */
#define BOARD_PIN_I2S_BCLK      GPIO_NUM_39
#define BOARD_PIN_I2S_WS        GPIO_NUM_40
#define BOARD_PIN_I2S_DOUT      GPIO_NUM_41

/* PDM microphone, for v2 transmit audio. Not initialised. */
#define BOARD_PIN_PDM_CLK       GPIO_NUM_45
#define BOARD_PIN_PDM_DATA      GPIO_NUM_46

/* --- the second chip -------------------------------------------------------
 * The board's other MCU, an ESP32 (ESP32-U4WDH) with classic Bluetooth, on a
 * UART of its own (Waveshare's schematic: nets ESP32S3_TX and ESP32S3_RX, its
 * IO18 and IO23). It runs the companion firmware (companion/): a Bluetooth
 * headset's audio gateway. See components/bt_link. */
#define BOARD_PIN_COMPANION_TX  GPIO_NUM_38
#define BOARD_PIN_COMPANION_RX  GPIO_NUM_48

/* The microSD card (TF-018): 4-bit SDMMC, 10 kOhm pull-ups on the board, no
 * card detect. Waveshare's demo keeps its pictures on it; the knob, its
 * firmware images (components/sd_cache). */
#define BOARD_PIN_SD_CLK  GPIO_NUM_4
#define BOARD_PIN_SD_CMD  GPIO_NUM_3
#define BOARD_PIN_SD_D0   GPIO_NUM_5
#define BOARD_PIN_SD_D1   GPIO_NUM_6
#define BOARD_PIN_SD_D2   GPIO_NUM_42
#define BOARD_PIN_SD_D3   GPIO_NUM_2

/* The board's 5 V rail, halved by two 10 kOhm resistors (R62, R63) into ADC1
 * channel 0: Waveshare's BATT_ADC. On USB the rail is the cable's 5 V; on
 * the battery, whatever the base board under CN1 passes on from it -- that
 * board's schematic is not published. */
#define BOARD_PIN_BATT_ADC GPIO_NUM_1

#endif /* BOARD_PINS_H */
