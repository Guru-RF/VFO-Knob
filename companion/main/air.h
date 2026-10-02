/* The air between this chip and the headset, measured: what reaches us of the
 * headset's packets, what of ours the controller sent, the link the audio got,
 * and its signal. The measuring changes nothing that is sent or heard. The
 * two settings the measurements led to do: this chip runs the link
 * (air_prefer_master()), and keeps it awake through a call -- in the wraps
 * air.c puts in front of the stack's audio open and close (CMakeLists.txt).
 * Drop those two and a call sleeps at the stack's sniff intervals again,
 * with the losses of 2026-10-02. See air.c. */
#ifndef AIR_H
#define AIR_H

#include <stdbool.h>
#include <stdint.h>

/* Once, after esp_bluedroid_enable(): on a connection the headset makes,
 * this chip asks to be the link's master, as it is on one it makes itself. */
void air_prefer_master(void);

/* All but air_signal() are for the main task (hfp.c's reports): never one of
 * the stack's, short of stack for a log line, nor the audio's pump. */

/* At each audio open: the signal asked, and its numbers from now on. */
void air_opened(const uint8_t *bda);
/* ... and the link it got, logged. */
void air_log_link(void);
/* The answer to the signal's question (ESP_BT_GAP_READ_RSSI_DELTA_EVT). */
void air_signal(bool ok, int8_t delta);
/* A moment before each report: the signal, asked again. */
void air_ask_signal(void);
/* A report's numbers, taken now: what came from the headset and went to it
 * since the report before, or since the link opened -- up to its close, once
 * it closed. With hfp.c's frames heard and made since the audio opened.
 * False when there is nothing to report: the link's last numbers, to its
 * close, were taken already. */
bool air_take(uint32_t frames_in, uint32_t frames_out);
/* ... logged, a line each. */
void air_log_from(void);
void air_log_to(void);

#endif /* AIR_H */
