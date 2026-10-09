/* The settings' copies on the SD card, and the merge of NVS into them: pure
 * C, with no ESP-IDF in it, so test/host tries it on the PC (test_kv).
 *
 * A copy is one 16 kB file: a 64-byte header, then records, then whatever the
 * file held before (never read). A record is
 *     u8 type | u8 klen 1..15 | u16 vlen LE | key | value
 * with NVS's type codes; 0xB0 is a base record -- an NVS key as it was at the
 * last merge, {u8 type, u8 0, u16 length, u32 crc32 of the value}. See
 * SETTINGS-ON-CARD-PLAN.md §3.2 and §8. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KV_FILE     16384u                 /* every copy's file, one cluster on a card the knob formats */
#define KV_HDR      64u
#define KV_ROOM     (KV_FILE - KV_HDR)     /* records, values and base together */
#define KV_KEY_MAX  15
#define KV_NS_MAX   8                      /* [a-z0-9]{1,8} */
#define KV_STR_MAX  4000                   /* a str with its NUL: NVS can always take it back */

enum {
    KV_T_U8 = 0x01, KV_T_I8 = 0x11, KV_T_U16 = 0x02, KV_T_I16 = 0x12, KV_T_U32 = 0x04, KV_T_I32 = 0x14,
    KV_T_U64 = 0x08, KV_T_I64 = 0x18, KV_T_STR = 0x21, KV_T_BLOB = 0x42, KV_T_BASE = 0xB0,
};

/* Header flags. */
#define KV_HF_MERGED  0x01                 /* written by a merge */
#define KV_HF_EMPTIED 0x02                 /* the NVS copy has been emptied */

typedef struct {
    uint32_t seq, len, crc;
    uint16_t format;
    uint8_t  slot, gen, flags;
    uint8_t  mac[6];
    char     ns[KV_NS_MAX + 2];
    char     version[12];
} kv_hdr_t;

uint32_t kv_crc32(uint32_t crc, const void *p, size_t n);      /* as esp_rom_crc32_le */
/* The header's 64 bytes, its own CRC last. */
void     kv_hdr_put(uint8_t out[KV_HDR], const kv_hdr_t *h);
/* The magic, its length and its CRC: a header that parsed (its seq counts). */
bool     kv_hdr_get(const uint8_t in[KV_HDR], kv_hdr_t *h);
/* Every record within len, keys 1..15 with no NUL, fixed sizes right, strs
 * ending in NUL, base records of 8 bytes, no key twice among the values nor
 * twice among the bases. */
bool     kv_recs_ok(const uint8_t *r, size_t len);

typedef enum { KV_VALID, KV_CORRUPT, KV_UNREADABLE, KV_ABSENT } kv_cstate_t;
/* A copy read whole (n bytes from its start; the file is file_size): VALID or
 * CORRUPT, *parsed when its header did (its seq then counts for the next). A
 * newer format is VALID with h->format > 1: the caller leaves it alone. */
kv_cstate_t kv_img_check(const uint8_t *img, size_t n, uint32_t file_size, const char *ns, kv_hdr_t *h,
                         bool *parsed);

/* One copy as read at boot, and which of them wins. */
typedef struct {
    kv_cstate_t st;
    bool        parsed;                    /* its header parsed: seq is the header's */
    uint32_t    seq;
} kv_copy_t;
typedef struct {
    int      winner;                       /* -1: none valid */
    uint32_t next_seq;                     /* 1 + the highest seq of any header that parsed */
    bool     held;                         /* none valid and one unreadable: not written this boot */
    uint8_t  repair;                       /* bit i: copy i is behind the winner, bad or absent */
} kv_choice_t;
kv_choice_t kv_choose(const kv_copy_t *c, int n);
/* Of A and B, the one a commit writes first: the invalid one, else the older,
 * else A. */
int kv_first(const kv_copy_t *a, const kv_copy_t *b);

/* Records in a buffer that grows (PSRAM on the knob). */
typedef struct { uint8_t *p; size_t len, cap; } kv_buf_t;
typedef struct { uint8_t type, klen; uint16_t vlen; const char *key; const uint8_t *val; } kv_rec_t;

/* The record at *off, *off moved past it; false at the end. Only for records
 * kv_recs_ok() passed, or a kv_buf_t's own. */
bool kv_rec_next(const uint8_t *r, size_t len, size_t *off, kv_rec_t *out);
bool kv_buf_find(const kv_buf_t *b, const char *key, kv_rec_t *out, size_t *at);
/* 1 changed, 0 the same value already there, -1 out of memory, -2 not a key. */
int  kv_buf_set(kv_buf_t *b, uint8_t type, const char *key, const void *v, size_t vlen);
bool kv_buf_del(kv_buf_t *b, const char *key);
void kv_buf_free(kv_buf_t *b);
int  kv_buf_copy(kv_buf_t *dst, const uint8_t *src, size_t len);     /* -1 out of memory */
/* The same records, in whatever order: NVS reorders its entries itself (a
 * page's clean-up moves them), and that is no change. */
bool kv_buf_same(const kv_buf_t *a, const kv_buf_t *b);
bool kv_key_ok(const char *key);

/* The base records of E: one per key, {type, 0, length, crc}. -1 out of memory. */
int kv_base_of(kv_buf_t *out, const kv_buf_t *e);

/* The three-way merge (§8.3): E the NVS copy, B the base, K the card's, K
 * changed in place. A key in `identity` is the card's unless the NVS copy
 * changed it against B; the keys in `group` are taken all from one side. */
typedef bool (*kv_merge_fn)(const char *key, const void *card, size_t clen, const void *nvs, size_t nlen,
                            void *out, size_t *olen);
typedef struct {
    const char *const *identity;
    const char *const *group;
    kv_merge_fn        fn;
    void             (*say)(void *ctx, const char *what, const char *key);   /* never a value */
    void              *ctx;
} kv_rules_t;
typedef struct { uint16_t added, changed, deleted, set_aside; bool group_nvs; } kv_merged_t;
/* 1 K changed, 0 not, -1 out of memory (K then partly merged: not written). */
int kv_merge(kv_buf_t *k, const kv_buf_t *b, const kv_buf_t *e, const kv_rules_t *r, kv_merged_t *out);

bool kv_listed(const char *const *list, const char *key);
