/* components/kvstore's pure parts (kv_tlv.c, kv_merge.c): the settings'
 * copies on the SD card and the merge of NVS into them
 * (SETTINGS-ON-CARD-PLAN.md §18.1, H1-H6). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kv_img.h"

static int s_fail;

static uint32_t u32_at(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
            s_fail++;                                                            \
        }                                                                        \
    } while (0)

static uint32_t s_rng = 12345;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/* An image of vals then base, as kvstore writes it; its length. */
static size_t image(uint8_t *out, const kv_buf_t *v, const kv_buf_t *b, const char *ns, uint32_t seq)
{
    memcpy(out + KV_HDR, v->p, v->len);
    if (b && b->len) memcpy(out + KV_HDR + v->len, b->p, b->len);
    const uint32_t len = (uint32_t)(v->len + (b ? b->len : 0));
    kv_hdr_t h = { .seq = seq, .len = len, .crc = kv_crc32(0, out + KV_HDR, len), .format = 1, .slot = 'A',
                   .mac = { 1, 2, 3, 4, 5, 6 } };
    snprintf(h.ns, sizeof h.ns, "%s", ns);
    snprintf(h.version, sizeof h.version, "v1.20.0");
    kv_hdr_put(out, &h);
    return KV_HDR + len;
}

static void set_u32(kv_buf_t *b, const char *k, uint32_t v) { CHECK(kv_buf_set(b, KV_T_U32, k, &v, 4) >= 0); }
static void set_str(kv_buf_t *b, const char *k, const char *s)
{
    CHECK(kv_buf_set(b, KV_T_STR, k, s, strlen(s) + 1) >= 0);
}

static bool has_str(const kv_buf_t *b, const char *k, const char *s)
{
    kv_rec_t x;
    return kv_buf_find(b, k, &x, NULL) && x.type == KV_T_STR && !strcmp((const char *)x.val, s);
}

/* H1: every type round trips, keys of 1 and 15, blobs of 0 B and 15 kB, an
 * unknown type kept, base records. */
static void h1(void)
{
    static uint8_t img[KV_FILE];
    kv_buf_t v = { 0 }, b = { 0 };
    uint8_t u8 = 200; int8_t i8 = -5; uint16_t u16 = 60000; int16_t i16 = -30000;
    uint32_t u32 = 4000000000u; int32_t i32 = -2000000000; uint64_t u64 = 0xFEDCBA9876543210ull;
    int64_t i64 = -1234567890123ll;
    CHECK(kv_buf_set(&v, KV_T_U8, "a", &u8, 1) == 1);
    CHECK(kv_buf_set(&v, KV_T_I8, "i8", &i8, 1) == 1);
    CHECK(kv_buf_set(&v, KV_T_U16, "u16", &u16, 2) == 1);
    CHECK(kv_buf_set(&v, KV_T_I16, "i16", &i16, 2) == 1);
    CHECK(kv_buf_set(&v, KV_T_U32, "u32", &u32, 4) == 1);
    CHECK(kv_buf_set(&v, KV_T_I32, "i32", &i32, 4) == 1);
    CHECK(kv_buf_set(&v, KV_T_U64, "u64", &u64, 8) == 1);
    CHECK(kv_buf_set(&v, KV_T_I64, "abcdefghijklmno", &i64, 8) == 1);
    set_str(&v, "s", "hello");
    CHECK(kv_buf_set(&v, KV_T_BLOB, "empty", "", 0) == 1);
    CHECK(kv_buf_set(&v, 0x33, "future", "xyz", 3) == 1);                /* a code a later firmware adds */
    CHECK(kv_buf_set(&v, KV_T_U8, "a", &u8, 1) == 0);                   /* the same: no change */
    CHECK(kv_buf_set(&v, KV_T_U8, "abcdefghijklmnop", &u8, 1) == -2);   /* 16 */
    CHECK(kv_buf_set(&v, KV_T_U8, "", &u8, 1) == -2);
    static uint8_t big[15000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)rnd();
    CHECK(kv_buf_set(&v, KV_T_BLOB, "big", big, sizeof big) == 1);
    CHECK(kv_base_of(&b, &v) == 0);
    kv_buf_t sm = { 0 };                                                  /* the bases of the small ones */
    CHECK(kv_buf_copy(&sm, b.p, b.len) == 0);
    const size_t n = image(img, &v, &b, "vfo", 7);
    CHECK(n <= KV_FILE);
    kv_hdr_t h;
    bool parsed;
    CHECK(kv_img_check(img, n, KV_FILE, "vfo", &h, &parsed) == KV_VALID && parsed);
    CHECK(h.seq == 7 && h.slot == 'A' && h.mac[5] == 6 && !strcmp(h.version, "v1.20.0"));
    CHECK(kv_img_check(img, n, KV_FILE, "svx", &h, &parsed) == KV_CORRUPT);      /* another namespace's */
    CHECK(kv_img_check(img, n, KV_FILE - 1, "vfo", &h, &parsed) == KV_CORRUPT);  /* not its file */
    CHECK(kv_img_check(img, n - 1, KV_FILE, "vfo", &h, &parsed) == KV_CORRUPT);  /* read short */
    /* Back: the values and the bases apart, each as written. */
    kv_buf_t v2 = { 0 }, b2 = { 0 };
    size_t off = 0;
    kv_rec_t x;
    while (kv_rec_next(img + KV_HDR, h.len, &off, &x)) {
        char key[16];
        memcpy(key, x.key, x.klen);
        key[x.klen] = 0;
        CHECK(kv_buf_set(x.type == KV_T_BASE ? &b2 : &v2, x.type, key, x.val, x.vlen) == 1);
    }
    CHECK(off == h.len);
    CHECK(v2.len == v.len && !memcmp(v2.p, v.p, v.len));
    CHECK(b2.len == b.len && !memcmp(b2.p, b.p, b.len));
    CHECK(kv_buf_find(&v2, "big", &x, NULL) && x.vlen == sizeof big && !memcmp(x.val, big, sizeof big));
    CHECK(kv_buf_find(&v2, "future", &x, NULL) && x.type == 0x33 && x.vlen == 3);
    CHECK(kv_buf_find(&v2, "abcdefghijklmno", &x, NULL) && !memcmp(x.val, &i64, 8));
    CHECK(kv_buf_del(&v2, "s") && !kv_buf_find(&v2, "s", NULL, NULL) && !kv_buf_del(&v2, "s"));
    kv_buf_free(&v); kv_buf_free(&b); kv_buf_free(&v2); kv_buf_free(&b2); kv_buf_free(&sm);
}

/* H2: every single-bit flip of a 4 kB image, rejected. */
static void h2(void)
{
    static uint8_t img[KV_FILE];
    kv_buf_t v = { 0 };
    char k[16], s[64];
    for (int i = 0; v.len < 4096 - KV_HDR - 80; i++) {
        snprintf(k, sizeof k, "k%d", i);
        snprintf(s, sizeof s, "value number %d, %u", i, rnd());
        set_str(&v, k, s);
    }
    const size_t n = image(img, &v, NULL, "phone", 3);
    kv_hdr_t h;
    bool parsed;
    CHECK(kv_img_check(img, n, KV_FILE, "phone", &h, &parsed) == KV_VALID);
    int bad = 0;
    for (size_t byte = 0; byte < n; byte++)
        for (int bit = 0; bit < 8; bit++) {
            img[byte] ^= 1u << bit;
            if (kv_img_check(img, n, KV_FILE, "phone", &h, &parsed) == KV_VALID) bad++;
            img[byte] ^= 1u << bit;
        }
    CHECK(!bad);
    printf("H2: %zu bytes, every flip rejected\n", n);
    kv_buf_free(&v);
}

/* A record list with its CRC made right, so only the records are judged. */
static kv_cstate_t judge(const uint8_t *recs, size_t len)
{
    static uint8_t img[KV_FILE + 64];
    if (len) memcpy(img + KV_HDR, recs, len);
    kv_hdr_t h = { .seq = 1, .len = (uint32_t)len, .crc = kv_crc32(0, img + KV_HDR, len), .format = 1 };
    snprintf(h.ns, sizeof h.ns, "vfo");
    kv_hdr_put(img, &h);
    bool parsed;
    return kv_img_check(img, KV_HDR + len, KV_FILE, "vfo", &h, &parsed);
}

/* H3: random records, and each way records go wrong. */
static void h3(void)
{
    static uint8_t r[2048];
    int valid = 0;
    for (int t = 0; t < 200000; t++) {
        const size_t len = rnd() % 64;
        for (size_t i = 0; i < len; i++) r[i] = (uint8_t)rnd();
        if (t & 1 && len >= 4) {                   /* plausible heads, so the deeper checks are reached */
            r[0] = (uint8_t[]){ KV_T_U8, KV_T_STR, KV_T_BLOB, KV_T_BASE, 0x33 }[rnd() % 5];
            r[1] = 1 + rnd() % 15;
            r[2] = rnd() % 24;
            r[3] = 0;
        }
        if (judge(r, len) == KV_VALID) {
            valid++;
            CHECK(kv_recs_ok(r, len));
        }
    }
    printf("H3: %d of 200000 random record lists valid\n", valid);
    /* each fault on its own */
    const uint8_t ok[] = { KV_T_STR, 2, 3, 0, 'a', 'b', 'x', 'y', 0 };
    CHECK(judge(ok, sizeof ok) == KV_VALID);
    CHECK(judge(ok, sizeof ok - 1) == KV_CORRUPT);                         /* len cut */
    const uint8_t vpast[] = { KV_T_BLOB, 1, 9, 0, 'a', 1, 2 };
    CHECK(judge(vpast, sizeof vpast) == KV_CORRUPT);
    const uint8_t k0[] = { KV_T_U8, 0, 1, 0, 7 };
    CHECK(judge(k0, sizeof k0) == KV_CORRUPT);
    uint8_t k16[4 + 16 + 1] = { KV_T_U8, 16, 1, 0 };
    memset(k16 + 4, 'k', 16);
    CHECK(judge(k16, sizeof k16) == KV_CORRUPT);
    const uint8_t nonul[] = { KV_T_STR, 1, 2, 0, 's', 'h', 'i' };
    CHECK(judge(nonul, sizeof nonul) == KV_CORRUPT);
    const uint8_t empty_str[] = { KV_T_STR, 1, 0, 0, 's' };
    CHECK(judge(empty_str, sizeof empty_str) == KV_CORRUPT);
    const uint8_t dup[] = { KV_T_U8, 1, 1, 0, 'a', 1, KV_T_U32, 1, 4, 0, 'a', 1, 2, 3, 4 };
    CHECK(judge(dup, sizeof dup) == KV_CORRUPT);
    const uint8_t dupbase[] = { KV_T_BASE, 1, 8, 0, 'a', 1, 0, 1, 0, 0, 0, 0, 0,
                                KV_T_BASE, 1, 8, 0, 'a', 1, 0, 1, 0, 0, 0, 0, 0 };
    CHECK(judge(dupbase, sizeof dupbase) == KV_CORRUPT);
    const uint8_t valbase[] = { KV_T_U8, 1, 1, 0, 'a', 1, KV_T_BASE, 1, 8, 0, 'a', 1, 0, 1, 0, 0, 0, 0, 0 };
    CHECK(judge(valbase, sizeof valbase) == KV_VALID);                     /* a value and its base */
    const uint8_t wrongsize[] = { KV_T_U32, 1, 2, 0, 'a', 1, 2 };
    CHECK(judge(wrongsize, sizeof wrongsize) == KV_CORRUPT);
    const uint8_t nul_key[] = { KV_T_U8, 2, 1, 0, 'a', 0, 1 };
    CHECK(judge(nul_key, sizeof nul_key) == KV_CORRUPT);
    const uint8_t high[] = { 0x90, 1, 1, 0, 'a', 1 };
    CHECK(judge(high, sizeof high) == KV_CORRUPT);
    CHECK(judge(NULL, 0) == KV_VALID);                                      /* an empty namespace */
    /* the header: len past the room, a newer format, a wrong magic */
    static uint8_t img[KV_FILE];
    kv_hdr_t h = { .seq = 1, .len = KV_ROOM + 1, .format = 1 };
    snprintf(h.ns, sizeof h.ns, "vfo");
    kv_hdr_put(img, &h);
    bool parsed;
    CHECK(kv_img_check(img, KV_FILE, KV_FILE, "vfo", &h, &parsed) == KV_CORRUPT && parsed);
    h = (kv_hdr_t){ .seq = 1, .len = 0, .crc = kv_crc32(0, "", 0), .format = 2 };
    snprintf(h.ns, sizeof h.ns, "vfo");
    kv_hdr_put(img, &h);
    CHECK(kv_img_check(img, KV_FILE, KV_FILE, "vfo", &h, &parsed) == KV_VALID && h.format == 2);
    img[0] = 'X';
    CHECK(kv_img_check(img, KV_FILE, KV_FILE, "vfo", &h, &parsed) == KV_CORRUPT && !parsed);
}

/* H4: the choice, against a reference written the other way round. */
static void h4(void)
{
    const kv_cstate_t S[] = { KV_VALID, KV_CORRUPT, KV_CORRUPT, KV_UNREADABLE, KV_ABSENT };
    const bool P[] = { true, true, false, false, false };    /* a corrupt copy whose header parsed, or not */
    int cases = 0;
    for (int n = 2; n <= 3; n++)
        for (int a = 0; a < 5; a++) for (int b = 0; b < 5; b++) for (int c = 0; c < (n == 3 ? 5 : 1); c++)
            for (int sa = 1; sa <= 3; sa++) for (int sb = 1; sb <= 3; sb++) for (int sc = 1; sc <= 3; sc++) {
                const int idx[3] = { a, b, c };
                const int sq[3] = { sa, sb, sc };
                kv_copy_t cp[3];
                for (int i = 0; i < 3; i++)
                    cp[i] = (kv_copy_t){ .st = S[idx[i]], .parsed = P[idx[i]], .seq = P[idx[i]] ? sq[i] : 0 };
                const kv_choice_t ch = kv_choose(cp, n);
                /* reference */
                int win = -1, best = 0, hi = 0;
                bool unread = false;
                for (int i = n - 1; i >= 0; i--) {
                    if (cp[i].st == KV_VALID && (int)cp[i].seq >= best) { best = cp[i].seq; win = i; }
                    if (cp[i].parsed && (int)cp[i].seq > hi) hi = cp[i].seq;
                    unread |= cp[i].st == KV_UNREADABLE;
                }
                CHECK(ch.winner == win);
                CHECK(ch.next_seq == (uint32_t)hi + 1);
                CHECK(ch.held == (win < 0 && unread));
                uint8_t rep = 0;
                if (win >= 0)
                    for (int i = 0; i < n; i++)
                        if (i != win && !(cp[i].st == KV_VALID && cp[i].seq == cp[win].seq)) rep |= 1u << i;
                CHECK(ch.repair == rep);
                cases++;
            }
    /* a few by hand */
    kv_copy_t x[3] = { { KV_CORRUPT, true, 9 }, { KV_VALID, true, 8 }, { KV_VALID, true, 5 } };
    kv_choice_t ch = kv_choose(x, 3);
    CHECK(ch.winner == 1 && ch.next_seq == 10 && !ch.held && ch.repair == 5);
    kv_copy_t y[3] = { { KV_UNREADABLE, false, 0 }, { KV_ABSENT, false, 0 }, { KV_CORRUPT, false, 0 } };
    ch = kv_choose(y, 3);
    CHECK(ch.winner < 0 && ch.held && ch.next_seq == 1);
    kv_copy_t z[3] = { { KV_ABSENT, false, 0 }, { KV_ABSENT, false, 0 }, { KV_ABSENT, false, 0 } };
    ch = kv_choose(z, 3);
    CHECK(ch.winner < 0 && !ch.held && ch.repair == 0);
    printf("H4: %d cases\n", cases);
}

/* H5: which of A and B a commit writes first. */
static void h5(void)
{
    const kv_copy_t va = { KV_VALID, true, 4 }, vb = { KV_VALID, true, 5 }, bad = { KV_CORRUPT, true, 9 },
                    gone = { KV_ABSENT, false, 0 }, eq = { KV_VALID, true, 4 }, un = { KV_UNREADABLE, false, 0 };
    CHECK(kv_first(&va, &vb) == 0);      /* A older */
    CHECK(kv_first(&vb, &va) == 1);      /* B older */
    CHECK(kv_first(&va, &eq) == 0);      /* equal: A */
    CHECK(kv_first(&bad, &vb) == 0);     /* A invalid */
    CHECK(kv_first(&va, &bad) == 1);     /* B invalid */
    CHECK(kv_first(&va, &gone) == 1);
    CHECK(kv_first(&un, &va) == 0);
    CHECK(kv_first(&gone, &gone) == 0);
}

static int s_said;
static void say(void *ctx, const char *what, const char *key) { (void)ctx; (void)what; (void)key; s_said++; }

static void pki(kv_buf_t *b, const char *side)
{
    char s[32];
    const char *const m[] = { "key", "csr", "crt" };
    for (int i = 0; i < 3; i++) {
        snprintf(s, sizeof s, "%s-%s", side, m[i]);
        set_str(b, m[i], s);
    }
}

/* The key and the certificate come from the same side, always. */
static bool paired(const kv_buf_t *k)
{
    kv_rec_t a, c;
    const bool ha = kv_buf_find(k, "key", &a, NULL), hc = kv_buf_find(k, "crt", &c, NULL);
    if (!ha || !hc) return true;
    return !strncmp((const char *)a.val, (const char *)c.val, 4);
}

static bool max_merge(const char *key, const void *card, size_t clen, const void *nvs, size_t nlen, void *out,
                      size_t *olen)
{
    (void)key;
    if (clen != 4 || nlen != 4) return false;
    uint32_t a, b;
    memcpy(&a, card, 4);
    memcpy(&b, nvs, 4);
    const uint32_t m = a > b ? a : b;
    memcpy(out, &m, 4);
    *olen = 4;
    return true;
}

/* H6: the merge. */
static void h6(void)
{
    const char *const ident[] = { "key", "uuid", NULL };
    const char *const group[] = { "key", "csr", "crt", NULL };
    kv_rules_t r = { .identity = ident, .say = say };
    kv_merged_t m;
    kv_buf_t e = { 0 }, b = { 0 }, k = { 0 };

    /* A first migration: everything added. */
    set_str(&e, "host", "be.svx.link");
    set_u32(&e, "port", 5300);
    set_str(&e, "uuid", "nvs-uuid");
    CHECK(kv_merge(&k, &b, &e, &r, &m) == 1 && m.added == 3);
    CHECK(has_str(&k, "host", "be.svx.link") && has_str(&k, "uuid", "nvs-uuid"));
    CHECK(kv_base_of(&b, &e) == 0);
    /* Again, nothing changed: nothing to write. The card changes something on its own. */
    set_str(&k, "host", "card.example");
    CHECK(kv_merge(&k, &b, &e, &r, &m) == 0 && has_str(&k, "host", "card.example"));
    /* An older firmware changes port, deletes host, adds call. */
    set_u32(&e, "port", 5301);
    kv_buf_del(&e, "host");
    set_str(&e, "call", "ON6URE");
    CHECK(kv_merge(&k, &b, &e, &r, &m) == 1 && m.changed == 1 && m.deleted == 1 && m.added == 1);
    kv_rec_t x;
    CHECK(kv_buf_find(&k, "port", &x, NULL) && u32_at(x.val) == 5301);
    CHECK(!kv_buf_find(&k, "host", NULL, NULL) && has_str(&k, "call", "ON6URE"));
    CHECK(kv_base_of(&b, &e) == 0);
    /* NVS's own reordering is no change: the same base either way round. */
    {
        kv_buf_t e2 = { 0 }, b2 = { 0 };
        size_t off = 0;
        kv_rec_t r;
        kv_rec_t recs[8];
        int nr = 0;
        while (nr < 8 && kv_rec_next(e.p, e.len, &off, &r)) recs[nr++] = r;
        for (int i = nr - 1; i >= 0; i--) {
            char key[16];
            memcpy(key, recs[i].key, recs[i].klen);
            key[recs[i].klen] = 0;
            kv_buf_set(&e2, recs[i].type, key, recs[i].val, recs[i].vlen);
        }
        CHECK(kv_base_of(&b2, &e2) == 0);
        CHECK(nr > 1 && b2.len == b.len && memcmp(b2.p, b.p, b.len) && kv_buf_same(&b, &b2));
        set_u32(&e2, "port", 1);
        kv_base_of(&b2, &e2);
        CHECK(!kv_buf_same(&b, &b2));
        kv_buf_free(&e2);
        kv_buf_free(&b2);
    }
    /* An identity with no base: the card's own stays. */
    kv_buf_t b0 = { 0 }, k2 = { 0 };
    set_str(&k2, "uuid", "card-uuid");
    s_said = 0;
    CHECK(kv_merge(&k2, &b0, &e, &r, &m) == 1 && has_str(&k2, "uuid", "card-uuid") && m.set_aside == 1 && s_said);
    /* ...but changed in NVS against its base: the NVS one. */
    set_str(&e, "uuid", "nvs-uuid-2");
    CHECK(kv_merge(&k, &b, &e, &r, &m) == 1 && has_str(&k, "uuid", "nvs-uuid-2"));
    CHECK(kv_base_of(&b, &e) == 0);
    /* ...and deleted in NVS (a sign-out by an older firmware): gone from the card. */
    kv_buf_del(&e, "uuid");
    CHECK(kv_merge(&k, &b, &e, &r, &m) == 1 && !kv_buf_find(&k, "uuid", NULL, NULL));
    kv_buf_free(&e); kv_buf_free(&b); kv_buf_free(&k); kv_buf_free(&k2);

    /* svxpki's group. */
    r.group = group;
    kv_buf_t pe = { 0 }, pb = { 0 }, pk = { 0 };
    pki(&pe, "nvs1");
    CHECK(kv_merge(&pk, &pb, &pe, &r, &m) == 1 && m.group_nvs && has_str(&pk, "crt", "nvs1-crt") && paired(&pk));
    CHECK(kv_base_of(&pb, &pe) == 0);
    /* The card renews its certificate: NVS unchanged, the card's kept. */
    set_str(&pk, "crt", "nvs1-crt-renewed");
    CHECK(kv_merge(&pk, &pb, &pe, &r, &m) == 0 && has_str(&pk, "crt", "nvs1-crt-renewed"));
    /* An older firmware makes a new key and certificate: the whole group from NVS. */
    kv_buf_free(&pe);
    pki(&pe, "nvs2");
    CHECK(kv_merge(&pk, &pb, &pe, &r, &m) == 1 && has_str(&pk, "key", "nvs2-key") &&
          has_str(&pk, "crt", "nvs2-crt") && paired(&pk));
    CHECK(kv_base_of(&pb, &pe) == 0);
    /* Only the certificate renewed in NVS: still every member from NVS. */
    set_str(&pe, "crt", "nvs2-crt-b");
    set_str(&pk, "csr", "card-csr");
    CHECK(kv_merge(&pk, &pb, &pe, &r, &m) == 1 && has_str(&pk, "csr", "nvs2-csr") && has_str(&pk, "crt", "nvs2-crt-b"));
    CHECK(kv_base_of(&pb, &pe) == 0);
    /* The certificate erased in NVS: erased on the card, the key stays NVS's. */
    kv_buf_del(&pe, "crt");
    CHECK(kv_merge(&pk, &pb, &pe, &r, &m) == 1 && !kv_buf_find(&pk, "crt", NULL, NULL) && has_str(&pk, "key", "nvs2-key"));
    /* No base, NVS holds a group, the card its own: the card's kept whole. */
    kv_buf_t eb = { 0 }, ck = { 0 }, ce = { 0 };
    pki(&ck, "card");
    pki(&ce, "nvs3");
    s_said = 0;
    CHECK(kv_merge(&ck, &eb, &ce, &r, &m) == 0 && has_str(&ck, "key", "card-key") && has_str(&ck, "crt", "card-crt") &&
          m.set_aside == 1 && s_said);
    /* No base, NVS holds a group, the card none: NVS's. */
    kv_buf_t nk = { 0 };
    set_str(&nk, "other", "x");
    CHECK(kv_merge(&nk, &eb, &ce, &r, &m) == 1 && has_str(&nk, "key", "nvs3-key") && paired(&nk));
    kv_buf_free(&pe); kv_buf_free(&pb); kv_buf_free(&pk); kv_buf_free(&ck); kv_buf_free(&ce); kv_buf_free(&nk);

    /* Random rounds: the key and the certificate never from two sides. */
    for (int t = 0; t < 2000; t++) {
        kv_buf_t E = { 0 }, B = { 0 }, K = { 0 };
        char s[16];
        snprintf(s, sizeof s, "k%03d", t % 1000);
        if (rnd() & 1) pki(&K, "card");
        if (rnd() & 1) { pki(&B, "base"); kv_buf_t t2 = B; B = (kv_buf_t){ 0 }; kv_base_of(&B, &t2); kv_buf_free(&t2); }
        if (rnd() & 1) pki(&E, (rnd() & 1) ? "base" : "nvsX");
        if (rnd() & 1) kv_buf_del(&E, (const char *[]){ "key", "csr", "crt" }[rnd() % 3]);
        if (rnd() & 1) kv_buf_del(&K, "csr");
        CHECK(kv_merge(&K, &B, &E, &r, &m) >= 0);
        CHECK(paired(&K));
        kv_buf_free(&E); kv_buf_free(&B); kv_buf_free(&K);
    }

    /* The order E is read in changes nothing. */
    r.group = NULL;
    for (int t = 0; t < 200; t++) {
        kv_buf_t E1 = { 0 }, E2 = { 0 }, B = { 0 }, K1 = { 0 }, K2 = { 0 };
        int keys[20];
        for (int i = 0; i < 20; i++) keys[i] = i;
        for (int i = 19; i > 0; i--) { const int j = rnd() % (i + 1), tt = keys[i]; keys[i] = keys[j]; keys[j] = tt; }
        for (int i = 0; i < 20; i++) {
            char kk[8];
            snprintf(kk, sizeof kk, "n%d", i);
            set_u32(&E1, kk, i * 7);
            snprintf(kk, sizeof kk, "n%d", keys[i]);
            set_u32(&E2, kk, keys[i] * 7);
            snprintf(kk, sizeof kk, "n%d", i);
            if (i % 3 == 0) set_u32(&B, kk, i);
            if (i % 2 == 0) { set_u32(&K1, kk, 1000 + i); set_u32(&K2, kk, 1000 + i); }
        }
        kv_buf_t BB = { 0 };
        kv_base_of(&BB, &B);
        CHECK(kv_merge(&K1, &BB, &E1, &r, &m) >= 0);
        CHECK(kv_merge(&K2, &BB, &E2, &r, &m) >= 0);
        for (int i = 0; i < 20; i++) {
            char kk[8];
            kv_rec_t a, c;
            snprintf(kk, sizeof kk, "n%d", i);
            const bool fa = kv_buf_find(&K1, kk, &a, NULL), fc = kv_buf_find(&K2, kk, &c, NULL);
            CHECK(fa == fc && (!fa || (a.vlen == c.vlen && !memcmp(a.val, c.val, a.vlen))));
        }
        kv_buf_free(&E1); kv_buf_free(&E2); kv_buf_free(&B); kv_buf_free(&BB); kv_buf_free(&K1); kv_buf_free(&K2);
    }

    /* A namespace's own rule (kiwi's strikes: the higher). */
    kv_rules_t rk = { .fn = max_merge };
    kv_buf_t E = { 0 }, B = { 0 }, K = { 0 };
    set_u32(&E, "m1234abcd", 2);
    set_u32(&K, "m1234abcd", 5);
    set_u32(&E, "m0000ffff", 9);
    set_u32(&K, "m0000ffff", 1);
    CHECK(kv_merge(&K, &B, &E, &rk, &m) == 1);
    CHECK(kv_buf_find(&K, "m1234abcd", &x, NULL) && u32_at(x.val) == 5);
    CHECK(kv_buf_find(&K, "m0000ffff", &x, NULL) && u32_at(x.val) == 9);
    kv_buf_free(&E); kv_buf_free(&B); kv_buf_free(&K);
}

int main(void)
{
    h1();
    h2();
    h3();
    h4();
    h5();
    h6();
    if (s_fail) {
        fprintf(stderr, "test_kv: %d checks failed\n", s_fail);
        return 1;
    }
    printf("test_kv: all passed\n");
    return 0;
}
