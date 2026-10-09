/* The three-way merge of a namespace's NVS copy into its card copy, against
 * the base the last merge left (SETTINGS-ON-CARD-PLAN.md §8.3). Pure C. */
#include "kv_img.h"

#include <string.h>

enum { S_NONE, S_SAME, S_ADDED, S_CHANGED, S_DELETED };

static void key_of(const kv_rec_t *x, char out[KV_KEY_MAX + 1])
{
    memcpy(out, x->key, x->klen);
    out[x->klen] = 0;
}

static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* How the NVS copy holds `key` against the base; *ex its record when it has one. */
static int status(const kv_buf_t *b, const kv_buf_t *e, const char *key, kv_rec_t *ex)
{
    kv_rec_t br;
    const bool in_e = kv_buf_find(e, key, ex, NULL);
    const bool in_b = kv_buf_find(b, key, &br, NULL) && br.vlen == 8;
    if (!in_e) return in_b ? S_DELETED : S_NONE;
    if (!in_b) return S_ADDED;
    const bool same = br.val[0] == ex->type && (br.val[2] | br.val[3] << 8) == ex->vlen &&
                      get32(br.val + 4) == kv_crc32(0, ex->val, ex->vlen);
    return same ? S_SAME : S_CHANGED;
}

static void say(const kv_rules_t *r, const char *what, const char *key)
{
    if (r->say) r->say(r->ctx, what, key);
}

/* E's value, or its absence, onto K: 1 changed, 0 not, -1 out of memory. */
static int take(kv_buf_t *k, const char *key, int st, const kv_rec_t *ex)
{
    if (st == S_DELETED || st == S_NONE) return kv_buf_del(k, key) ? 1 : 0;
    const int c = kv_buf_set(k, ex->type, key, ex->val, ex->vlen);
    return c < 0 ? -1 : c;
}

static bool differs(const kv_rec_t *a, const kv_rec_t *b)
{
    return a->type != b->type || a->vlen != b->vlen || memcmp(a->val, b->val, a->vlen);
}

int kv_merge(kv_buf_t *k, const kv_buf_t *b, const kv_buf_t *e, const kv_rules_t *r, kv_merged_t *out)
{
    memset(out, 0, sizeof *out);
    int changed = 0;
    kv_rec_t ex, kx;

    /* The group, all from one side. */
    if (r->group && *r->group) {
        bool in_b = false, in_e = false, moved = false, card_holds = false;
        bool has_ident = false;
        for (const char *const *g = r->group; *g; g++) {
            const int st = status(b, e, *g, &ex);
            in_b |= kv_buf_find(b, *g, NULL, NULL);
            in_e |= st != S_NONE && st != S_DELETED;
            moved |= st == S_ADDED || st == S_CHANGED || st == S_DELETED;
            has_ident |= kv_listed(r->identity, *g);
        }
        for (const char *const *g = r->group; *g; g++)
            if ((!has_ident || kv_listed(r->identity, *g)) && kv_buf_find(k, *g, NULL, NULL)) card_holds = true;
        bool from_e = false;
        if (in_b) from_e = moved;
        else if (in_e) {
            from_e = !card_holds;
            if (card_holds) {
                say(r, "set aside: the NVS copy's group, the card holding its own", r->group[0]);
                out->set_aside++;
            }
        }
        if (from_e) {
            out->group_nvs = true;
            say(r, "the group taken from NVS", r->group[0]);
            for (const char *const *g = r->group; *g; g++) {
                const int st = status(b, e, *g, &ex);
                const bool had = kv_buf_find(k, *g, NULL, NULL);
                const int c = take(k, *g, st, &ex);
                if (c < 0) return -1;
                if (c) {
                    changed = 1;
                    if (st == S_DELETED || st == S_NONE) out->deleted++;
                    else if (had) out->changed++;
                    else out->added++;
                }
            }
        }
    }

    /* Every other key of E, then of B that E no longer has. */
    for (int pass = 0; pass < 2; pass++) {
        const kv_buf_t *src = pass ? b : e;
        size_t off = 0;
        kv_rec_t x;
        while (kv_rec_next(src->p, src->len, &off, &x)) {
            char key[KV_KEY_MAX + 1];
            key_of(&x, key);
            if (kv_listed(r->group, key)) continue;
            if (pass && kv_buf_find(e, key, NULL, NULL)) continue;
            const int st = status(b, e, key, &ex);
            if (st == S_SAME || st == S_NONE) continue;
            const bool in_k = kv_buf_find(k, key, &kx, NULL);
            int c = 0;
            if (kv_listed(r->identity, key) && st == S_ADDED && in_k) {
                /* Generated or learned with no base: the card's own stays. */
                if (differs(&kx, &ex)) {
                    say(r, "set aside: the NVS copy's, the card's kept", key);
                    out->set_aside++;
                }
                continue;
            }
            if (st != S_DELETED && in_k && r->fn && differs(&kx, &ex)) {
                uint8_t m[512];
                size_t ml = sizeof m;
                if (r->fn(key, kx.val, kx.vlen, ex.val, ex.vlen, m, &ml) && ml <= sizeof m) {
                    c = kv_buf_set(k, ex.type, key, m, ml);
                    if (c < 0) return -1;
                    if (c) {
                        changed = 1;
                        out->changed++;
                    }
                    continue;
                }
            }
            c = take(k, key, st, &ex);
            if (c < 0) return -1;
            if (!c) continue;
            changed = 1;
            if (st == S_DELETED) out->deleted++;
            else if (st == S_ADDED) out->added++;
            else out->changed++;
        }
    }
    return changed;
}
