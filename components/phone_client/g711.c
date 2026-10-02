/* G.711, A-law and µ-law: the companding every SIP phone speaks, after the
 * public-domain reference (Sun Microsystems), with the decoders as tables. */
#include "g711.h"

#define SIGN_BIT   0x80
#define QUANT_MASK 0x0F
#define SEG_SHIFT  4
#define SEG_MASK   0x70
#define BIAS       0x84
#define CLIP       8159

static const int16_t SEG_AEND[8] = { 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF };
static const int16_t SEG_UEND[8] = { 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF };

static int16_t s_alaw[256], s_ulaw[256];
static bool s_made;

static int seg_of(int v, const int16_t *end)
{
    for (int i = 0; i < 8; i++)
        if (v <= end[i]) return i;
    return 8;
}

uint8_t g711_alaw_enc(int16_t pcm)
{
    int v = pcm >> 3, mask;
    if (v >= 0) mask = 0xD5;
    else      { mask = 0x55; v = -v - 1; }
    const int seg = seg_of(v, SEG_AEND);
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);
    uint8_t a = (uint8_t)(seg << SEG_SHIFT);
    a |= (seg < 2 ? v >> 1 : v >> seg) & QUANT_MASK;
    return (uint8_t)(a ^ mask);
}

static int16_t alaw_dec(uint8_t a)
{
    a ^= 0x55;
    int t = (a & QUANT_MASK) << 4;
    const int seg = (a & SEG_MASK) >> SEG_SHIFT;
    if (seg == 0)      t += 8;
    else if (seg == 1) t += 0x108;
    else             { t += 0x108; t <<= seg - 1; }
    return (int16_t)((a & SIGN_BIT) ? t : -t);
}

uint8_t g711_ulaw_enc(int16_t pcm)
{
    int v = pcm >> 2, mask;
    if (v < 0) { v = -v; mask = 0x7F; }
    else         mask = 0xFF;
    if (v > CLIP) v = CLIP;
    v += BIAS >> 2;
    const int seg = seg_of(v, SEG_UEND);
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);
    const uint8_t u = (uint8_t)((seg << 4) | ((v >> (seg + 1)) & 0xF));
    return (uint8_t)(u ^ mask);
}

static int16_t ulaw_dec(uint8_t u)
{
    u = (uint8_t)~u;
    int t = ((u & QUANT_MASK) << 3) + BIAS;
    t <<= (u & SEG_MASK) >> SEG_SHIFT;
    return (int16_t)((u & SIGN_BIT) ? (BIAS - t) : (t - BIAS));
}

static void make(void)
{
    for (int i = 0; i < 256; i++) {
        s_alaw[i] = alaw_dec((uint8_t)i);
        s_ulaw[i] = ulaw_dec((uint8_t)i);
    }
    s_made = true;
}

void g711_encode(uint8_t pt, const int16_t *pcm, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) out[i] = pt == 8 ? g711_alaw_enc(pcm[i]) : g711_ulaw_enc(pcm[i]);
}

void g711_decode(uint8_t pt, const uint8_t *in, int16_t *pcm, int n)
{
    if (!s_made) make();
    const int16_t *t = pt == 8 ? s_alaw : s_ulaw;
    for (int i = 0; i < n; i++) pcm[i] = t[in[i]];
}

uint8_t g711_silence(uint8_t pt) { return pt == 8 ? 0xD5 : 0xFF; }
