/* kvstore's side of NVS: reading a namespace's copy at boot, writing one
 * when the card does not hold it, and kvstore's own records (namespace kvs).
 * A flash write stops the cache, so whatever writes here after boot runs on
 * a short-lived helper whose stack is internal RAM -- kvwr's is in PSRAM. */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "kv_priv.h"
#include "nvs.h"

static const char *TAG = "kv";

#define KVS        "kvs"
#define CARD_MAGIC 0x3143564Bu                   /* "KVC1" */

typedef struct __attribute__((packed)) {
    uint32_t magic, cid;
    uint8_t  state;
} card_rec_t;

static void *ps(size_t n) { return heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

/* One NVS entry's value onto e. */
static esp_err_t take(nvs_handle_t h, const nvs_entry_info_t *in, kv_buf_t *e)
{
    uint8_t v[8];
    size_t n = 0;
    esp_err_t r;
    switch (in->type) {
    case NVS_TYPE_U8: r = nvs_get_u8(h, in->key, (uint8_t *)v); n = 1; break;
    case NVS_TYPE_I8: r = nvs_get_i8(h, in->key, (int8_t *)v); n = 1; break;
    case NVS_TYPE_U16: r = nvs_get_u16(h, in->key, (uint16_t *)v); n = 2; break;
    case NVS_TYPE_I16: r = nvs_get_i16(h, in->key, (int16_t *)v); n = 2; break;
    case NVS_TYPE_U32: r = nvs_get_u32(h, in->key, (uint32_t *)v); n = 4; break;
    case NVS_TYPE_I32: r = nvs_get_i32(h, in->key, (int32_t *)v); n = 4; break;
    case NVS_TYPE_U64: r = nvs_get_u64(h, in->key, (uint64_t *)v); n = 8; break;
    case NVS_TYPE_I64: r = nvs_get_i64(h, in->key, (int64_t *)v); n = 8; break;
    case NVS_TYPE_STR:
    case NVS_TYPE_BLOB: {
        const bool str = in->type == NVS_TYPE_STR;
        r = str ? nvs_get_str(h, in->key, NULL, &n) : nvs_get_blob(h, in->key, NULL, &n);
        if (r != ESP_OK) return r;
        if (n > 0xFFFF) return ESP_ERR_NVS_VALUE_TOO_LONG;
        uint8_t *b = ps(n);
        if (!b) return ESP_ERR_NO_MEM;
        r = str ? nvs_get_str(h, in->key, (char *)b, &n) : nvs_get_blob(h, in->key, b, &n);
        if (r == ESP_OK && kv_buf_set(e, (uint8_t)in->type, in->key, b, n) < 0) r = ESP_ERR_NO_MEM;
        free(b);
        return r;
    }
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    if (r == ESP_OK && kv_buf_set(e, (uint8_t)in->type, in->key, v, n) < 0) r = ESP_ERR_NO_MEM;
    return r;
}

esp_err_t kvn_load(const kv_ns_def_t *d, kv_buf_t *e)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(d->name, NVS_READONLY, &h);             /* read-only: no namespace made */
    if (r == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;                  /* nothing there */
    if (r != ESP_OK) return r;
    char drop[8][NVS_KEY_NAME_MAX_SIZE];
    int ndrop = 0, bad = 0;
    nvs_iterator_t it = NULL;
    esp_err_t f = nvs_entry_find(NVS_DEFAULT_PART_NAME, d->name, NVS_TYPE_ANY, &it);
    while (f == ESP_OK) {
        nvs_entry_info_t in;
        nvs_entry_info(it, &in);
        if (kv_listed(d->drop, in.key)) {
            if (ndrop < 8) strlcpy(drop[ndrop++], in.key, sizeof drop[0]);
        } else if (!kv_listed(d->nvs_keep, in.key) && take(h, &in, e) != ESP_OK) {
            ESP_LOGW(TAG, "%s/%s: not read from NVS", d->name, in.key);
            bad++;
        }
        f = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(h);
    if (ndrop && nvs_open(d->name, NVS_READWRITE, &h) == ESP_OK) {
        for (int i = 0; i < ndrop; i++) nvs_erase_key(h, drop[i]);
        const esp_err_t c = nvs_commit(h);
        nvs_close(h);
        for (int i = 0; i < ndrop; i++)
            ESP_LOGI(TAG, "%s/%s: off NVS%s", d->name, drop[i], c == ESP_OK ? "" : " (not committed)");
    }
    return bad ? ESP_FAIL : ESP_OK;
}

/* ---- the helper ---- */

typedef struct {
    kv_ns_t       *n;
    const uint8_t *recs;
    size_t         len;
    bool           set, skip_ident;
    esp_err_t      out;
    card_rec_t     card;                         /* with n NULL: kvs/card */
} job_t;

static StaticSemaphore_t s_done_buf;
static SemaphoreHandle_t s_done;

static esp_err_t write_ns(job_t *j)
{
    const kv_ns_def_t *d = j->n->def;
    nvs_handle_t h;
    esp_err_t r = nvs_open(d->name, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    kv_buf_t ram = { .p = (uint8_t *)j->recs, .len = j->len, .cap = j->len };
    /* NVS's keys first, then those RAM lacks erased: the iterator is not
     * walked while erasing. */
    char (*keys)[NVS_KEY_NAME_MAX_SIZE] = NULL;
    uint8_t *types = NULL;
    int nk = 0, cap = 0;
    nvs_iterator_t it = NULL;
    esp_err_t f = nvs_entry_find(NVS_DEFAULT_PART_NAME, d->name, NVS_TYPE_ANY, &it);
    while (f == ESP_OK) {
        nvs_entry_info_t in;
        nvs_entry_info(it, &in);
        if (nk == cap) {
            cap = cap ? cap * 2 : 32;
            void *k2 = heap_caps_realloc(keys, cap * sizeof *keys, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            void *t2 = k2 ? heap_caps_realloc(types, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
            if (k2) keys = k2;
            if (!k2 || !t2) {
                r = ESP_ERR_NO_MEM;
                break;
            }
            types = t2;
        }
        strlcpy(keys[nk], in.key, sizeof keys[0]);
        types[nk++] = (uint8_t)in.type;
        f = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    int erased = 0, set = 0;
    for (int i = 0; r == ESP_OK && i < nk; i++) {
        if (kv_listed(d->nvs_keep, keys[i]) || (j->skip_ident && kv_listed(d->identity, keys[i]))) continue;
        kv_rec_t x;
        const bool in_ram = kv_buf_find(&ram, keys[i], &x, NULL);
        if (in_ram && (x.type == types[i] || !j->set)) continue;
        const esp_err_t e = nvs_erase_key(h, keys[i]);
        if (e == ESP_OK) erased++;
        else if (e != ESP_ERR_NVS_NOT_FOUND) r = e;
    }
    free(keys);
    free(types);
    size_t off = 0;
    kv_rec_t x;
    while (r == ESP_OK && j->set && kv_rec_next(j->recs, j->len, &off, &x)) {
        char k[KV_KEY_MAX + 1];
        memcpy(k, x.key, x.klen);
        k[x.klen] = 0;
        if (kv_listed(d->nvs_keep, k) || (j->skip_ident && kv_listed(d->identity, k))) continue;
        uint64_t v = 0;
        if (x.vlen <= 8) memcpy(&v, x.val, x.vlen);
        switch (x.type) {
        case KV_T_U8: r = nvs_set_u8(h, k, (uint8_t)v); break;
        case KV_T_I8: r = nvs_set_i8(h, k, (int8_t)v); break;
        case KV_T_U16: r = nvs_set_u16(h, k, (uint16_t)v); break;
        case KV_T_I16: r = nvs_set_i16(h, k, (int16_t)v); break;
        case KV_T_U32: r = nvs_set_u32(h, k, (uint32_t)v); break;
        case KV_T_I32: r = nvs_set_i32(h, k, (int32_t)v); break;
        case KV_T_U64: r = nvs_set_u64(h, k, v); break;
        case KV_T_I64: r = nvs_set_i64(h, k, (int64_t)v); break;
        case KV_T_STR: r = nvs_set_str(h, k, (const char *)x.val); break;
        case KV_T_BLOB: r = nvs_set_blob(h, k, x.val, x.vlen); break;
        default: continue;                       /* a type NVS has no call for: the card's alone */
        }
        if (r == ESP_OK) set++;
        else ESP_LOGW(TAG, "%s/%s: not set in NVS: %s", d->name, k, esp_err_to_name(r));
    }
    const esp_err_t c = nvs_commit(h);
    nvs_close(h);
    if (r == ESP_OK) r = c;
    if (r == ESP_OK && erased) ESP_LOGI(TAG, "%s: %d key(s) erased from NVS", d->name, erased);
    return r;
}

static void helper(void *arg)
{
    job_t *j = arg;
    if (j->n) j->out = write_ns(j);
    else {
        nvs_handle_t h;
        j->out = nvs_open(KVS, NVS_READWRITE, &h);
        if (j->out == ESP_OK) {
            j->out = nvs_set_blob(h, "card", &j->card, sizeof j->card);
            if (j->out == ESP_OK) j->out = nvs_commit(h);
            nvs_close(h);
        }
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

static esp_err_t run(job_t *j)
{
    if (!s_done) s_done = xSemaphoreCreateBinaryStatic(&s_done_buf);
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    if (prio < 2) prio = 2;
    if (xTaskCreatePinnedToCore(helper, "kvnvs", 4096, j, prio, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "no room for NVS's helper");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_done, portMAX_DELAY);
    return j->out;
}

esp_err_t kvn_write(kv_ns_t *n, const uint8_t *recs, size_t len, bool set, bool skip_ident)
{
    job_t j = { .n = n, .recs = recs, .len = len, .set = set, .skip_ident = skip_ident };
    return run(&j);
}

void kvn_card_get(uint32_t *state, uint32_t *cid)
{
    *state = KV_CARD_NONE;
    *cid = 0;
    nvs_handle_t h;
    if (nvs_open(KVS, NVS_READONLY, &h) != ESP_OK) return;
    card_rec_t c;
    size_t n = sizeof c;
    if (nvs_get_blob(h, "card", &c, &n) == ESP_OK && n == sizeof c && c.magic == CARD_MAGIC) {
        *state = c.state;
        *cid = c.cid;
    }
    nvs_close(h);
}

esp_err_t kvn_card_set(uint32_t state, uint32_t cid, bool on_helper)
{
    job_t j = { .card = { .magic = CARD_MAGIC, .cid = cid, .state = (uint8_t)state } };
    esp_err_t e;
    if (on_helper) e = run(&j);
    else {
        nvs_handle_t h;
        e = nvs_open(KVS, NVS_READWRITE, &h);
        if (e == ESP_OK) {
            e = nvs_set_blob(h, "card", &j.card, sizeof j.card);
            if (e == ESP_OK) e = nvs_commit(h);
            nvs_close(h);
        }
    }
    if (e != ESP_OK) ESP_LOGW(TAG, "kvs/card not kept in NVS: %s", esp_err_to_name(e));
    return e;
}

void kvn_stats(uint32_t *used, uint32_t *freec)
{
    nvs_stats_t st = { 0 };
    nvs_get_stats(NULL, &st);
    *used = st.used_entries;
    *freec = st.free_entries;
}
