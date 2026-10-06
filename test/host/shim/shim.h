/* The harness's view of the shim (shim.c). */
#ifndef SHIM_H
#define SHIM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void shim_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool shim_timers_idle(void);
extern uint32_t          shim_nvs_writes;
extern volatile uint64_t shim_sdr_samples;
extern volatile bool     shim_sdr_on;
extern volatile uint64_t shim_samples;
extern volatile uint32_t shim_flushes;
/* The played audio's pitch: upward crossings of zero since the reset, per
 * second of it at 24 kHz; and the knob's clock, s. */
void   shim_pitch_reset(void);
double shim_pitch(uint64_t *samples);
double shim_now(void);
/* The same for the second receiver's audio, and its ring: its level, the
 * feeds it let go, the times it ran dry. */
void   shim_sdr_pitch_reset(void);
double shim_sdr_pitch(uint64_t *samples);
void   shim_sdr_audio(size_t *level, uint32_t *dropped, uint32_t *underruns);
#endif
