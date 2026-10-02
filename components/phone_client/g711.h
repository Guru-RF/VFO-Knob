/* G.711, A-law (RTP payload 8, PCMA) and µ-law (0, PCMU), 8 kHz. */
#ifndef G711_H
#define G711_H

#include <stdbool.h>
#include <stdint.h>

uint8_t g711_alaw_enc(int16_t pcm);
uint8_t g711_ulaw_enc(int16_t pcm);
/* `n` samples, in the payload type's law. */
void g711_encode(uint8_t pt, const int16_t *pcm, uint8_t *out, int n);
void g711_decode(uint8_t pt, const uint8_t *in, int16_t *pcm, int n);
/* The law's code for silence. */
uint8_t g711_silence(uint8_t pt);

#endif /* G711_H */
