/* The headset: classic Bluetooth's hands-free profile, this chip as its audio
 * gateway -- the phone's side of it. */
#ifndef HFP_H
#define HFP_H

#include <stdbool.h>
#include <stdint.h>

void hfp_init(void);
/* Calls the headset when it should be, and opens its audio: every 100 ms. */
void hfp_tick(void);
/* A frame from the knob for the headset (commands, and its audio). */
void hfp_on_frame(uint8_t type, const uint8_t *p, uint16_t n);
void hfp_report_state(void);
/* The knob said hello, asking -- it started, or lost this chip for 15 s:
 * counted for the history logged at each audio open. */
void hfp_knob_hello(void);

/* For an update of this chip's own firmware (upd.c), which only comes in
 * while nothing goes on with a headset. Idle: no link to one, being made or
 * up, no scan, no audio, no call. */
bool hfp_idle(void);
/* Idle -- and if so, from now on not calling the headset either: one look
 * under the state's lock, so the tick cannot start a call in between. A
 * headset that calls this chip still connects, and stops the update. */
bool hfp_try_hold(void);
/* Off: a call that came due meanwhile goes out a second later. */
void hfp_hold(bool on);
/* The headset's audio is open. */
bool hfp_audio_open(void);

#endif /* HFP_H */
