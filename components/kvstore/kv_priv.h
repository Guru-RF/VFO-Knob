/* kvstore's insides, shared by its files. See kvstore.h. */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kv_img.h"
#include "kvstore.h"

enum {
    KVF_CACHE  = 1,         /* RAM only without a card; never taken from NVS (erased there); no C copy */
    KVF_FROZEN = 2,         /* the NVS copy stays in place: a pre-card firmware still reads it */
};

typedef struct {
    const char        *name;
    uint8_t            flags;
    const char *const *nvs_keep;   /* stays in NVS: never read, moved or erased by kvstore */
    const char *const *identity;   /* generated or learned: kept with the card */
    const char *const *group;      /* merged as one, every key from the same side */
    const char *const *drop;       /* erased from NVS at every boot that finds them, never moved */
} kv_ns_def_t;

extern const kv_ns_def_t KV_NS[];
extern const int         KV_NS_N;

#define KV_SLOTS   3

struct kv_ns {
    const kv_ns_def_t *def;
    SemaphoreHandle_t  mx;
    StaticSemaphore_t  mxb;
    kv_buf_t           val, base;    /* the values; the NVS copy as at the last merge */
    kv_buf_t           ebase;        /* the NVS copy as this boot found it */
    volatile uint32_t  gen;          /* changes made */
    volatile uint32_t  done;         /* the change last written to its medium */
    volatile uint32_t  failed;       /* the change that no medium took (0: none) */
    volatile bool      asked, urgent, scrub, force;   /* force: written to NVS whole (the card was lost) */
    int64_t            changed_us, dirty_us, retry_us;
    bool               card, held, ro;
    uint32_t           seq, seq_hi;  /* the copies' content; the highest seen */
    kv_copy_t          cp[KV_SLOTS];
    uint8_t            repair;       /* copies to write again from RAM */
    uint32_t           clu[KV_SLOTS];
    uint32_t           c_done;       /* the change C holds */
    int64_t            c_us;
    kv_merge_cb        merge;
};
typedef struct kv_ns kv_ns_t;

enum { KV_CARD_NONE = 0, KV_CARD_IN_USE = 1, KV_CARD_DECLINED = 2 };

typedef struct {
    kv_hooks_t  hooks;
    kv_where_t  where;
    char        why[64];
    bool        away;                /* the card that holds the settings is away: identities in RAM only */
    bool        card_ok;             /* the card takes the settings' writes */
    bool        mounted;             /* kvstore holds a mount reference */
    bool        can_prepare;
    uint32_t    cid;
    uint8_t     mac[6];
    uint8_t     gen[KV_SLOTS];
    uint8_t     damaged;             /* folders found damaged, by slot */
    uint32_t    kvs_state, kvs_cid;  /* kvs/card */
    uint16_t    errors;
    int         streak;              /* namespaces in a row whose A and B both failed */
    uint32_t    last_ms;
    bool        nvs_full;
    char        version[12];
} kv_g_t;

extern kv_g_t   kvg;
extern kv_ns_t *kv_all;
extern int      kv_n;
extern uint8_t *kv_img;              /* 16 kB, PSRAM: the image being written, or read at boot */
extern uint8_t *kv_chk;              /* 16 kB, PSRAM: the read-back */

static inline void kv_lock(kv_ns_t *n) { xSemaphoreTakeRecursive(n->mx, portMAX_DELAY); }
static inline void kv_unlock(kv_ns_t *n) { xSemaphoreGiveRecursive(n->mx); }
static inline int kv_slots(const kv_ns_t *n) { return n->def->flags & KVF_CACHE ? 2 : 3; }
/* Identities kept in RAM only: the card that holds them is away. */
static inline bool kv_ident_ram(const kv_ns_t *n) { return n->held || (!n->card && kvg.away); }

/* kv.c */
size_t kv_snapshot(kv_ns_t *n, uint8_t *out, bool with_base, uint32_t *gen);
void   kv_load_recs(kv_ns_t *n, const uint8_t *r, size_t len);
void   kv_set_where(kv_where_t w, const char *why);
void   kv_card_lost(const char *why);
void   kv_restart(void);

/* kv_card.c -- FatFs on the card, directly; never from more than one task at once. */
enum { KV_STAGE_IDLE = 0, KV_STAGE_CARD = 1, KV_STAGE_WRITE = 2 };
bool        kvc_rtc_skip(void);              /* the last start stopped in a card operation */
void        kvc_stage(int stage);
bool        kvc_scan(uint8_t gen[KV_SLOTS]); /* VFO-CFG there; each slot's newest folder */
kv_cstate_t kvc_read(kv_ns_t *n, int slot, kv_hdr_t *h, bool *parsed);   /* into kv_img */
/* kv_img's first n bytes (whole sectors) to copy `slot`, read back. ESP_ERR_INVALID_STATE: the
 * folder is damaged (kvc_next_gen) */
esp_err_t   kvc_write(kv_ns_t *n, int slot, size_t bytes);
bool        kvc_next_gen(int slot);

/* kv_nvs.c */
esp_err_t kvn_load(const kv_ns_def_t *d, kv_buf_t *e);          /* main task: the namespace's NVS entries */
/* On a helper task with an internal stack: every key of NVS's copy that recs
 * lacks erased (never an nvs_keep key, nor an identity when skip_ident), and,
 * with set, every key of recs set there. */
esp_err_t kvn_write(kv_ns_t *n, const uint8_t *recs, size_t len, bool set, bool skip_ident);
void      kvn_card_get(uint32_t *state, uint32_t *cid);
esp_err_t kvn_card_set(uint32_t state, uint32_t cid, bool helper);
void      kvn_stats(uint32_t *used, uint32_t *freec);

#if VFO_KV_TEST
/* kv_test.c: a made-up fault for this start, kept in RTC memory across the
 * restart that asked for it. */
typedef struct {
    bool     nocard;                 /* the card taken as not answering */
    bool     foreign;                /* its copies taken as another knob's */
    uint32_t ioerr;                  /* bit i * 3 + slot: that copy's read fails */
    bool     crashload;              /* a crash inside this start's reading of the card */
} kv_fake_t;
extern kv_fake_t kv_fake;
extern volatile bool kv_tear;        /* the next copy written: half of it, then a crash */
void kv_test_boot(void);
#endif

/* kv_writer.c */
esp_err_t kvw_start(void);
void      kvw_kick(void);
void      kvw_lend(void);                    /* a waiter's priority, up to 4, while it waits */
enum { KV_DO_NONE, KV_DO_FORMATTED, KV_DO_PREPARE, KV_DO_USE, KV_DO_FRESH, KV_DO_FORGET, KV_DO_AGAIN };
esp_err_t kvw_action(int what, uint32_t ms);
