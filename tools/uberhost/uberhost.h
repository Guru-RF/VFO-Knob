/* Between the harness (main.c) and the PC's stand-ins for ESP-IDF (stubs.c). */
#ifndef UBERHOST_H
#define UBERHOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern int uh_verbose;                 /* ESP_LOGD and ESP_LOGV too */
extern int uh_tls_inits;               /* esp_tls_init() calls: none in the clear */

/* Whether the knob's log has said this, since the start. */
bool uh_logged(const char *needle);

/* What reached the jack: the radio's samples (audio_out_feed_pcm16) and the
 * web SDR's (audio_out_feed_sdr), and their RMS. */
void uh_audio(uint64_t *radio, double *radio_rms, uint64_t *sdr, double *sdr_rms);

/* The last picture handed to the PNG decoder: its bytes and size. */
void uh_png_last(size_t *n, int *w, int *h);

#endif /* UBERHOST_H */
