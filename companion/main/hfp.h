/* The headset: classic Bluetooth's hands-free profile, this chip as its audio
 * gateway -- the phone's side of it. */
#ifndef HFP_H
#define HFP_H

#include <stdint.h>

void hfp_init(void);
/* Calls the headset when it should be, and opens its audio: every 100 ms. */
void hfp_tick(void);
/* A frame from the knob for the headset (commands, and its audio). */
void hfp_on_frame(uint8_t type, const uint8_t *p, uint16_t n);
void hfp_report_state(void);

#endif /* HFP_H */
