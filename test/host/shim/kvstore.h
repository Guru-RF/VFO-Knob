/* components/kvstore on the PC: the knob's settings straight onto the shim's
 * NVS (shim.c's file), written at once -- as if on a card that never fails. */
#ifndef SHIM_KVSTORE_H
#define SHIM_KVSTORE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "nvs.h"

typedef nvs_handle_t kv_handle_t;

static inline esp_err_t kv_open(const char *ns, kv_handle_t *h) { return nvs_open(ns, NVS_READWRITE, h); }
static inline void kv_close(kv_handle_t h) { nvs_close(h); }
#define kv_get_str  nvs_get_str
#define kv_set_str  nvs_set_str
#define kv_get_blob nvs_get_blob
#define kv_set_blob nvs_set_blob
#define kv_get_u8   nvs_get_u8
#define kv_set_u8   nvs_set_u8
#define kv_get_i8   nvs_get_i8
#define kv_set_i8   nvs_set_i8
#define kv_get_u32  nvs_get_u32
#define kv_set_u32  nvs_set_u32
#define kv_get_i32  nvs_get_i32
#define kv_set_i32  nvs_set_i32
#define kv_get_i64  nvs_get_i64
#define kv_set_i64  nvs_set_i64
static inline esp_err_t kv_erase_key(kv_handle_t h, const char *key)
{
    const esp_err_t e = nvs_erase_key(h, key);
    return e == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : e;
}
static inline void kv_edit_begin(kv_handle_t h) { (void)h; }
static inline void kv_edit_end(kv_handle_t h) { (void)h; }
static inline esp_err_t kv_commit(kv_handle_t h) { return nvs_commit(h); }
static inline esp_err_t kv_commit_now(kv_handle_t h) { return nvs_commit(h); }
static inline esp_err_t kv_commit_wait(kv_handle_t h, unsigned ms) { (void)ms; return nvs_commit(h); }
static inline bool kv_saved(kv_handle_t h) { (void)h; return true; }
static inline bool kv_on_card(kv_handle_t h) { (void)h; return false; }
/* Where the settings are: on the PC, the shim's NVS file -- "memory". */
typedef enum { KV_ON_CARD, KV_IN_NVS, KV_CARD_MISSING, KV_CARD_TROUBLE, KV_CARD_FAILED, KV_CARD_FOREIGN,
               KV_CARD_WIPE } kv_where_t;
typedef struct {
    kv_where_t where;
    char       why[64], held[64];
    uint16_t   pending, errors;
    uint32_t   last_ms, nvs_used, nvs_free;
    bool       nvs_full, can_prepare, can_again;
    uint8_t    gen[3];
} kv_status_t;
static inline void kv_status(kv_status_t *o) { *o = (kv_status_t){ .where = KV_IN_NVS }; }
static inline const char *kv_where_word(kv_where_t w) { return w == KV_IN_NVS ? "memory" : "card"; }
static inline esp_err_t kv_card_forget(unsigned ms) { (void)ms; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t kv_card_prepare(unsigned ms) { (void)ms; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t kv_card_use(bool keep, unsigned ms) { (void)keep; (void)ms; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t kv_card_again(unsigned ms) { (void)ms; return ESP_ERR_NOT_SUPPORTED; }
static inline char *kv_dup(kv_handle_t h, const char *key, size_t *len)
{
    size_t n = 0;
    if (nvs_get_str(h, key, NULL, &n) != ESP_OK) return NULL;
    char *o = malloc(n + 1);
    if (o && nvs_get_str(h, key, o, &n) != ESP_OK) {
        free(o);
        return NULL;
    }
    if (o) {
        o[n] = 0;
        if (len) *len = n ? n - 1 : 0;
    }
    return o;
}
#endif
