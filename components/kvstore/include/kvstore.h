/* The knob's settings: every firmware's, on the SD card, the way NVS keeps
 * them -- namespaces, keys and NVS's own types and answers. WiFi and the
 * Bluetooth records stay in NVS, read there directly.
 *
 * Every namespace is held in PSRAM from boot: a get reads RAM, a set changes
 * RAM, and one task (kvwr) writes. No call here touches flash or the card, so
 * any task may make them, on any stack -- PSRAM stacks and esp_timer's
 * included, never an ISR. On the card each namespace is three copies (A and
 * B, written in turn and read back, and C ten minutes behind); a power cut
 * leaves the last completed save. A knob without a card keeps its settings in
 * NVS, as before. See SETTINGS-ON-CARD-PLAN.md.
 *
 * For now the NVS copies stay where they are, frozen: a firmware from before
 * the card still finds its settings, and what it changes there is merged
 * back onto the card at the next start. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h"            /* its error codes */

typedef struct kv_ns *kv_handle_t;      /* one per namespace for the whole boot; never freed */

typedef struct {
    bool (*busy)(void);     /* an over or a call: NVS writes wait; the restart flush is skipped */
    bool (*sound)(void);    /* sound playing: an ordinary commit to NVS waits, up to 10 min */
} kv_hooks_t;
typedef struct {
    bool wipe;              /* vfo/sdwipe is set: the card's settings are not read */
    bool safe;              /* safe mode: the card is not touched */
} kv_boot_t;

/* On the main task, after NVS and before anything reads a setting. */
esp_err_t kv_init(const kv_hooks_t *hooks, const kv_boot_t *boot);
/* [a-z0-9]{1,8}, a namespace of the table (kv_ns.c); ESP_ERR_INVALID_STATE
 * before kv_init, ESP_ERR_NOT_FOUND for one the table lacks. */
esp_err_t kv_open(const char *ns, kv_handle_t *out);
static inline void kv_close(kv_handle_t h) { (void)h; }

/* As nvs_get_*: ESP_ERR_NVS_NOT_FOUND when absent or of another type. str and
 * blob: out NULL asks the length (str: with its NUL); a short buffer gives
 * ESP_ERR_NVS_INVALID_LENGTH with *len set. */
esp_err_t kv_get_u8 (kv_handle_t h, const char *key, uint8_t *out);
esp_err_t kv_get_i8 (kv_handle_t h, const char *key, int8_t *out);
esp_err_t kv_get_u16(kv_handle_t h, const char *key, uint16_t *out);
esp_err_t kv_get_i16(kv_handle_t h, const char *key, int16_t *out);
esp_err_t kv_get_u32(kv_handle_t h, const char *key, uint32_t *out);
esp_err_t kv_get_i32(kv_handle_t h, const char *key, int32_t *out);
esp_err_t kv_get_u64(kv_handle_t h, const char *key, uint64_t *out);
esp_err_t kv_get_i64(kv_handle_t h, const char *key, int64_t *out);
esp_err_t kv_get_str (kv_handle_t h, const char *key, char *out, size_t *len);
esp_err_t kv_get_blob(kv_handle_t h, const char *key, void *out, size_t *len);
/* A PSRAM copy with a NUL after it, *len (if asked) its length; the caller
 * frees it. NULL when absent. */
char     *kv_dup(kv_handle_t h, const char *key, size_t *len);

/* RAM only, and only if the value changes. A str holds at most 4000 B with
 * its NUL (ESP_ERR_NVS_VALUE_TOO_LONG); a namespace at most 16 320 B of
 * records, past which ESP_ERR_NVS_NOT_ENOUGH_SPACE, as NVS would say. */
esp_err_t kv_set_u8 (kv_handle_t h, const char *key, uint8_t v);
esp_err_t kv_set_i8 (kv_handle_t h, const char *key, int8_t v);
esp_err_t kv_set_u16(kv_handle_t h, const char *key, uint16_t v);
esp_err_t kv_set_i16(kv_handle_t h, const char *key, int16_t v);
esp_err_t kv_set_u32(kv_handle_t h, const char *key, uint32_t v);
esp_err_t kv_set_i32(kv_handle_t h, const char *key, int32_t v);
esp_err_t kv_set_u64(kv_handle_t h, const char *key, uint64_t v);
esp_err_t kv_set_i64(kv_handle_t h, const char *key, int64_t v);
esp_err_t kv_set_str (kv_handle_t h, const char *key, const char *s);
esp_err_t kv_set_blob(kv_handle_t h, const char *key, const void *v, size_t n);
esp_err_t kv_erase_key(kv_handle_t h, const char *key);     /* ESP_OK when absent too */
esp_err_t kv_erase_all(kv_handle_t h);

/* Sets that reach the medium together: the namespace's (recursive) mutex is
 * held between them. Only kv sets and erases, and plain computation, in
 * between: no I/O, no logging, no other lock. */
void kv_edit_begin(kv_handle_t h);
void kv_edit_end(kv_handle_t h);

/* Written soon (about 20 ms, batched), never blocking. A change never
 * committed is written once its namespace has been quiet for 5 s. */
esp_err_t kv_commit(kv_handle_t h);
/* The same, past the wait for sound that NVS writes otherwise keep. */
esp_err_t kv_commit_now(kv_handle_t h);
/* kv_commit_now, then wait until written and read back: ESP_OK; ESP_ERR_TIMEOUT
 * (in RAM, written later -- callers count it as saved); ESP_FAIL (no medium
 * took it: the card failed, or NVS is full). Never from esp_timer, LVGL or
 * while holding a lock another task needs. */
esp_err_t kv_commit_wait(kv_handle_t h, uint32_t ms);
bool      kv_saved(kv_handle_t h);                      /* nothing of it waits to be written */
/* After erasing a secret: the erased keys also leave the frozen NVS copy,
 * then every card copy is rewritten. Blocks, as kv_commit_wait. */
esp_err_t kv_scrub_wait(kv_handle_t h, uint32_t ms);
/* Everything not yet written; the restart flush. */
esp_err_t kv_flush(uint32_t ms);

typedef enum {
    KV_ON_CARD,             /* this knob's card: every namespace on it */
    KV_IN_NVS,              /* no card, never one, or the owner carried on without it */
    KV_CARD_MISSING,        /* this knob's card does not answer: changes kept in NVS meanwhile */
    KV_CARD_TROUBLE,        /* the card answers, but some namespace could not be read, or its settings went */
    KV_CARD_FAILED,         /* the card stopped taking writes this boot */
    KV_CARD_FOREIGN,        /* the settings on the card are another knob's */
    KV_CARD_WIPE,           /* provisioning's mark is set: the card waits to be emptied */
} kv_where_t;
kv_where_t  kv_where(void);
const char *kv_where_word(kv_where_t w);     /* "card" "memory" "card missing" ... for the page */
bool        kv_on_card(kv_handle_t h);       /* this namespace is the card's this boot */
/* A generated identity (a station key, a station id) may be made into it:
 * false while the card that holds the namespace is away. */
bool        kv_identity_ok(kv_handle_t h);

typedef struct {
    kv_where_t where;
    char       why[64];
    char       held[64];          /* the namespaces held off the card, by name */
    uint16_t   pending, errors;
    uint32_t   last_ms;           /* the last card write's time */
    uint32_t   nvs_used, nvs_free;
    bool       nvs_full;          /* a save to NVS found no room */
    bool       can_prepare;       /* a card answers that the knob cannot read */
    bool       can_again;         /* the owner carried on without the card: it may be taken again */
    uint8_t    gen[3];            /* the folders' generations, A B C */
} kv_status_t;
void kv_status(kv_status_t *out);

/* From the confirmed boot (boot_ok). Emptying the frozen NVS copies waits for
 * the release that brings every firmware onto the card: nothing yet. */
void kv_confirmed(void);

/* A namespace's own merge of a key both sides hold differently. Before kv_init. */
typedef bool (*kv_merge_cb)(const char *key, const void *card, size_t clen, const void *nvs, size_t nlen,
                            void *out, size_t *olen);
esp_err_t kv_set_merge(const char *ns, kv_merge_cb fn);

/* The owner's actions, carried out by kvwr; the caller waits up to ms. */
esp_err_t kv_card_formatted(uint32_t ms);    /* setup: the card emptied at provisioning, taken as new */
esp_err_t kv_card_prepare(uint32_t ms);      /* a card the knob cannot read, or marked to be emptied: formatted, taken */
/* A card with another knob's settings, or whose settings went: keep = its
 * settings become this knob's (restart after); else this knob's go onto it,
 * over what it holds. */
esp_err_t kv_card_use(bool keep, uint32_t ms);
esp_err_t kv_card_forget(uint32_t ms);       /* carry on without the SD card */
/* The card carried on without, used again from the next start (restart
 * after): what changed in NVS meanwhile is merged onto what the card holds. */
esp_err_t kv_card_again(uint32_t ms);

#if VFO_KV_TEST
/* kv_test.c, a test build's only: the namespaces as the writer sees them, as
 * JSON (never a value); and what=hammer|stop|panic|nocard|foreign|ioerr|
 * corrupt|rewrite, with ns and slots ("ABC") where they apply. */
void      kv_test_start(void);
size_t    kv_test_json(char *out, size_t cap);
esp_err_t kv_test_do(const char *what, const char *ns, const char *slots, char *say, size_t cap);
#endif
