#pragma once

/* Shows the RF.Guru boot splash over the already-built main screen and fades
 * through to it after a few seconds. Call with the LVGL port lock held. */
void ui_splash_start(void);
