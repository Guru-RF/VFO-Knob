/* The knob's settings in RAM, read at boot from the SD card and NVS. See
 * kvstore.h and SETTINGS-ON-CARD-PLAN.md. */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "kv_priv.h"
#include "sd_cache.h"

static const char *TAG = "kv";

kv_g_t   kvg;
kv_ns_t *kv_all;
int      kv_n;
uint8_t *kv_img;
uint8_t *kv_chk;

static EXT_RAM_BSS_ATTR kv_ns_t s_ns[16];
static kv_merge_cb s_merge[16];
static const char *s_merge_ns[16];
static bool s_up;

static int64_t now_us(void) { return esp_timer_get_time(); }

/* ---- the namespaces in RAM ---- */

size_t kv_snapshot(kv_ns_t *n, uint8_t *out, bool with_base, uint32_t *gen)
{
    kv_lock(n);
    size_t len = n->val.len;
    if (len) memcpy(out, n->val.p, len);
    if (with_base && n->base.len) {
        memcpy(out + len, n->base.p, n->base.len);
        len += n->base.len;
    }
    *gen = n->gen;
    kv_unlock(n);
    return len;
}

/* A valid copy's records into val and base. */
void kv_load_recs(kv_ns_t *n, const uint8_t *r, size_t len)
{
    kv_lock(n);
    n->val.len = n->base.len = 0;
    size_t off = 0;
    kv_rec_t x;
    while (kv_rec_next(r, len, &off, &x)) {
        char key[KV_KEY_MAX + 1];
        memcpy(key, x.key, x.klen);
        key[x.klen] = 0;
        kv_buf_set(x.type == KV_T_BASE ? &n->base : &n->val, x.type, key, x.val, x.vlen);
    }
    kv_unlock(n);
}

static void changed(kv_ns_t *n, bool was_clean)
{
    n->gen++;
    n->changed_us = now_us();
    if (was_clean) n->dirty_us = n->changed_us;
}

static esp_err_t put(kv_ns_t *n, uint8_t type, const char *key, const void *v, size_t vlen)
{
    if (!n || !key) return ESP_ERR_INVALID_ARG;
    if (!kv_key_ok(key)) return strlen(key) ? ESP_ERR_NVS_KEY_TOO_LONG : ESP_ERR_NVS_INVALID_NAME;
    kv_lock(n);
    kv_rec_t x;
    const size_t old = kv_buf_find(&n->val, key, &x, NULL) ? 4u + x.klen + x.vlen : 0;
    if (n->val.len - old + 4 + strlen(key) + vlen + n->base.len > KV_ROOM) {
        kv_unlock(n);
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    const bool clean = n->done == n->gen;
    const int c = kv_buf_set(&n->val, type, key, v, vlen);
    if (c == 1) changed(n, clean);
    kv_unlock(n);
    return c == -1 ? ESP_ERR_NO_MEM : c < 0 ? ESP_ERR_INVALID_ARG : ESP_OK;
}

static esp_err_t get(kv_ns_t *n, uint8_t type, const char *key, void *out, size_t want)
{
    if (!n || !key || !out) return ESP_ERR_INVALID_ARG;
    kv_lock(n);
    kv_rec_t x;
    esp_err_t e = ESP_ERR_NVS_NOT_FOUND;
    if (kv_buf_find(&n->val, key, &x, NULL) && x.type == type && x.vlen == want) {
        memcpy(out, x.val, want);
        e = ESP_OK;
    }
    kv_unlock(n);
    return e;
}

static esp_err_t get_var(kv_ns_t *n, uint8_t type, const char *key, void *out, size_t *len)
{
    if (!n || !key || !len) return ESP_ERR_INVALID_ARG;
    kv_lock(n);
    kv_rec_t x;
    esp_err_t e = ESP_ERR_NVS_NOT_FOUND;
    if (kv_buf_find(&n->val, key, &x, NULL) && x.type == type) {
        if (!out) {
            *len = x.vlen;
            e = ESP_OK;
        } else if (*len < x.vlen) {
            *len = x.vlen;
            e = ESP_ERR_NVS_INVALID_LENGTH;
        } else {
            if (x.vlen) memcpy(out, x.val, x.vlen);
            *len = x.vlen;
            e = ESP_OK;
        }
    }
    kv_unlock(n);
    return e;
}

#define FIXED(sfx, T, code)                                                                             \
    esp_err_t kv_get_##sfx(kv_handle_t h, const char *key, T *out) { return get(h, code, key, out, sizeof(T)); } \
    esp_err_t kv_set_##sfx(kv_handle_t h, const char *key, T v) { return put(h, code, key, &v, sizeof v); }
FIXED(u8, uint8_t, KV_T_U8)
FIXED(i8, int8_t, KV_T_I8)
FIXED(u16, uint16_t, KV_T_U16)
FIXED(i16, int16_t, KV_T_I16)
FIXED(u32, uint32_t, KV_T_U32)
FIXED(i32, int32_t, KV_T_I32)
FIXED(u64, uint64_t, KV_T_U64)
FIXED(i64, int64_t, KV_T_I64)

esp_err_t kv_get_str(kv_handle_t h, const char *key, char *out, size_t *len)
{
    return get_var(h, KV_T_STR, key, out, len);
}

esp_err_t kv_get_blob(kv_handle_t h, const char *key, void *out, size_t *len)
{
    return get_var(h, KV_T_BLOB, key, out, len);
}

esp_err_t kv_set_str(kv_handle_t h, const char *key, const char *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    const size_t n = strlen(s) + 1;
    if (n > KV_STR_MAX) return ESP_ERR_NVS_VALUE_TOO_LONG;
    return put(h, KV_T_STR, key, s, n);
}

esp_err_t kv_set_blob(kv_handle_t h, const char *key, const void *v, size_t n)
{
    if (!v && n) return ESP_ERR_INVALID_ARG;
    if (n > 0xFFFF) return ESP_ERR_NVS_VALUE_TOO_LONG;
    return put(h, KV_T_BLOB, key, v, n);
}

char *kv_dup(kv_handle_t n, const char *key, size_t *len)
{
    if (!n || !key) return NULL;
    char *o = NULL;
    kv_lock(n);
    kv_rec_t x;
    if (kv_buf_find(&n->val, key, &x, NULL) && (x.type == KV_T_STR || x.type == KV_T_BLOB)) {
        o = heap_caps_malloc(x.vlen + 1u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (o) {
            if (x.vlen) memcpy(o, x.val, x.vlen);
            o[x.vlen] = 0;
            if (len) *len = x.type == KV_T_STR && x.vlen ? x.vlen - 1u : x.vlen;
        }
    }
    kv_unlock(n);
    return o;
}

esp_err_t kv_erase_key(kv_handle_t n, const char *key)
{
    if (!n || !key) return ESP_ERR_INVALID_ARG;
    kv_lock(n);
    const bool clean = n->done == n->gen;
    if (kv_buf_del(&n->val, key)) changed(n, clean);
    kv_unlock(n);
    return ESP_OK;
}

esp_err_t kv_erase_all(kv_handle_t n)
{
    if (!n) return ESP_ERR_INVALID_ARG;
    kv_lock(n);
    if (n->val.len) {
        const bool clean = n->done == n->gen;
        n->val.len = 0;
        changed(n, clean);
    }
    kv_unlock(n);
    return ESP_OK;
}

void kv_edit_begin(kv_handle_t n) { if (n) kv_lock(n); }
void kv_edit_end(kv_handle_t n) { if (n) kv_unlock(n); }

/* ---- commits ---- */

esp_err_t kv_commit(kv_handle_t n)
{
    if (!n) return ESP_ERR_INVALID_ARG;
    n->asked = true;
    kvw_kick();
    return ESP_OK;
}

esp_err_t kv_commit_now(kv_handle_t n)
{
    if (!n) return ESP_ERR_INVALID_ARG;
    n->urgent = true;
    return kv_commit(n);
}

static esp_err_t wait_for(kv_ns_t *n, uint32_t target, uint32_t ms)
{
    for (uint32_t t = 0;; t += 10) {
        if ((int32_t)(n->done - target) >= 0 && !n->scrub) return ESP_OK;
        if (n->failed && (int32_t)(n->failed - target) >= 0) return ESP_FAIL;
        if (t >= ms) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t kv_commit_wait(kv_handle_t n, uint32_t ms)
{
    if (!n) return ESP_ERR_INVALID_ARG;
    const uint32_t target = n->gen;
    if (n->done == target && !n->scrub) return ESP_OK;
    kv_commit_now(n);
    kvw_lend();
    const esp_err_t e = wait_for(n, target, ms);
    if (e == ESP_ERR_TIMEOUT) ESP_LOGW(TAG, "%s: not written within %u ms; it stays in RAM and goes later",
                                       n->def->name, (unsigned)ms);
    return e;
}

esp_err_t kv_scrub_wait(kv_handle_t n, uint32_t ms)
{
    if (!n) return ESP_ERR_INVALID_ARG;
    n->scrub = true;
    return kv_commit_wait(n, ms);
}

bool kv_saved(kv_handle_t n) { return n && n->done == n->gen && !n->scrub && !n->force; }

esp_err_t kv_flush(uint32_t ms)
{
    if (!s_up) return ESP_OK;
    for (int i = 0; i < kv_n; i++)
        if (!kv_saved(&kv_all[i])) {
            kv_all[i].urgent = true;
            kv_all[i].asked = true;
        }
    kvw_lend();
    for (uint32_t t = 0;; t += 10) {
        bool all = true;
        for (int i = 0; i < kv_n; i++) {
            kv_ns_t *n = &kv_all[i];
            if (!kv_saved(n) && !n->ro && !(n->failed && (int32_t)(n->failed - n->gen) >= 0)) all = false;
        }
        if (all) return ESP_OK;
        if (t >= ms) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Every esp_restart(): what waits is written first -- never while on the air. */
void kv_restart(void)
{
    if (kvg.hooks.busy && kvg.hooks.busy()) return;
    int left = 0;
    for (int i = 0; i < kv_n; i++) left += !kv_saved(&kv_all[i]);
    if (!left) return;
    const esp_err_t e = kv_flush(1500);
    ESP_LOGI(TAG, "at the restart: %d namespace(s) %s", left, e == ESP_OK ? "written" : "not all written");
}

/* ---- where they live ---- */

void kv_set_where(kv_where_t w, const char *why)
{
    kvg.where = w;
    snprintf(kvg.why, sizeof kvg.why, "%s", why ? why : "");
}

void kv_card_lost(const char *why)
{
    ESP_LOGE(TAG, "the card is off for this boot: %s; the settings go to NVS", why);
    kvg.card_ok = false;
    kvg.away = true;
    kv_set_where(KV_CARD_FAILED, why);
    for (int i = 0; i < kv_n; i++)
        if (kv_all[i].card) {
            kv_all[i].card = false;
            kv_all[i].force = true;
        }
}

kv_where_t kv_where(void) { return kvg.where; }

const char *kv_where_word(kv_where_t w)
{
    switch (w) {
    case KV_ON_CARD: return "card";
    case KV_IN_NVS: return "memory";
    case KV_CARD_MISSING: return "card missing";
    case KV_CARD_TROUBLE: return "card fault";
    case KV_CARD_FAILED: return "card failed";
    case KV_CARD_FOREIGN: return "other card";
    case KV_CARD_WIPE: return "wipe";
    }
    return "?";
}

bool kv_on_card(kv_handle_t n) { return n && n->card && kvg.card_ok; }

bool kv_identity_ok(kv_handle_t n) { return n && !kv_ident_ram(n); }

void kv_status(kv_status_t *o)
{
    memset(o, 0, sizeof *o);
    o->where = kvg.where;
    snprintf(o->why, sizeof o->why, "%s", kvg.why);
    for (int i = 0; i < kv_n; i++) {
        if (!kv_saved(&kv_all[i])) o->pending++;
        if (kv_all[i].held) {
            const size_t l = strlen(o->held);
            snprintf(o->held + l, sizeof o->held - l, "%s%s", l ? " " : "", kv_all[i].def->name);
        }
    }
    o->errors = kvg.errors;
    o->last_ms = kvg.last_ms;
    kvn_stats(&o->nvs_used, &o->nvs_free);
    o->nvs_full = kvg.nvs_full;
    o->can_prepare = kvg.can_prepare;
    o->can_again = kvg.where == KV_IN_NVS && kvg.kvs_state == KV_CARD_DECLINED;
    memcpy(o->gen, kvg.gen, sizeof o->gen);
}

void kv_confirmed(void) {}

esp_err_t kv_set_merge(const char *ns, kv_merge_cb fn)
{
    for (int i = 0; i < 16; i++)
        if (!s_merge_ns[i] || !strcmp(s_merge_ns[i], ns)) {
            s_merge_ns[i] = ns;
            s_merge[i] = fn;
            return ESP_OK;
        }
    return ESP_ERR_NO_MEM;
}

esp_err_t kv_open(const char *ns, kv_handle_t *out)
{
    if (!out || !ns) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!s_up) return ESP_ERR_INVALID_STATE;
    for (int i = 0; i < kv_n; i++)
        if (!strcmp(kv_all[i].def->name, ns)) {
            *out = &kv_all[i];
            return ESP_OK;
        }
    ESP_LOGE(TAG, "%s: not a namespace kvstore keeps", ns);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t kv_card_formatted(uint32_t ms) { return kvw_action(KV_DO_FORMATTED, ms); }
esp_err_t kv_card_prepare(uint32_t ms) { return kvw_action(KV_DO_PREPARE, ms); }
esp_err_t kv_card_use(bool keep, uint32_t ms) { return kvw_action(keep ? KV_DO_USE : KV_DO_FRESH, ms); }
esp_err_t kv_card_forget(uint32_t ms) { return kvw_action(KV_DO_FORGET, ms); }
esp_err_t kv_card_again(uint32_t ms) { return kvw_action(KV_DO_AGAIN, ms); }

/* ---- boot ---- */

static void say(void *ctx, const char *what, const char *key)
{
    ESP_LOGW(TAG, "%s/%s: %s", ((kv_ns_t *)ctx)->def->name, key, what);
}

static const char *word(kv_cstate_t s)
{
    return s == KV_VALID ? "ok" : s == KV_CORRUPT ? "bad" : s == KV_UNREADABLE ? "unreadable" : "none";
}

/* Every copy of every namespace, read within the budget. */
static void card_load(int64_t budget_us)
{
    const bool root = kvc_scan(kvg.gen);
    if (!root) {
        if (kvg.kvs_state == KV_CARD_IN_USE && kvg.kvs_cid == kvg.cid) {
            ESP_LOGE(TAG, "this card held the settings, and they have gone from it: running on NVS's copy");
            kv_set_where(KV_CARD_TROUBLE, "the settings have gone from this card");
            kvg.away = true;
            for (int i = 0; i < kv_n; i++) kv_all[i].held = true;
            return;
        }
        const bool other = kvg.kvs_state == KV_CARD_IN_USE && kvg.kvs_cid != kvg.cid;
        ESP_LOGW(TAG, "%s: the settings the knob holds go onto it", other ? "a new card" : "no settings on the card yet");
        kv_set_where(KV_ON_CARD, other ? "a new card: the settings the knob held went onto it" : "");
        kvg.card_ok = true;
        for (int i = 0; i < kv_n; i++) {
            kv_all[i].card = true;
            for (int s = 0; s < KV_SLOTS; s++) kv_all[i].cp[s] = (kv_copy_t){ KV_ABSENT, false, 0 };
        }
        return;
    }
    const int64_t t0 = now_us();
    int errs = 0, held = 0;
    bool foreign = false;
    for (int i = 0; i < kv_n; i++) {
        kv_ns_t *n = &kv_all[i];
        const int slots = kv_slots(n);
        bool loaded = false;
        uint32_t lseq = 0;
        for (int s = 0; s < KV_SLOTS; s++) {
            kv_copy_t c = { s < slots ? KV_UNREADABLE : KV_ABSENT, false, 0 };
#if VFO_KV_TEST
            if (s < slots && kv_fake.ioerr & (1u << (i * 3 + s))) {
                ESP_LOGW(TAG, "%s %c: test -- read taken as failed", n->def->name, 'A' + s);
                n->cp[s] = c;                    /* unreadable; not counted against the card */
                continue;
            }
#endif
            if (s < slots && now_us() - t0 < budget_us && errs < 2) {
                kv_hdr_t h;
                bool parsed;
                c.st = kvc_read(n, s, &h, &parsed);
#if VFO_KV_TEST
                if (kv_fake.crashload) {
                    ESP_LOGE(TAG, "test: a crash in the middle of reading the card");
                    abort();
                }
#endif
                c.parsed = parsed;
                c.seq = parsed ? h.seq : 0;
                if (c.st == KV_UNREADABLE) errs++;
                if (c.st == KV_VALID) {
                    if (memcmp(h.mac, kvg.mac, 6)) foreign = true;
#if VFO_KV_TEST
                    if (kv_fake.foreign) foreign = true;
#endif
                    if (!loaded || h.seq > lseq) {
                        loaded = true;
                        lseq = h.seq;
                        n->ro = h.format > 1;
                        if (!n->ro) kv_load_recs(n, kv_img + KV_HDR, h.len);
                    }
                }
            }
            n->cp[s] = c;
        }
        const kv_choice_t ch = kv_choose(n->cp, slots);
        n->seq_hi = ch.next_seq - 1;
        if (ch.held) {
            n->held = true;
            n->val.len = n->base.len = 0;
            held++;
        } else {
            n->card = true;
            n->seq = ch.winner >= 0 ? n->cp[ch.winner].seq : 0;
            n->repair = ch.repair;
        }
        if (ch.winner >= 0 || ch.held || ch.repair) {
            char c3[24] = "", to[8] = "empty";
            if (slots > 2) snprintf(c3, sizeof c3, ", C %s %u", word(n->cp[2].st), (unsigned)n->cp[2].seq);
            if (ch.winner >= 0) snprintf(to, sizeof to, "%c", 'A' + ch.winner);
            ESP_LOGI(TAG, "%s: A %s %u, B %s %u%s -> %s%s%s", n->def->name, word(n->cp[0].st), (unsigned)n->cp[0].seq,
                     word(n->cp[1].st), (unsigned)n->cp[1].seq, c3, ch.held ? "HELD, not written this boot" : to,
                     ch.repair ? ", the others written again" : "", n->ro ? " (a newer firmware's: left alone)" : "");
        }
    }
    if (foreign) {
        ESP_LOGW(TAG, "the settings on this card are another knob's: not used until you choose on the page");
        kv_set_where(KV_CARD_FOREIGN, "the settings on this card are another knob's");
        kvg.away = true;
        for (int i = 0; i < kv_n; i++) {
            kv_all[i].card = kv_all[i].held = kv_all[i].ro = false;
            kv_all[i].val.len = kv_all[i].base.len = 0;
        }
        return;
    }
    kvg.card_ok = true;
    if (held) {
        char why[64];
        snprintf(why, sizeof why, "%d part(s) of the settings unreadable on the card", held);
        kv_set_where(KV_CARD_TROUBLE, why);
    } else kv_set_where(KV_ON_CARD, "");
    ESP_LOGI(TAG, "read in %u ms, folders A%u B%u C%u", (unsigned)((now_us() - t0) / 1000), kvg.gen[0], kvg.gen[1],
             kvg.gen[2]);
}

/* NVS's copy of each namespace: merged onto the card's, or used as it is. */
static void nvs_side(void)
{
    for (int i = 0; i < kv_n; i++) {
        kv_ns_t *n = &kv_all[i];
        if (n->def->flags & KVF_CACHE) continue;           /* never in NVS */
        kv_buf_t e = { 0 };
        if (kvn_load(n->def, &e) != ESP_OK) ESP_LOGW(TAG, "%s: NVS's copy not read whole", n->def->name);
        if (kv_base_of(&n->ebase, &e) < 0) ESP_LOGE(TAG, "%s: no memory for the base", n->def->name);
        if (n->card && !n->ro) {
            const kv_rules_t r = { .identity = n->def->identity, .group = n->def->group, .fn = n->merge,
                                   .say = say, .ctx = n };
            kv_merged_t m;
            int c = kv_merge(&n->val, &n->base, &e, &r, &m);
            if (c < 0) {
                ESP_LOGE(TAG, "%s: no memory for the merge: NVS's copy used this boot", n->def->name);
                n->card = false;
            } else {
                if (!kv_buf_same(&n->base, &n->ebase)) {
                    if (kv_buf_copy(&n->base, n->ebase.p, n->ebase.len) == 0) c = 1;
                }
                if (m.added || m.changed || m.deleted)
                    ESP_LOGI(TAG, "%s: from NVS onto the card: %u added, %u changed, %u erased%s", n->def->name,
                             m.added, m.changed, m.deleted, m.group_nvs ? " (its group whole)" : "");
                if (n->val.len + n->base.len > KV_ROOM) {
                    ESP_LOGE(TAG, "%s: %u B with its base, more than a copy holds: NVS's copy used this boot",
                             n->def->name, (unsigned)(n->val.len + n->base.len));
                    n->card = false;
                } else if (c) {
                    n->gen++;
                    n->changed_us = n->dirty_us = now_us();
                    n->asked = n->urgent = true;
                }
            }
        }
        /* Off the card this boot -- or a newer firmware's there, left alone
         * and never written: NVS's copy, as it is. */
        if (!n->card || n->ro) {
            kv_buf_t t = n->val;
            n->val = e;
            e = t;
            n->base.len = 0;
            if (n->ro) n->card = false;
        }
        kv_buf_free(&e);
    }
}

esp_err_t kv_init(const kv_hooks_t *hooks, const kv_boot_t *boot)
{
    if (s_up) return ESP_OK;
    const int64_t t0 = now_us();
    if (hooks) kvg.hooks = *hooks;
    kv_img = heap_caps_malloc(KV_FILE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    kv_chk = heap_caps_malloc(KV_FILE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!kv_img || !kv_chk) return ESP_ERR_NO_MEM;
    esp_efuse_mac_get_default(kvg.mac);
    strlcpy(kvg.version, esp_app_get_description()->version, sizeof kvg.version);
    kv_all = s_ns;
    kv_n = KV_NS_N < 16 ? KV_NS_N : 16;
    for (int i = 0; i < kv_n; i++) {
        kv_ns_t *n = &kv_all[i];
        memset(n, 0, sizeof *n);
        n->def = &KV_NS[i];
        n->mx = xSemaphoreCreateRecursiveMutexStatic(&n->mxb);
        n->c_us = t0;
        for (int k = 0; k < 16 && s_merge_ns[k]; k++)
            if (!strcmp(s_merge_ns[k], n->def->name)) n->merge = s_merge[k];
    }
    const bool skip = kvc_rtc_skip();
#if VFO_KV_TEST
    kv_test_boot();
#endif
    uint32_t used, freec;
    kvn_stats(&used, &freec);
    ESP_LOGI(TAG, "NVS: %u entries used, %u free", (unsigned)used, (unsigned)freec);
    kvn_card_get(&kvg.kvs_state, &kvg.kvs_cid);

    if (boot && boot->wipe) {
        kv_set_where(KV_CARD_WIPE, "the card waits to be emptied (provisioning)");
        kvg.can_prepare = true;
    } else if (boot && boot->safe) {
        kv_set_where(kvg.kvs_state == KV_CARD_IN_USE ? KV_CARD_MISSING : KV_IN_NVS, "safe mode: the card is left alone");
    } else if (skip) {
        ESP_LOGE(TAG, "skipped: the last start stopped in a card operation");
        kv_set_where(KV_CARD_MISSING, "skipped: the last start stopped in a card operation");
    } else {
        kvc_stage(KV_STAGE_CARD);
        kvg.mounted = sdc_mount();
#if VFO_KV_TEST
        if (kvg.mounted && kv_fake.nocard) {
            ESP_LOGW(TAG, "test: the card taken as not answering");
            sdc_unmount();
            kvg.mounted = false;
        }
#endif
        if (!kvg.mounted) {
            const bool nofat = sdc_last_error() == ESP_FAIL;
            kvg.can_prepare = nofat;
            kv_set_where(kvg.kvs_state == KV_CARD_IN_USE ? KV_CARD_MISSING : KV_IN_NVS,
                         nofat ? "the card holds no FAT the knob reads" : "no card answers");
        } else {
            kvg.cid = sdc_cid();
            if (kvg.kvs_state == KV_CARD_DECLINED && kvg.kvs_cid == kvg.cid) {
                kv_set_where(KV_IN_NVS, "you chose to carry on without this card");
                sdc_unmount();
                kvg.mounted = false;
            } else card_load((kvg.kvs_state == KV_CARD_IN_USE ? 2000 : 3000) * 1000LL);
        }
        kvc_stage(KV_STAGE_IDLE);
    }
    if (kvg.where != KV_ON_CARD && kvg.where != KV_IN_NVS && kvg.where != KV_CARD_WIPE) kvg.away = true;
    nvs_side();
    if (kvg.card_ok && (kvg.kvs_state != KV_CARD_IN_USE || kvg.kvs_cid != kvg.cid)) {
        if (kvn_card_set(KV_CARD_IN_USE, kvg.cid, false) == ESP_OK) {
            kvg.kvs_state = KV_CARD_IN_USE;
            kvg.kvs_cid = kvg.cid;
        }
    }
    s_up = true;
    const esp_err_t e = kvw_start();
    if (e != ESP_OK) ESP_LOGE(TAG, "the writer did not start: %s", esp_err_to_name(e));
    sdc_on_restart(kv_restart);
    ESP_LOGI(TAG, "settings: %s%s%s (%u ms)", kv_where_word(kvg.where), kvg.why[0] ? ": " : "", kvg.why,
             (unsigned)((now_us() - t0) / 1000));
    return e;
}
