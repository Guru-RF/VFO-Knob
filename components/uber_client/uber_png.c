/* See uber_png.h. */
#include "uber_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "miniz.h"

typedef struct {
    int      w, h, bpp, stride, ct;
    uint8_t *cur, *prev;          /* a row and the one before, each with its filter byte */
    size_t   pos;                 /* bytes of the current row so far */
    int      y;                   /* the source row being filled */
    uint16_t *out;
    int      W, H, oy;            /* the fitted picture, and its next row */
    const uint8_t *pal;           /* PLTE, 3 bytes an entry */
    int      npal;
} rows_t;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static uint8_t paeth(int a, int b, int c)
{
    const int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

static void unfilter(rows_t *r)
{
    uint8_t *l = r->cur + 1;
    const uint8_t *u = r->prev + 1;
    const int bpp = r->bpp, n = r->stride;
    switch (r->cur[0]) {
    case 1: for (int i = bpp; i < n; i++) l[i] += l[i - bpp]; break;
    case 2: for (int i = 0; i < n; i++) l[i] += u[i]; break;
    case 3:
        for (int i = 0; i < n; i++) l[i] += (uint8_t)(((i >= bpp ? l[i - bpp] : 0) + u[i]) >> 1);
        break;
    case 4:
        for (int i = 0; i < n; i++)
            l[i] += paeth(i >= bpp ? l[i - bpp] : 0, u[i], i >= bpp ? u[i - bpp] : 0);
        break;
    default: break;
    }
}

static uint16_t px565(const rows_t *r, const uint8_t *p)
{
    uint8_t R, G, B;
    switch (r->ct) {
    case 0: case 4: R = G = B = p[0]; break;
    case 3: {
        const int i = p[0] < r->npal ? p[0] : 0;
        R = r->pal ? r->pal[i * 3] : 0;
        G = r->pal ? r->pal[i * 3 + 1] : 0;
        B = r->pal ? r->pal[i * 3 + 2] : 0;
        break;
    }
    default: R = p[0]; G = p[1]; B = p[2]; break;
    }
    return (uint16_t)((R & 0xF8) << 8 | (G & 0xFC) << 3 | B >> 3);
}

/* The fitted rows that come from source row y. */
static void emit(rows_t *r)
{
    const uint8_t *l = r->cur + 1;
    while (r->oy < r->H && (int)((int64_t)r->oy * r->h / r->H) == r->y) {
        uint16_t *o = r->out + (size_t)r->oy * r->W;
        for (int x = 0; x < r->W; x++) o[x] = px565(r, l + (size_t)((int64_t)x * r->w / r->W) * r->bpp);
        r->oy++;
    }
}

static void feed(rows_t *r, const uint8_t *d, size_t n)
{
    while (n && r->y < r->h) {
        size_t k = (size_t)r->stride + 1 - r->pos;
        if (k > n) k = n;
        memcpy(r->cur + r->pos, d, k);
        r->pos += k;
        d += k;
        n -= k;
        if (r->pos == (size_t)r->stride + 1) {
            unfilter(r);
            emit(r);
            uint8_t *t = r->prev;
            r->prev = r->cur;
            r->cur = t;
            r->pos = 0;
            r->y++;
        }
    }
}

esp_err_t upng_decode(uint8_t *png, size_t n, uint16_t *out, int box_w, int box_h,
                      int *ow, int *oh, char *why, size_t wn)
{
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (n < 33 || memcmp(png, SIG, 8) || memcmp(png + 12, "IHDR", 4)) {
        snprintf(why, wn, "not a PNG");
        return ESP_FAIL;
    }
    rows_t r = { 0 };
    r.w = (int)be32(png + 16);
    r.h = (int)be32(png + 20);
    const int depth = png[24], il = png[28];
    r.ct = png[25];
    static const int CH[7] = { 1, 0, 3, 1, 2, 0, 4 };
    if (depth != 8 || il || r.ct > 6 || !CH[r.ct] || r.w <= 0 || r.h <= 0 || r.w > 2048 || r.h > 2048) {
        snprintf(why, wn, "a PNG this cannot read");
        return ESP_FAIL;
    }
    r.bpp = CH[r.ct];
    r.stride = r.w * r.bpp;
    /* Fitted, its shape kept. */
    if ((int64_t)r.w * box_h > (int64_t)r.h * box_w) {
        r.W = box_w;
        r.H = (int)((int64_t)r.h * box_w / r.w);
    } else {
        r.H = box_h;
        r.W = (int)((int64_t)r.w * box_h / r.h);
    }
    if (r.W < 1) r.W = 1;
    if (r.H < 1) r.H = 1;
    r.out = out;

    /* The image data, gathered to the front of the buffer: it only moves
     * down, so in place. The palette is copied first -- its bytes may be
     * written over. */
    uint8_t pal[256 * 3];
    size_t z = 0;
    for (size_t o = 8; o + 12 <= n;) {
        const uint32_t len = be32(png + o);
        const uint8_t *type = png + o + 4;
        if (o + 12 + len > n) break;
        if (!memcmp(type, "PLTE", 4) && len <= sizeof pal) {
            memcpy(pal, png + o + 8, len);
            r.npal = (int)(len / 3);
            r.pal = pal;
        } else if (!memcmp(type, "IDAT", 4)) {
            memmove(png + z, png + o + 8, len);
            z += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        o += 12 + len;
    }
    if (!z) {
        snprintf(why, wn, "no image data");
        return ESP_FAIL;
    }

    tinfl_decompressor *dc = heap_caps_malloc(sizeof *dc, MALLOC_CAP_SPIRAM);
    uint8_t *dict = heap_caps_malloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM);
    r.cur  = heap_caps_calloc(1, (size_t)r.stride + 1, MALLOC_CAP_SPIRAM);
    r.prev = heap_caps_calloc(1, (size_t)r.stride + 1, MALLOC_CAP_SPIRAM);
    esp_err_t err = ESP_FAIL;
    if (!dc || !dict || !r.cur || !r.prev) {
        snprintf(why, wn, "no memory");
        goto done;
    }
    memset(out, 0, (size_t)box_w * box_h * sizeof *out);
    tinfl_init(dc);
    size_t in = 0, dofs = 0;
    tinfl_status st;
    for (;;) {
        size_t isz = z - in, osz = TINFL_LZ_DICT_SIZE - dofs;
        st = tinfl_decompress(dc, png + in, &isz, dict, dict + dofs, &osz, TINFL_FLAG_PARSE_ZLIB_HEADER);
        in += isz;
        if (osz) feed(&r, dict + dofs, osz);
        dofs = (dofs + osz) & (TINFL_LZ_DICT_SIZE - 1);
        if (st != TINFL_STATUS_HAS_MORE_OUTPUT) break;
    }
    /* A picture cut short is still worth showing: SSTV's often are. */
    if (st == TINFL_STATUS_DONE || r.y > 0) {
        err = ESP_OK;
        *ow = r.W;
        *oh = r.H;
    } else {
        snprintf(why, wn, "a damaged PNG");
    }
done:
    free(dc);
    free(dict);
    free(r.cur);
    free(r.prev);
    return err;
}
