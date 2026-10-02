/* G.722, ITU-T Recommendation G.722 (09/2012), 7 kHz audio-coding within
 * 64 kbit/s: sub-band ADPCM, mode 1 only (64 kbit/s; the decoder's 56 and
 * 48 kbit/s variants are left out). Written from the Recommendation alone:
 * clause 5's QMFs and clause 6's lower and higher sub-band ADPCM, block for
 * block under its own names (SUBTRA, QUANTL, INVQAL, LOGSCL, SCALEL, ...),
 * with its tables 11 and 14 to 21.
 *
 * Clause 6's arithmetic, which is bit exact (the Recommendation's digital
 * test sequences hold an ADPCM to it): 16-bit words; "+" and "-" saturate to
 * -32768..32767 -- sat() here; A * B is (A times B) >> 15 -- mul(); ">>" is
 * arithmetic, and by a negative count shifts left.
 *
 * Clause 5 leaves the QMF's precision to the implementation (accumulators of
 * 24 bits or more, at least the 15 bits of its XIN and XOUT; no digital test
 * sequence for it). With 16-bit PCM either side the bit below those 15 is
 * kept, not dropped:
 *  - transmit: a sample s is XIN = s / 2 with its half kept, the products
 *    and sums exact, so y = 29 and LOWT's XL = (XA + XB) >> (y - 15) is
 *    >> 14 -- then held to 15 bits, as LOWT and HIGHT hold XL and XH;
 *  - receive: XOUT and the bit under it, WD >> 11 where XOUT1 = WD >> 12,
 *    saturated to 16 bits.
 * The pairing is the Recommendation's: of two input samples the second is
 * XIN(j), the current one; of two output samples XOUT1 (XD, the even
 * coefficients) comes first. The two QMFs together delay by 22 samples.
 *
 * Beside ffmpeg's adpcm_g722, run as a black box (tools/g722test): its
 * decoder's output is this one's sample for sample, whatever the octets;
 * its encoder's octets are this one's except where LOWT/HIGHT's limit takes
 * hold -- full-scale input, the QMF overshooting 15 bits -- which ffmpeg
 * leaves out.
 *
 * Integer only, no allocation, nothing global but the tables. */
#include "g722.h"

#include <string.h>

_Static_assert((-3 >> 1) == -2, "the Recommendation's >> is an arithmetic shift");

/* "+" and "-": 16-bit, saturating. */
static inline int sat(int x)
{
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

/* "*": A * B = (A times B) >> 15. */
static inline int mul(int a, int b)
{
    return (a * b) >> 15;
}

/* LIMIT (blocks 6L and 5H), and the QMF's own: 15 bits. */
static inline int limit(int x)
{
    return x > 16383 ? 16383 : x < -16384 ? -16384 : x;
}

/* Table 11: the QMF coefficients H0..H23, scaled by 2^13. */
static const int16_t H[24] = {
       3,  -11,  -11,   53,   12, -156,   32,  362, -210, -805,  951, 3876,
    3876,  951, -805, -210,  362,   32, -156,   12,   53,  -11,  -11,    3,
};

/* Table 14, lower sub-band: Q6, the decision levels, by MIL (1..29); QQ6,
 * the 6-bit inverse quantizer's outputs, by IL6 (1..30); QQ4 and WL, the
 * 4-bit one's outputs and the log scale factor's multipliers, by IL4
 * (0..7). */
static const int16_t Q6[30] = {
       0,   35,   72,  110,  150,  190,  233,  276,  323,  370,  422,  473,
     530,  587,  650,  714,  786,  858,  940, 1023, 1121, 1219, 1339, 1458,
    1612, 1765, 1980, 2195, 2557, 2919,
};
static const int16_t QQ6[31] = {
       0,   17,   54,   91,  130,  170,  211,  254,  300,  347,  396,  447,
     501,  558,  618,  682,  750,  822,  899,  982, 1072, 1170, 1279, 1399,
    1535, 1689, 1873, 2088, 2376, 2738, 3101,
};
static const int16_t QQ4[8] = { 0, 150, 323, 530, 786, 1121, 1612, 2557 };
static const int16_t WL[8]  = { -60, -30, 58, 172, 334, 538, 1198, 3042 };

/* Table 14, higher sub-band: Q2(1), its one decision level; QQ2 and WH by
 * IH2 (1, 2). */
#define Q2_1 564
static const int16_t QQ2[3] = { 0, 202, 926 };
static const int16_t WH[3]  = { 0, -214, 798 };

/* Table 15: ILB, the 32-entry log-to-linear table (SCALEL's method 2). */
static const int16_t ILB[32] = {
    2048, 2093, 2139, 2186, 2233, 2282, 2332, 2383, 2435, 2489, 2543,
    2599, 2656, 2714, 2774, 2834, 2896, 2960, 3025, 3091, 3158, 3228,
    3298, 3371, 3444, 3520, 3597, 3676, 3756, 3838, 3922, 4008,
};

/* Table 16: the 6-bit code IL of interval MIL (1..30), for a negative EL
 * (SIL = -1) and for a positive one (SIL = 0). The four codes 0000xx are
 * never sent. */
static const uint8_t IL_NEG[31] = {
     0, 63, 62, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19,
    18, 17, 16, 15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,
};
static const uint8_t IL_POS[31] = {
     0, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47,
    46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32,
};

/* Table 17: the interval IL4 of a 4-bit code RIL; SIL is -1 for 0001 to
 * 0111, else 0 (1111 and a corrupted 0000 both being the zero level). */
static const uint8_t IL4[16] = { 0, 7, 6, 5, 4, 3, 2, 1, 7, 6, 5, 4, 3, 2, 1, 0 };

/* Table 18: the interval IL6 of a 6-bit code; SIL is -1 for 000000 to
 * 011111, 111110 and 111111 (the corrupted 0000xx read as 111111). */
static const uint8_t IL6[64] = {
     1,  1,  1,  1, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19,
    18, 17, 16, 15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,
    30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15,
    14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  2,  1,
};

/* ------------------------------------------- blocks 3L/3H and 4L/4H, common */

/* FILTEZ, FILTEP and PREDIC: the band's prediction (SL, SH) from its delayed
 * variables, and the zero section's part of it (SZL, SZH) in *sz. */
static int predic(const g722_sb_t *b, int *sz)
{
    int z = 0;                          /* ((((WD6 + WD5) + WD4) + WD3) + WD2) + WD1 */
    for (int i = 5; i >= 0; i--)
        z = sat(z + mul(b->bl[i], sat(b->dlt[i] + b->dlt[i])));
    const int p = sat(mul(b->al[0], sat(b->rlt[0] + b->rlt[0])) +
                      mul(b->al[1], sat(b->rlt[1] + b->rlt[1])));
    *sz = z;
    return sat(p + z);
}

/* LOGSCL/LOGSCH, SCALEL/SCALEH (method 2) and DELAYL/DELAYH: the next scale
 * factor from this one and its multiplier w (WL or WH). The log one is held
 * to 0..nbmax (9 in the lower band, 11 in the higher), the linear one taken
 * from ILB, shifted by `shift` (8, 10) less the log one's integer part. */
static void scale(g722_sb_t *b, int w, int nbmax, int shift)
{
    int nbp = sat(mul(b->nbl, 32512) + w);          /* leakage 127/128 */
    if (nbp < 0) nbp = 0;
    else if (nbp > nbmax) nbp = nbmax;
    const int wd1 = (nbp >> 6) & 31;                /* fractional part */
    const int wd2 = shift - (nbp >> 11);            /* integer part */
    const int wd3 = wd2 >= 0 ? ILB[wd1] >> wd2 : ILB[wd1] << -wd2;
    b->nbl  = (int16_t)nbp;
    b->detl = (int16_t)(wd3 << 2);
}

/* The rest of block 4L/4H once the quantized difference d (DLT, DH) is
 * known: RECONS, PARREC, UPZERO, UPPOL2, UPPOL1 and the DELAYA blocks, with
 * s and sz from predic(). Returns the reconstructed signal (RLT, YH). */
static int adapt(g722_sb_t *b, int d, int s, int sz)
{
    const int r = sat(s + d);                       /* RECONS */
    const int p = sat(d + sz);                      /* PARREC */

    /* UPZERO: gain 1/128 by the signs of d and each DLTi (none while d is
     * 0), leakage 255/256 -- from the oldest, its DELAYA moving each DLTi on
     * once it is used. */
    const int g = d == 0 ? 0 : 128;
    for (int i = 5; i >= 0; i--) {
        b->bl[i]  = (int16_t)sat(((d ^ b->dlt[i]) < 0 ? -g : g) + mul(b->bl[i], 32640));
        b->dlt[i] = i ? b->dlt[i - 1] : (int16_t)d;
    }

    /* UPPOL2: f(AL1) = 4 AL1, saturating; gain 1/128; leakage 127/128;
     * held to +-0.75. */
    const int same1 = (p ^ b->plt[0]) >= 0;         /* SG0 == SG1 */
    const int same2 = (p ^ b->plt[1]) >= 0;         /* SG0 == SG2 */
    int wd = sat(b->al[0] + b->al[0]);
    wd = sat(wd + wd);
    wd = (same1 ? sat(0 - wd) : wd) >> 7;
    int apl2 = sat(sat(wd + (same2 ? 128 : -128)) + mul(b->al[1], 32512));
    if (apl2 > 12288) apl2 = 12288;
    else if (apl2 < -12288) apl2 = -12288;

    /* UPPOL1: gain 3/256, leakage 255/256, held to +-(1 - 2^-4 - APL2). */
    int apl1 = sat((same1 ? 192 : -192) + mul(b->al[0], 32640));
    const int wd3 = sat(15360 - apl2);
    if (apl1 > wd3) apl1 = wd3;
    else if (apl1 < -wd3) apl1 = -wd3;

    /* DELAYA, the rest */
    b->plt[1] = b->plt[0];
    b->plt[0] = (int16_t)p;
    b->rlt[1] = b->rlt[0];
    b->rlt[0] = (int16_t)r;
    b->al[0]  = (int16_t)apl1;
    b->al[1]  = (int16_t)apl2;
    return r;
}

/* ----------------------------------------------------- the lower sub-band */

/* QUANTL: EL's 6-bit code IL under the scale factor DETL. */
static int quantl(int el, int detl)
{
    /* WD: EL's magnitude, less one when EL is negative (SIL = -1). */
    const int wd = el >= 0 ? el : 32767 - (el & 32767);
    /* MIL: the first interval whose higher decision level, (Q6(MIL) << 3) *
     * DETL, lies above WD -- WD on a level takes the larger MIL (note 1), an
     * interval whose two levels coincide is never taken (note 2) -- or 30.
     * The levels never fall as MIL rises: a binary search. */
    int lo = 1, hi = 30;
    while (lo < hi) {
        const int mil = (lo + hi) >> 1;
        if (wd < mul(Q6[mil] << 3, detl)) hi = mil;
        else lo = mil + 1;
    }
    return el >= 0 ? IL_POS[lo] : IL_NEG[lo];
}

/* INVQAL, LOGSCL, SCALEL and block 4L: the lower band's adaptation, which
 * encoder and decoder alike drive from the code's four MSBs only (the two
 * LSBs are the data channel's to take, in modes 2 and 3). */
static void lower(g722_sb_t *b, int il, int s, int sz)
{
    const int ril = il >> 2;                        /* RIL = IL >>> 2 */
    const int il4 = IL4[ril];
    const int wd1 = QQ4[il4] << 3;
    const int dlt = mul(b->detl, ril >= 1 && ril <= 7 ? -wd1 : wd1);
    scale(b, WL[il4], 18432, 8);
    adapt(b, dlt, s, sz);
}

/* The lower sub-band encoder, blocks 1L to 4L: XL's code IL. */
static int encode_lo(g722_sb_t *b, int xl)
{
    int sz;
    const int sl = predic(b, &sz);
    const int il = quantl(sat(xl - sl), b->detl);   /* SUBTRA, QUANTL */
    lower(b, il, sl, sz);
    return il;
}

/* The lower sub-band decoder in mode 1, blocks 2L to 6L: ILR's RL. Its
 * output from all six bits -- INVQBL (table 18), RECONS, LIMIT -- its
 * adaptation from four. */
static int decode_lo(g722_sb_t *b, int ilr)
{
    int sz;
    const int sl = predic(b, &sz);
    const int wd1 = QQ6[IL6[ilr]] << 3;
    const int dl = mul(b->detl, ilr < 32 || ilr >= 62 ? -wd1 : wd1);
    lower(b, ilr, sl, sz);
    return limit(sat(sl + dl));
}

/* ---------------------------------------------------- the higher sub-band */

/* QUANTH: EH's 2-bit code IH under the scale factor DETH. Table 20: 00 and
 * 01 the negative intervals 2 and 1, 11 and 10 the positive 1 and 2. */
static int quanth(int eh, int deth)
{
    const int wd = eh >= 0 ? eh : 32767 - (eh & 32767);
    const int mih = wd >= mul(Q2_1 << 3, deth) ? 2 : 1;
    return eh >= 0 ? (mih == 1 ? 3 : 2) : (mih == 1 ? 1 : 0);
}

/* INVQAH, LOGSCH, SCALEH and block 4H. Table 21: IH 00 and 01 negative
 * (SIH = -1); IH2 1 for 01 and 11, 2 for 00 and 10. Returns YH. */
static int higher(g722_sb_t *b, int ih, int s, int sz)
{
    const int ih2 = ih & 1 ? 1 : 2;
    const int wd1 = QQ2[ih2] << 3;
    const int dh = mul(b->detl, ih < 2 ? -wd1 : wd1);
    scale(b, WH[ih2], 22528, 10);
    return adapt(b, dh, s, sz);
}

/* The higher sub-band encoder, blocks 1H to 4H: XH's code IH. */
static int encode_hi(g722_sb_t *b, int xh)
{
    int sz;
    const int sh = predic(b, &sz);
    const int ih = quanth(sat(xh - sh), b->detl);   /* SUBTRA, QUANTH */
    higher(b, ih, sh, sz);
    return ih;
}

/* The higher sub-band decoder, blocks 2H to 5H: IH's RH. */
static int decode_hi(g722_sb_t *b, int ih)
{
    int sz;
    const int sh = predic(b, &sz);
    return limit(higher(b, ih, sh, sz));
}

/* ------------------------------------------------------------- the coder */

static void band_reset(g722_sb_t *b, int det)
{
    memset(b, 0, sizeof *b);
    b->detl = (int16_t)det;                         /* DELAYL / DELAYH */
}

void g722_enc_init(g722_enc_t *s)
{
    memset(s->xin, 0, sizeof s->xin);
    band_reset(&s->lo, 32);
    band_reset(&s->hi, 8);
}

void g722_dec_init(g722_dec_t *s)
{
    memset(s->xd, 0, sizeof s->xd);
    memset(s->xs, 0, sizeof s->xs);
    band_reset(&s->lo, 32);
    band_reset(&s->hi, 8);
}

int g722_encode(g722_enc_t *s, const int16_t *pcm, int samples, uint8_t *out)
{
    int n = 0;
    for (int j = 0; j + 1 < samples; j += 2) {
        /* ACCUMA and ACCUMB, from the oldest, with DELAYX moving each pair
         * of samples on two places as it is used; then the two new ones,
         * the second XIN(j). LOWT, HIGHT. */
        int32_t xa = 0, xb = 0;
        for (int i = 22; i > 0; i -= 2) {
            const int a = s->xin[i - 2], b = s->xin[i - 1];
            xa += a * H[i];
            xb += b * H[i + 1];
            s->xin[i]     = (int16_t)a;
            s->xin[i + 1] = (int16_t)b;
        }
        s->xin[1] = pcm[j];
        s->xin[0] = pcm[j + 1];
        xa += s->xin[0] * H[0];
        xb += s->xin[1] * H[1];
        const int il = encode_lo(&s->lo, limit((xa + xb) >> 14));
        const int ih = encode_hi(&s->hi, limit((xa - xb) >> 14));
        out[n++] = (uint8_t)(ih << 6 | il);
    }
    return n;
}

int g722_decode(g722_dec_t *s, const uint8_t *in, int bytes, int16_t *pcm)
{
    int k = 0;
    for (; k < bytes; k++) {
        const int rl = decode_lo(&s->lo, in[k] & 63);
        const int rh = decode_hi(&s->hi, in[k] >> 6);

        /* ACCUMC and ACCUMD, from the oldest, with DELAYZ moving each XDi
         * and XSi on as it is used; then RECA's and RECB's new ones. SELECT:
         * XOUT1 first. */
        int32_t wd = 0, ws = 0;
        for (int i = 11; i > 0; i--) {
            const int xd = s->xd[i - 1], xs = s->xs[i - 1];
            wd += xd * H[2 * i];
            ws += xs * H[2 * i + 1];
            s->xd[i] = (int16_t)xd;
            s->xs[i] = (int16_t)xs;
        }
        s->xd[0] = (int16_t)(rl - rh);
        s->xs[0] = (int16_t)(rl + rh);
        wd += s->xd[0] * H[0];
        ws += s->xs[0] * H[1];
        pcm[2 * k]     = (int16_t)sat(wd >> 11);
        pcm[2 * k + 1] = (int16_t)sat(ws >> 11);
    }
    return 2 * k;
}
