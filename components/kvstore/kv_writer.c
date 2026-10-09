/* kvwr: the one task that writes the settings -- to the card, or to NVS when
 * the card does not hold them -- and carries out the owner's actions. Core 0,
 * priority 2 (up to 4 while someone waits on it), its stack in PSRAM. */
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "kv_priv.h"
#include "sd_cache.h"

static const char *TAG = "kv";

#define QUIET_US   (5 * 1000000LL)                 /* a change never committed goes after 5 s of quiet */
#define C_AFTER_US (10 * 60 * 1000000LL)           /* C: ten minutes after the last change */
#define C_EVERY_US (60 * 60 * 1000000LL)           /* ...and at least hourly while it keeps changing */
#define SOUND_US   (10 * 60 * 1000000LL)           /* an ordinary NVS commit waits for quiet this long at most */
#define RETRY_US   (60 * 1000000LL)

static TaskHandle_t      s_task;
static StaticSemaphore_t s_amx_buf;
static SemaphoreHandle_t s_amx;
static volatile int       s_act;
static volatile esp_err_t s_res;
static volatile bool      s_did;

void kvw_kick(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void kvw_lend(void)
{
    if (!s_task) return;
    UBaseType_t p = uxTaskPriorityGet(NULL);
    if (p > 4) p = 4;
    if (p > uxTaskPriorityGet(s_task)) vTaskPrioritySet(s_task, p);
    kvw_kick();
}

static bool busy(void) { return kvg.hooks.busy && kvg.hooks.busy(); }

/* kv_img's records (at KV_HDR, len bytes) as copy `slot`, with seq. */
static bool write_slot(kv_ns_t *n, int slot, uint32_t seq, size_t len)
{
    kv_hdr_t h = { .seq = seq, .len = (uint32_t)len, .crc = kv_crc32(0, kv_img + KV_HDR, len), .format = 1,
                   .slot = (uint8_t)('A' + slot), .gen = kvg.gen[slot] };
    memcpy(h.mac, kvg.mac, 6);
    strlcpy(h.ns, n->def->name, sizeof h.ns);
    strlcpy(h.version, kvg.version, sizeof h.version);
    kv_hdr_put(kv_img, &h);
    const size_t bytes = (KV_HDR + len + 511u) & ~511u;
    memset(kv_img + KV_HDR + len, 0, bytes - KV_HDR - len);
    kvc_stage(KV_STAGE_WRITE);
    esp_err_t e = kvc_write(n, slot, bytes);
    if (e == ESP_ERR_INVALID_STATE && kvc_next_gen(slot)) e = kvc_write(n, slot, bytes);
    kvc_stage(KV_STAGE_IDLE);
    if (e == ESP_OK) {
        n->cp[slot] = (kv_copy_t){ KV_VALID, true, seq };
        n->repair &= ~(1u << slot);
        kvg.last_ms = (uint32_t)(esp_timer_get_time() / 1000);
        return true;
    }
    n->cp[slot].st = KV_CORRUPT;
    n->repair |= 1u << slot;
    kvg.errors++;
    if (e == ESP_ERR_NO_MEM) kv_set_where(KV_CARD_TROUBLE, "the card is full");
    return false;
}

static void card_commit(kv_ns_t *n)
{
    const int64_t t0 = esp_timer_get_time();
    uint32_t g;
    const size_t len = kv_snapshot(n, kv_img + KV_HDR, true, &g);
    const uint32_t seq = n->seq_hi + 1;
    const int first = kv_first(&n->cp[0], &n->cp[1]);
    int ok = 0;
    for (int i = 0; i < 2; i++) ok += write_slot(n, i ? 1 - first : first, seq, len);
    if (ok) {
        n->seq = n->seq_hi = seq;
        n->done = g;
        n->failed = 0;
        n->force = false;
        if (n->gen == g) n->asked = n->urgent = false;
        kvg.streak = 0;
        ESP_LOGI(TAG, "%s: written to the card, %u B, seq %u, %s (%u ms)", n->def->name, (unsigned)len, (unsigned)seq,
                 ok == 2 ? "A and B" : "one copy of two", (unsigned)((esp_timer_get_time() - t0) / 1000));
        return;
    }
    n->failed = g;
    n->retry_us = esp_timer_get_time() + RETRY_US / 4;
    ESP_LOGE(TAG, "%s: neither A nor B written", n->def->name);
    if (++kvg.streak >= 3) kv_card_lost("the card stopped taking writes");
}

/* A secret erased: off the frozen NVS copy first, then off the base. */
static bool scrub_nvs(kv_ns_t *n)
{
    if (!(n->def->flags & KVF_FROZEN)) {
        n->scrub = false;
        return true;
    }
    if (busy()) return false;
    uint32_t g;
    const size_t len = kv_snapshot(n, kv_img, false, &g);
    const esp_err_t e = kvn_write(n, kv_img, len, false, false);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "%s: not erased from NVS: %s", n->def->name, esp_err_to_name(e));
        n->retry_us = esp_timer_get_time() + RETRY_US;
        return false;
    }
    kv_lock(n);
    size_t off = 0;
    kv_rec_t x;
    while (kv_rec_next(n->base.p, n->base.len, &off, &x)) {
        char k[KV_KEY_MAX + 1];
        memcpy(k, x.key, x.klen);
        k[x.klen] = 0;
        if (!kv_buf_find(&n->val, k, NULL, NULL)) {
            kv_buf_del(&n->base, k);
            off -= 4u + x.klen + x.vlen;
        }
    }
    n->gen++;                                    /* A, B and C written again */
    kv_unlock(n);
    n->scrub = false;
    n->asked = n->urgent = true;
    if (kv_slots(n) > 2) n->repair |= 4;
    return true;
}

static void nvs_pass(kv_ns_t *n, int64_t now)
{
    if (busy()) return;
    if (!n->urgent && !n->force && !n->scrub && kvg.hooks.sound && kvg.hooks.sound() && now - n->dirty_us < SOUND_US)
        return;
    if (now < n->retry_us) return;
    uint32_t g;
    const bool sc = n->scrub;
    const size_t len = kv_snapshot(n, kv_img, false, &g);
    const esp_err_t e = kvn_write(n, kv_img, len, true, kv_ident_ram(n));
    if (e == ESP_OK) {
        n->done = g;
        n->failed = 0;
        n->force = false;
        if (sc) n->scrub = false;
        if (n->gen == g) n->asked = n->urgent = false;
        kvg.nvs_full = false;
        return;
    }
    n->failed = g;
    n->retry_us = now + RETRY_US;
    if (e == ESP_ERR_NVS_NOT_ENOUGH_SPACE) kvg.nvs_full = true;
    ESP_LOGE(TAG, "%s: not saved in NVS: %s%s", n->def->name, esp_err_to_name(e),
             e == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? " -- the knob's memory is full: fit an SD card" : "");
}

static void pass(kv_ns_t *n)
{
    if (n->ro) return;
    const int64_t now = esp_timer_get_time();
    const bool dirty = n->gen != n->done || n->force;
    const bool due = n->asked || n->urgent || n->force || n->scrub || now - n->changed_us >= QUIET_US;
    if (n->card && kvg.card_ok) {
        if (n->scrub && !scrub_nvs(n)) return;
        if (n->gen != n->done || n->force) {     /* again: a scrub makes a change */
            if (!(n->asked || n->urgent || n->force || now - n->changed_us >= QUIET_US) || now < n->retry_us) return;
            card_commit(n);
            if (!n->card || !kvg.card_ok || n->gen != n->done) return;
        }
        if (now < n->retry_us) return;
        /* clean: the copies behind it, and C */
        const uint8_t ab = n->repair & 3;
        const bool c = kv_slots(n) > 2 &&
                       ((n->repair & 4) || (n->c_done != n->done && (now - n->changed_us >= C_AFTER_US ||
                                                                       now - n->c_us >= C_EVERY_US)));
        if (!ab && !c) return;
        uint32_t g;
        const size_t len = kv_snapshot(n, kv_img + KV_HDR, true, &g);
        if (g != n->done) return;
        for (int s = 0; s < 2; s++)
            if (ab & (1u << s) && write_slot(n, s, n->seq, len))
                ESP_LOGI(TAG, "%s: copy %c written again", n->def->name, 'A' + s);
        if (c && write_slot(n, 2, n->seq, len)) {
            n->c_done = n->done;
            n->c_us = now;
        }
        if (n->repair) n->retry_us = now + RETRY_US;
        return;
    }
    if (n->def->flags & KVF_CACHE) {             /* RAM only without the card */
        n->done = n->gen;
        n->asked = n->urgent = n->force = n->scrub = false;
        return;
    }
    if ((dirty || n->scrub) && due) nvs_pass(n, now);
}

/* ---- the owner's actions ---- */

static void take_card(const char *why)
{
    kvg.card_ok = true;
    kvg.away = false;
    kvg.can_prepare = false;
    kv_set_where(KV_ON_CARD, why);
    if (kvn_card_set(KV_CARD_IN_USE, kvg.cid, true) == ESP_OK) {
        kvg.kvs_state = KV_CARD_IN_USE;
        kvg.kvs_cid = kvg.cid;
    }
}

/* Every namespace onto the card, from what RAM holds, its base the NVS
 * copy as this boot found it. */
static void onto_card(kv_ns_t *n)
{
    kv_lock(n);
    if (kv_buf_copy(&n->base, n->ebase.p, n->ebase.len) == 0 && n->val.len + n->base.len <= KV_ROOM) {
        n->card = true;
        n->held = n->ro = false;
        n->gen++;
        n->changed_us = esp_timer_get_time();
        n->asked = n->urgent = true;
        if (kv_slots(n) > 2) n->repair |= 4;
    } else ESP_LOGE(TAG, "%s: too large for a copy with its base: kept in NVS", n->def->name);
    kv_unlock(n);
}

/* The copies' numbers as the card has them now, before anything is written
 * over them: a copy left behind must never outnumber what goes on. */
static void learn_seq(kv_ns_t *n)
{
    for (int s = 0; s < kv_slots(n); s++) {
        kv_hdr_t h;
        bool parsed;
        const kv_cstate_t st = kvc_read(n, s, &h, &parsed);
        n->cp[s] = (kv_copy_t){ st, parsed, parsed ? h.seq : 0 };
    }
    const kv_choice_t ch = kv_choose(n->cp, kv_slots(n));
    if (ch.next_seq - 1 > n->seq_hi) n->seq_hi = ch.next_seq - 1;
}

static esp_err_t take_new(void)
{
    if (!kvg.mounted) {
        if (!sdc_mount()) return ESP_ERR_NOT_FOUND;
        kvg.mounted = true;
    }
    kvg.cid = sdc_cid();
    memset(kvg.gen, 0, sizeof kvg.gen);
    kvc_scan(kvg.gen);
    for (int i = 0; i < kv_n; i++) {
        kv_ns_t *n = &kv_all[i];
        n->seq = n->seq_hi = 0;
        n->repair = 0;
        memset(n->clu, 0, sizeof n->clu);
        for (int s = 0; s < KV_SLOTS; s++) n->cp[s] = (kv_copy_t){ KV_ABSENT, false, 0 };
        onto_card(n);
    }
    take_card("");
    ESP_LOGI(TAG, "the card is taken: the settings go onto it");
    return ESP_OK;
}

static esp_err_t use_its(void)
{
    for (int i = 0; i < kv_n; i++) {
        kv_ns_t *n = &kv_all[i];
        bool loaded = false;
        uint32_t lseq = 0;
        for (int s = 0; s < kv_slots(n); s++) {
            kv_hdr_t h;
            bool parsed;
            kv_copy_t c = { kvc_read(n, s, &h, &parsed), false, 0 };
            c.parsed = parsed;
            c.seq = parsed ? h.seq : 0;
            n->cp[s] = c;
            if (c.st == KV_VALID && h.format == 1 && (!loaded || h.seq > lseq)) {
                loaded = true;
                lseq = h.seq;
                kv_load_recs(n, kv_img + KV_HDR, h.len);
            }
        }
        const kv_choice_t ch = kv_choose(n->cp, kv_slots(n));
        n->seq_hi = ch.next_seq - 1;
        n->seq = lseq;
        onto_card(n);                            /* every copy rewritten, with this knob's MAC */
    }
    take_card("the card's settings, taken over from another knob");
    return ESP_OK;
}

static esp_err_t act(int what)
{
    switch (what) {
    case KV_DO_FORMATTED:
        return take_new();
    case KV_DO_PREPARE: {
        if (kvg.mounted) {
            sdc_unmount();
            kvg.mounted = false;
        }
        kvg.card_ok = false;
        esp_err_t e;
        if (sdc_mount()) {
            sdc_unmount();
            e = sdc_format();
        } else {
            e = sdc_prepare();
            if (e == ESP_OK) kvg.mounted = true;
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "the card was not prepared: %s", esp_err_to_name(e));
            return e;
        }
        return take_new();
    }
    case KV_DO_USE:
        if (!kvg.mounted || kvg.where != KV_CARD_FOREIGN) return ESP_ERR_INVALID_STATE;
        return use_its();
    case KV_DO_AGAIN:
        /* The card carried on without, taken again: in use once more, and the
         * restart that follows merges what changed in NVS meanwhile onto what
         * the card holds -- the card's settings are newer than NVS's frozen
         * copy, so nothing of NVS's is written over them whole. */
        if (kvg.where != KV_IN_NVS || kvg.kvs_state != KV_CARD_DECLINED) return ESP_ERR_INVALID_STATE;
        if (kvn_card_set(KV_CARD_IN_USE, kvg.kvs_cid, true) != ESP_OK) return ESP_FAIL;
        kvg.kvs_state = KV_CARD_IN_USE;
        ESP_LOGI(TAG, "the SD card is to be used again: from the next start");
        return ESP_OK;
    case KV_DO_FRESH: {
        if (!kvg.mounted || (kvg.where != KV_CARD_FOREIGN && kvg.where != KV_CARD_TROUBLE))
            return ESP_ERR_INVALID_STATE;
        memset(kvg.gen, 0, sizeof kvg.gen);
        kvc_scan(kvg.gen);
        for (int i = 0; i < kv_n; i++) {
            learn_seq(&kv_all[i]);
            onto_card(&kv_all[i]);
        }
        take_card("");
        ESP_LOGI(TAG, "the knob's settings go onto this card, over what it held");
        return ESP_OK;
    }
    case KV_DO_FORGET:
        kvn_card_set(KV_CARD_DECLINED, kvg.cid ? kvg.cid : kvg.kvs_cid, true);
        kvg.kvs_state = KV_CARD_DECLINED;
        for (int i = 0; i < kv_n; i++) {
            kv_ns_t *n = &kv_all[i];
            if (n->card) n->force = true;
            n->card = n->held = false;
        }
        kvg.card_ok = kvg.away = kvg.can_prepare = false;
        kv_set_where(KV_IN_NVS, "you chose to carry on without the SD card");
        if (kvg.mounted) {
            sdc_unmount();
            kvg.mounted = false;
        }
        ESP_LOGW(TAG, "carrying on without the SD card: the settings stay in NVS");
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t kvw_action(int what, uint32_t ms)
{
    if (!s_task) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_amx, portMAX_DELAY);
    s_did = false;
    s_act = what;
    kvw_lend();
    esp_err_t e = ESP_ERR_TIMEOUT;
    for (uint32_t t = 0; t <= ms; t += 20) {
        if (s_did) {
            e = s_res;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    xSemaphoreGive(s_amx);
    return e;
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        vTaskDelay(pdMS_TO_TICKS(20));                /* a page's several commits: one write */
        if (s_act) {
            const int a = s_act;
            s_res = act(a);
            s_act = KV_DO_NONE;
            s_did = true;
        }
        if (kvg.card_ok && kvg.damaged) {
            for (int s = 0; s < KV_SLOTS; s++)
                if (kvg.damaged & (1u << s)) kvc_next_gen(s);
            kvg.damaged = 0;
        }
        for (int i = 0; i < kv_n; i++) pass(&kv_all[i]);
        vTaskPrioritySet(NULL, 2);
    }
}

esp_err_t kvw_start(void)
{
    s_amx = xSemaphoreCreateMutexStatic(&s_amx_buf);
    if (xTaskCreatePinnedToCoreWithCaps(task, "kvwr", 6144, NULL, 2, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    kvw_kick();
    return ESP_OK;
}
