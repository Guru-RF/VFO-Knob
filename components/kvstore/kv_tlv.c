/* The settings' copies: records, the header, the checks and the choice. Pure
 * C. See kv_img.h. */
#include "kv_img.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static void *grow(void *p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
#else
static void *grow(void *p, size_t n) { return realloc(p, n); }
#endif

static const uint8_t MAGIC[4] = { 'V', 'K', 'S', '1' };

uint32_t kv_crc32(uint32_t crc, const void *p, size_t n)
{
    static const uint32_t T[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    const uint8_t *b = p;
    crc = ~crc;
    while (n--) {
        crc ^= *b++;
        crc = (crc >> 4) ^ T[crc & 15];
        crc = (crc >> 4) ^ T[crc & 15];
    }
    return ~crc;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static uint16_t get16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void kv_hdr_put(uint8_t o[KV_HDR], const kv_hdr_t *h)
{
    memset(o, 0, KV_HDR);
    memcpy(o, MAGIC, 4);
    put16(o + 4, KV_HDR);
    put16(o + 6, h->format ? h->format : 1);
    put32(o + 8, h->seq);
    put32(o + 12, KV_FILE);
    put32(o + 16, h->len);
    put32(o + 20, h->crc);
    o[24] = h->slot;
    o[25] = h->gen;
    o[26] = h->flags;
    memcpy(o + 28, h->mac, 6);
    memcpy(o + 34, h->ns, strnlen(h->ns, 9));             /* a NUL after each, from the memset */
    memcpy(o + 44, h->version, strnlen(h->version, 11));
    put32(o + 60, kv_crc32(0, o, 60));
}

bool kv_hdr_get(const uint8_t in[KV_HDR], kv_hdr_t *h)
{
    memset(h, 0, sizeof *h);
    if (memcmp(in, MAGIC, 4) || get16(in + 4) != KV_HDR || get32(in + 60) != kv_crc32(0, in, 60)) return false;
    h->format = get16(in + 6);
    h->seq = get32(in + 8);
    h->len = get32(in + 16);
    h->crc = get32(in + 20);
    h->slot = in[24];
    h->gen = in[25];
    h->flags = in[26];
    memcpy(h->mac, in + 28, 6);
    memcpy(h->ns, in + 34, 10);
    h->ns[sizeof h->ns - 1] = 0;
    memcpy(h->version, in + 44, 12);
    h->version[sizeof h->version - 1] = 0;
    return true;
}

static int fixed_len(uint8_t t)
{
    switch (t) {
    case KV_T_U8: case KV_T_I8: return 1;
    case KV_T_U16: case KV_T_I16: return 2;
    case KV_T_U32: case KV_T_I32: return 4;
    case KV_T_U64: case KV_T_I64: return 8;
    case KV_T_BASE: return 8;
    default: return -1;
    }
}

bool kv_key_ok(const char *key)
{
    if (!key) return false;
    const size_t n = strlen(key);
    return n >= 1 && n <= KV_KEY_MAX;
}

/* The record at off, checked against len; its size, or 0 when it does not fit. */
static size_t rec_at(const uint8_t *r, size_t len, size_t off, kv_rec_t *o)
{
    if (len - off < 4) return 0;
    o->type = r[off];
    o->klen = r[off + 1];
    o->vlen = get16(r + off + 2);
    if (o->klen < 1 || o->klen > KV_KEY_MAX) return 0;
    const size_t sz = 4u + o->klen + o->vlen;
    if (sz > len - off) return 0;
    o->key = (const char *)r + off + 4;
    o->val = r + off + 4 + o->klen;
    return sz;
}

static bool same_key(const kv_rec_t *a, const char *key, size_t klen)
{
    return a->klen == klen && !memcmp(a->key, key, klen);
}

bool kv_recs_ok(const uint8_t *r, size_t len)
{
    size_t off = 0;
    while (off < len) {
        kv_rec_t x;
        const size_t sz = rec_at(r, len, off, &x);
        if (!sz) return false;
        if (memchr(x.key, 0, x.klen)) return false;
        const int fl = fixed_len(x.type);
        if (fl > 0 && x.vlen != fl) return false;
        if (x.type == KV_T_STR && (!x.vlen || x.val[x.vlen - 1])) return false;
        if (x.type >= 0x80 && x.type != KV_T_BASE) return false;      /* a code no format 1 has */
        /* The same key twice among the values, or twice among the bases. */
        const bool base = x.type == KV_T_BASE;
        size_t o2 = 0;
        while (o2 < off) {
            kv_rec_t y;
            o2 += rec_at(r, len, o2, &y);
            if ((y.type == KV_T_BASE) == base && same_key(&y, x.key, x.klen)) return false;
        }
        off += sz;
    }
    return true;
}

kv_cstate_t kv_img_check(const uint8_t *img, size_t n, uint32_t file_size, const char *ns, kv_hdr_t *h,
                         bool *parsed)
{
    *parsed = false;
    if (n < KV_HDR || !kv_hdr_get(img, h)) return KV_CORRUPT;
    *parsed = true;
    if (get32(img + 12) != KV_FILE || file_size != KV_FILE) return KV_CORRUPT;
    if (strcmp(h->ns, ns)) return KV_CORRUPT;
    if (h->len > KV_ROOM || h->len > n - KV_HDR) return KV_CORRUPT;
    if (kv_crc32(0, img + KV_HDR, h->len) != h->crc) return KV_CORRUPT;
    if (h->format < 1) return KV_CORRUPT;
    if (h->format > 1) return KV_VALID;                /* a newer firmware's: not read further */
    return kv_recs_ok(img + KV_HDR, h->len) ? KV_VALID : KV_CORRUPT;
}

kv_choice_t kv_choose(const kv_copy_t *c, int n)
{
    kv_choice_t o = { .winner = -1, .next_seq = 1 };
    uint32_t hi = 0;
    bool any = false, unreadable = false;
    for (int i = 0; i < n; i++) {
        if (c[i].parsed) {
            if (!any || c[i].seq > hi) hi = c[i].seq;
            any = true;
        }
        if (c[i].st == KV_UNREADABLE) unreadable = true;
        if (c[i].st == KV_VALID && (o.winner < 0 || c[i].seq > c[o.winner].seq)) o.winner = i;
    }
    if (any) o.next_seq = hi + 1;
    if (o.winner < 0) {
        o.held = unreadable;
        return o;
    }
    for (int i = 0; i < n; i++)
        if (i != o.winner && (c[i].st != KV_VALID || c[i].seq != c[o.winner].seq)) o.repair |= 1u << i;
    return o;
}

int kv_first(const kv_copy_t *a, const kv_copy_t *b)
{
    const bool va = a->st == KV_VALID, vb = b->st == KV_VALID;
    if (!va) return 0;
    if (!vb) return 1;
    return b->seq < a->seq ? 1 : 0;
}

bool kv_rec_next(const uint8_t *r, size_t len, size_t *off, kv_rec_t *out)
{
    if (*off >= len) return false;
    const size_t sz = rec_at(r, len, *off, out);
    if (!sz) return false;
    *off += sz;
    return true;
}

bool kv_buf_find(const kv_buf_t *b, const char *key, kv_rec_t *out, size_t *at)
{
    const size_t kl = strlen(key);
    size_t off = 0;
    kv_rec_t x;
    while (off < b->len) {
        const size_t here = off;
        if (!kv_rec_next(b->p, b->len, &off, &x)) break;
        if (same_key(&x, key, kl)) {
            if (out) *out = x;
            if (at) *at = here;
            return true;
        }
    }
    return false;
}

static int reserve(kv_buf_t *b, size_t need)
{
    if (b->len + need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + need) cap *= 2;
    uint8_t *p = grow(b->p, cap);
    if (!p) return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}

bool kv_buf_del(kv_buf_t *b, const char *key)
{
    kv_rec_t x;
    size_t at;
    if (!kv_buf_find(b, key, &x, &at)) return false;
    const size_t sz = 4u + x.klen + x.vlen;
    memmove(b->p + at, b->p + at + sz, b->len - at - sz);
    b->len -= sz;
    return true;
}

int kv_buf_set(kv_buf_t *b, uint8_t type, const char *key, const void *v, size_t vlen)
{
    if (!kv_key_ok(key) || vlen > 0xFFFF) return -2;
    kv_rec_t x;
    if (kv_buf_find(b, key, &x, NULL) && x.type == type && x.vlen == vlen && (!vlen || !memcmp(x.val, v, vlen)))
        return 0;
    const size_t kl = strlen(key);
    /* Room first, so a failure leaves the old value. */
    if (reserve(b, 4 + kl + vlen) < 0) return -1;
    kv_buf_del(b, key);
    uint8_t *o = b->p + b->len;
    o[0] = type;
    o[1] = (uint8_t)kl;
    put16(o + 2, (uint16_t)vlen);
    memcpy(o + 4, key, kl);
    if (vlen) memcpy(o + 4 + kl, v, vlen);
    b->len += 4 + kl + vlen;
    return 1;
}

void kv_buf_free(kv_buf_t *b)
{
    free(b->p);
    *b = (kv_buf_t){ 0 };
}

int kv_buf_copy(kv_buf_t *dst, const uint8_t *src, size_t len)
{
    dst->len = 0;
    if (reserve(dst, len) < 0) return -1;
    if (len) memcpy(dst->p, src, len);
    dst->len = len;
    return 0;
}

bool kv_buf_same(const kv_buf_t *a, const kv_buf_t *b)
{
    if (a->len != b->len) return false;
    size_t off = 0;
    kv_rec_t x, y;
    while (kv_rec_next(a->p, a->len, &off, &x)) {
        char key[KV_KEY_MAX + 1];
        memcpy(key, x.key, x.klen);
        key[x.klen] = 0;
        if (!kv_buf_find(b, key, &y, NULL) || y.type != x.type || y.vlen != x.vlen ||
            (x.vlen && memcmp(x.val, y.val, x.vlen)))
            return false;
    }
    return true;
}

int kv_base_of(kv_buf_t *out, const kv_buf_t *e)
{
    out->len = 0;
    size_t off = 0;
    kv_rec_t x;
    while (kv_rec_next(e->p, e->len, &off, &x)) {
        char key[KV_KEY_MAX + 1];
        memcpy(key, x.key, x.klen);
        key[x.klen] = 0;
        uint8_t v[8] = { x.type, 0 };
        put16(v + 2, x.vlen);
        put32(v + 4, kv_crc32(0, x.val, x.vlen));
        if (kv_buf_set(out, KV_T_BASE, key, v, sizeof v) < 0) return -1;
    }
    return 0;
}

bool kv_listed(const char *const *list, const char *key)
{
    for (; list && *list; list++)
        if (!strcmp(*list, key)) return true;
    return false;
}
