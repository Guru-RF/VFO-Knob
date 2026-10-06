#ifndef SHIM_AUDIO_OUT_H
#define SHIM_AUDIO_OUT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define AUDIO_RATE_HZ 24000
/* The second receiver's, into its own ring. */
void   audio_out_sdr(bool on);
bool   audio_out_feed_sdr(const int16_t *pcm, size_t n);
void   audio_out_set_balance(int8_t b);
size_t audio_out_sdr_queued(void);
size_t audio_out_sdr_room(void);
size_t audio_out_sdr_preroll(void);
size_t audio_out_sdr_preroll_max(void);
void   audio_out_sdr_set_preroll(size_t n);
/* The kiwi firmware's receiver plays as a radio does. */
bool   audio_out_feed_pcm16(const int16_t *pcm, size_t frames, uint8_t channels);
void   audio_out_flush(void);
void   audio_out_trim(size_t keep_frames);
size_t audio_out_queued(void);
size_t audio_out_room(void);
size_t audio_out_preroll(void);
size_t audio_out_preroll_max(void);
void   audio_out_set_preroll(size_t frames);
typedef struct {
    uint32_t frames, dropped, underruns;
    uint32_t sample_rate, format, channels;
} audio_stats_t;
void audio_out_stats(audio_stats_t *st);
void audio_out_sdr_stats(audio_stats_t *st);
#endif
