/* RX audio: TCI stream -> PCM5100A -> 3.5 mm jack.
 *
 * Wire format is the ExpertSDR3 TCI v2.0 stream: a 64-byte header of 16
 * little-endian uint32 fields followed by samples. `length` counts REAL
 * samples, so a stereo frame count is length/2.
 *
 * One safety note that governs how this is switched on. AetherSDR's
 * effectiveTrx() redirects a PTT request for trx 0 to whichever receiver the
 * client declared in audio_start. Declaring audio on a DIFFERENT receiver than
 * the one being keyed would therefore transmit on a slice the operator never
 * addressed -- on that slice's band and antenna. We always declare audio on
 * the same trx we control, so the redirect is either a no-op (trx 0 -> 0) or
 * bypassed entirely (a non-zero trx is a deliberate address).
 */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Requested from the server. 24 kHz is plenty for SSB/CW and halves the wire
 * load against 48 kHz; the server accepts only 8/12/24/48. */
#define AUDIO_RATE_HZ   24000
#define AUDIO_CHANNELS  2

typedef struct {
    uint32_t receiver, sample_rate, format, codec, crc, length, type, channels;
    uint32_t reserved[8];
} __attribute__((packed)) tci_audio_hdr_t;

#define TCI_AUDIO_TYPE_RX     1
#define TCI_AUDIO_TYPE_TX     2
#define TCI_AUDIO_TYPE_CHRONO 3
#define TCI_AUDIO_FMT_INT16   0
#define TCI_AUDIO_FMT_FLOAT32 3

esp_err_t audio_out_init(void);

/* Feed one complete binary TCI frame (header + payload). Returns false if the
 * frame was not RX audio, or was dropped because the buffer is full. Safe to
 * call from the WebSocket task: it never blocks. */
bool audio_out_feed(const void *frame, size_t len);

/* Feed plain 16-bit PCM at AUDIO_RATE_HZ, for a radio whose stream is raw
 * samples rather than TCI frames (the IC-705's LAN audio). `frames` counts
 * sample frames; mono is widened to the stereo the I2S runs. Never blocks, so
 * it is safe from the network task; a full buffer drops and counts. */
bool audio_out_feed_pcm16(const int16_t *pcm, size_t frames, uint8_t channels);

/* 0..100. Applied in the playback task, not on the network path. */
void audio_out_set_volume(uint8_t vol);

typedef struct {
    uint32_t frames, dropped, underruns;
    uint32_t sample_rate, format, channels;
} audio_stats_t;
void audio_out_stats(audio_stats_t *st);

#endif /* AUDIO_OUT_H */
