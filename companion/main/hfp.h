/* The device -- a headset (classic Bluetooth's hands-free profile, this chip
 * as its audio gateway: the phone's side of it) or a speaker (A2DP, a2dp.c)
 * -- one at a time. */
#ifndef HFP_H
#define HFP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void hfp_init(void);
/* Calls the device when it should be, and opens its audio: every 100 ms. */
void hfp_tick(void);
/* A frame from the knob for the device (commands, and its audio). */
void hfp_on_frame(uint8_t type, const uint8_t *p, uint16_t n);
void hfp_report_state(void);
/* The knob said hello, asking -- it started, or lost this chip for 15 s:
 * counted for the history logged at each audio open; and a speaker's volume
 * is no longer the knob's until it says it again (a2dp.c). */
void hfp_knob_hello(void);
/* Each of the knob's hellos, asking or not: its flags (BTL_HELLO_*). Without
 * SPEAKERS every device is a headset to it, as before. */
void hfp_knob_flags(uint8_t flags);

/* A speaker's A2DP, from a2dp.c on the stack's tasks. Its link: BTL_LINK_*,
 * and the packets it takes. */
void hfp_av_conn(const uint8_t *bda, uint8_t link, uint16_t mtu);
/* Its stream started or stopped, as the stack says. */
void hfp_av_audio(const uint8_t *bda, bool started);
/* The answer to a2dp_start() or a2dp_suspend() (A2DP_START, A2DP_SUSPEND). */
void hfp_av_media(uint8_t cmd, bool ok);
/* The sink's delay report, 1/10 ms -- on the stack's BTU task. */
void hfp_av_delay(uint16_t v);
/* n samples of the knob's audio for the speaker, at 44.1 kHz: the stack's
 * media tick asks, on its BTC task. Silence while its stream is not open. */
void hfp_dn_pull(int16_t *out, size_t n);

/* For an update of this chip's own firmware (upd.c), which only comes in
 * while nothing goes on with a device. Idle: no link to one -- hands-free or
 * A2DP -- being made or up, no scan, no audio, no call. */
bool hfp_idle(void);
/* Idle -- and if so, from now on not calling the device either: one look
 * under the state's lock, so the tick cannot start a call in between. A
 * device that calls this chip still connects, and stops the update. */
bool hfp_try_hold(void);
/* Off: a call that came due meanwhile goes out a second later. */
void hfp_hold(bool on);
/* The device's audio is open: a headset's call, or a speaker's stream. */
bool hfp_audio_open(void);

#endif /* HFP_H */
