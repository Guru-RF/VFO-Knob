/* The tap's block (audio_out.c's mix_block): the two sources' mix, each
 * levelled and the balance's weights given, as a Bluetooth headset or
 * speaker takes it -- 16-bit stereo, before the volume.
 *
 * At full scale, then, where the jack at its volume has room to spare: the
 * leveller holds a weak source's gain, up to 8x, until a strong one that
 * starts has been heard a few blocks, and that strong one's first tens of
 * milliseconds pass full scale. Clipped, they crack. So a block past full
 * scale comes down whole, by 32767 over its peak; one within it goes as it
 * is, sample for sample what it always was. Plain C: test/host checks it on
 * the PC. */
#ifndef MIX_TAP_H
#define MIX_TAP_H

#include <stdint.h>

/* `n` samples of the mix into `out`. */
static inline void mix_tap(int16_t *out, const float *mix, int n)
{
    float hi = 0.0f, lo = 0.0f;
    for (int i = 0; i < n; i++) {
        if (mix[i] > hi) hi = mix[i];
        if (mix[i] < lo) lo = mix[i];
    }
    const float k = hi > 32767.0f || lo < -32768.0f ? 32767.0f / (hi > -lo ? hi : -lo) : 1.0f;
    for (int i = 0; i < n; i++) {
        const float v = k * mix[i];
        out[i] = v > 32767.0f ? 32767 : (v < -32768.0f ? -32768 : (int16_t)v);
    }
}

#endif /* MIX_TAP_H */
