/* Knob input for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
 *
 * IMPORTANT: this is NOT a quadrature encoder, despite the pin names. It is a
 * "bidirectional switch knob" -- two INDEPENDENT active-low momentary contacts,
 * one per direction. Turning right pulses A; turning left pulses B. There is no
 * phase relationship between them and they are never low at the same time.
 *
 * Verified on hardware: while turning one way, A produced a clean 26%-duty
 * pulse train and B stayed high, and the quadrature states 00 and 10 were
 * NEVER visited. Decoding this as quadrature yields alternating +1/-1 and a net
 * count of exactly zero, which is what the first attempt did.
 *
 * Direction therefore comes from WHICH line pulsed, not from phase. That also
 * rules out hardware PCNT decoding: its glitch filter tops out around 12 us,
 * far short of the millisecond-scale bounce of a mechanical contact.
 *
 * We use an edge ISR with a per-line timestamp lockout rather than the vendor's
 * 3 ms polling loop, because 3 ms of quantisation is visible in haptic timing
 * and the click must feel bound to the detent.
 */
#ifndef HAL_ENCODER_H
#define HAL_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* One contact closure per detent per direction. */
#define ENC_COUNTS_PER_DETENT_DEFAULT 1

/* Debounce by MINIMUM CONTACT DURATION, not by an edge lockout.
 *
 * An edge lockout accepts the FIRST edge of a bounce burst -- which may be the
 * glitch itself -- and then masks the genuine pulse behind it. Measured on this
 * hardware that produced impossible 548 detents/s peaks and fired the B contact
 * 64 times during a right-hand turn. Requiring the line to stay low for several
 * consecutive polls rejects narrow glitches outright, because a glitch cannot
 * hold the line down.
 *
 * This is the vendor's algorithm (bidi_switch_knob.c: count on release, only if
 * the line was low for >= DEBOUNCE_TICKS). They poll at 3 ms and need 2 ticks;
 * we poll at 1 ms and need 4 -- same rejection, a quarter of the quantisation,
 * which matters because 3 ms is visible in haptic timing.
 */
#define ENC_POLL_MS        1
#define ENC_MIN_LOW_TICKS  4

/* Directional lockout. The two contacts encode OPPOSITE directions, so they
 * should never fire together -- yet a measured left-hand revolution produced 4
 * spurious right detents out of 36. Those survived the duration filter, so they
 * are genuine sustained activity on the other contact, not electrical glitches,
 * and only a direction rule can reject them.
 *
 * 8 ms is safe: it would take ~125 detents/s to reverse legitimately inside the
 * window, and the fastest hand spin measured here was 29. */
#define ENC_DIR_LOCKOUT_TICKS 8

typedef struct {
    uint32_t raw_a, raw_b;            /* every transition seen          */
    uint32_t accepted_a, accepted_b;  /* survived the duration filter   */
    uint32_t rejected;                /* too brief to be a real detent  */
    uint32_t reversals;               /* rejected by the direction lockout */
} enc_stats_t;

esp_err_t hal_encoder_init(void);

/* Call every ENC_POLL_MS from exactly one task. Returns this tick's signed
 * delta: normally 0, occasionally +/-1. */
int32_t hal_encoder_poll(void);

/* Net detents since boot: right positive, left negative. */
int32_t hal_encoder_count(void);

/* Signed delta since the previous call to this function. */
int32_t hal_encoder_read_delta(void);

/* raw vs accepted vs rejected IS the bounce measurement. accepted_b climbing
 * during a right-hand turn means the duration filter is still too permissive. */
void hal_encoder_stats(enc_stats_t *st);

#endif /* HAL_ENCODER_H */
