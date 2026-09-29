#pragma once

/* The RF.Guru logo's gold, for whatever outside the splash wears the brand:
 * the power bar in transmit. */
#define RFG_GOLD_HEX 0xE9B61D

/* Shows the RF.Guru boot splash over the already-built main screen and fades
 * through to it after a few seconds. Call with the LVGL port lock held. */
void ui_splash_start(void);
