/* Transmit audio: PDM microphone -> TCI TX_AUDIO.
 *
 * TCI transmit is PACED BY THE SERVER, not free-running. AetherSDR sends a
 * header-only TX_CHRONO frame (type 3) every 21.33 ms asking for 1024 stereo
 * frames at 48 kHz, and the client answers each one with a TX_AUDIO frame
 * (type 2). Sending unsolicited audio, or answering late, shows up as a chrono
 * stall on the server side.
 *
 * We answer at 24 kHz, which the server resamples 1:1 to the radio's native
 * DAX TX rate. The payload is int16 mono, which AetherSDR converts and
 * upmixes itself. It used to be float32 duplicated stereo, as WSJT-X sends:
 * four times the bytes, 4 kB a frame against a 5.7 kB TCP send buffer, so
 * nearly every send waited on the computer's acknowledgement.
 *
 * Capture only runs while keyed. A microphone that is live when the operator
 * has not asked to transmit is a bug with privacy consequences, not just a
 * wasted buffer.
 */
#ifndef AUDIO_IN_H
#define AUDIO_IN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#if VFO_RADIO_SVXCONNECT
#define TX_AUDIO_RATE_HZ 16000     /* SvxLink's rate: see audio_out.h */
#else
#define TX_AUDIO_RATE_HZ 24000
#endif
/* 21.33 ms at 24 kHz = 512 mono samples. */
#define TX_CHRONO_FRAMES 512

esp_err_t audio_in_init(void);

/* Start/stop capture. Called from the PTT path. */
void audio_in_set_active(bool on);
bool audio_in_active(void);

/* Fill `out` with `samples` int16 mono samples, padding a short read with
 * silence. Returns false if the microphone has nothing to give yet -- at the
 * start of an over it holds back until it has a cushion -- in which case the
 * caller should send silence rather than nothing: a gap is worse than quiet. */
bool audio_in_take(int16_t *out, size_t samples);

void audio_in_set_gain(uint8_t percent);

typedef struct { uint32_t blocks, starved, overruns; float peak; } audio_in_stats_t;
void audio_in_stats(audio_in_stats_t *st);

#endif /* AUDIO_IN_H */
