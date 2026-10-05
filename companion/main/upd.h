/* This chip's own firmware, from the knob over the link (bt_link_proto.h's
 * BTL_UPD_*): taken into the slot not running, checked -- its SHA-256, the
 * image, its signature against the firmware running -- and switched to; then
 * on trial until the knob says it hears it, going back to the firmware before
 * by itself otherwise. What went back, and why, it keeps in NVS (namespace
 * "upd", never the pairing's), for the firmware before to tell the knob. */
#ifndef UPD_H
#define UPD_H

#include <stdbool.h>
#include <stdint.h>

/* app_main, after NVS and before the link and Bluetooth: the trial, the RTC
 * watchdog, what went back and why, the crash guard, the boot story. Plain
 * reads only -- except the crash guard's going back, an image check, which is
 * why the main task has 8 kB. */
void upd_boot(void);
/* For HELLO's UPDATE: this chip takes its firmware over the link. */
bool upd_can_take(void);
/* An update is coming in: paging is held, and the link reads quickly. */
bool upd_active(void);
/* link_rx: the knob's HELLO. The boot story and the INFO, for a knob that
 * may have just started; asking, mid-transfer: the knob restarted. */
void upd_knob_hello(bool ask);
/* link_rx: BTL_UPD_BEGIN .. BTL_UPD_ASK. Fast work only: never the flash,
 * never NVS, never an image check. */
void upd_on_frame(uint8_t type, const uint8_t *p, uint16_t n);
/* hfp: a headset's or a speaker's link came up. */
void upd_headset_came(void);
/* The main loop, every 100 ms: the trial's watchdog, its keeping, its
 * deadline. */
void upd_tick(void);

#endif /* UPD_H */
