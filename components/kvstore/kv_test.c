/* The settings' bench tests (SETTINGS-ON-CARD-PLAN.md §18.2), in a build
 * made with -D VFO_KV_TEST=1 only: a writer that never stops, for power cuts
 * -- a count and 4 kB that repeats it, in one namespace of its own, each
 * written logged once it is on the card -- and made-up card faults for the
 * next start. Never in a release. */
#include "kv_priv.h"

#if VFO_KV_TEST

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "ff.h"
#include "sd_cache.h"

static const char *TAG = "kvtest";

#define NS     "kvtest"
#define WORDS  1000                      /* the count, 1000 times: 4 kB, nine sectors with the header */
#define MAGIC  0x4B565446u

RTC_NOINIT_ATTR static struct {
    uint32_t magic;
    uint8_t  nocard, foreign, crashload;
    uint32_t ioerr;
} s_rtc;

kv_fake_t     kv_fake;
volatile bool kv_tear;

static volatile bool s_run;
static volatile uint32_t s_pace_ms;          /* 0: nonstop; else one save, then this long idle */
static uint32_t      s_count;
static esp_timer_handle_t s_panic;

void kv_test_boot(void)
{
    if (s_rtc.magic == MAGIC) {
        kv_fake = (kv_fake_t){ .nocard = s_rtc.nocard, .foreign = s_rtc.foreign, .ioerr = s_rtc.ioerr,
                               .crashload = s_rtc.crashload };
        if (kv_fake.nocard || kv_fake.foreign || kv_fake.ioerr || kv_fake.crashload)
            ESP_LOGW(TAG, "this start's made-up fault:%s%s%s%s", kv_fake.nocard ? " no card" : "",
                     kv_fake.foreign ? " another knob's card" : "", kv_fake.ioerr ? " unreadable copies" : "",
                     kv_fake.crashload ? " a crash while reading" : "");
    }
    memset(&s_rtc, 0, sizeof s_rtc);              /* for this start only */
}

static void hammer(void *arg)
{
    (void)arg;
    kv_handle_t h;
    if (kv_open(NS, &h) != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    uint32_t *w = heap_caps_malloc(WORDS * 4, MALLOC_CAP_SPIRAM);
    vTaskDelay(pdMS_TO_TICKS(3000));
    /* What this start found: the count, and the 4 kB beside it -- every word
     * the count, or the save was torn and loaded anyway. */
    uint32_t c = 0;
    size_t n = WORDS * 4;
    const bool have = kv_get_u32(h, "count", &c) == ESP_OK;
    bool whole = !have;
    if (have && w && kv_get_blob(h, "blob", w, &n) == ESP_OK && n == WORDS * 4) {
        whole = true;
        for (int i = 0; i < WORDS; i++)
            if (w[i] != c) whole = false;
    }
    s_count = c;
    uint8_t aut = 0;
    kv_get_u8(h, "auto", &aut);
    ESP_LOGW(TAG, "KVTEST START count %u %s, settings %s%s", (unsigned)c, whole ? "WHOLE" : "TORN",
             kv_where_word(kv_where()), aut ? ", writing on" : "");
    if (aut) s_run = true;
    uint32_t pace = 0;
    if (kv_get_u32(h, "pace", &pace) == ESP_OK) s_pace_ms = pace;
    bool said = false;
    for (;;) {
        if (!s_run || !w) {
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        if (!kv_on_card(h)) {                     /* never NVS's wear */
            if (!said) ESP_LOGW(TAG, "not on the card: not writing");
            said = true;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        said = false;
        c++;
        for (int i = 0; i < WORDS; i++) w[i] = c;
        kv_edit_begin(h);
        kv_set_u32(h, "count", c);
        kv_set_blob(h, "blob", w, WORDS * 4);
        kv_set_u8(h, "auto", 1);
        kv_edit_end(h);
        const esp_err_t e = kv_commit_wait(h, 5000);
        if (e == ESP_OK) ESP_LOGI(TAG, "KVTEST DURABLE %u", (unsigned)c);
        else ESP_LOGW(TAG, "KVTEST %u: %s", (unsigned)c, esp_err_to_name(e));
        s_count = c;
        if (s_pace_ms) vTaskDelay(pdMS_TO_TICKS(s_pace_ms));
    }
}

void kv_test_start(void)
{
    xTaskCreatePinnedToCoreWithCaps(hammer, "kvtest", 4096, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static const char *cword(kv_cstate_t s)
{
    return s == KV_VALID ? "ok" : s == KV_CORRUPT ? "bad" : s == KV_UNREADABLE ? "unreadable" : "none";
}

size_t kv_test_json(char *o, size_t cap)
{
    kv_status_t st;
    kv_status(&st);
    size_t k = (size_t)snprintf(o, cap,
                                "{\"where\":\"%s\",\"why\":\"%s\",\"gen\":[%u,%u,%u],\"count\":%u,\"writing\":%s,"
                                "\"nvs_used\":%u,\"nvs_free\":%u,\"errors\":%u,\"ns\":[",
                                kv_where_word(st.where), st.why, st.gen[0], st.gen[1], st.gen[2], (unsigned)s_count,
                                s_run ? "true" : "false", (unsigned)st.nvs_used, (unsigned)st.nvs_free,
                                (unsigned)st.errors);
    for (int i = 0; i < kv_n && k < cap; i++) {
        const kv_ns_t *n = &kv_all[i];
        int keys = 0;
        size_t off = 0;
        kv_rec_t x;
        while (kv_rec_next(n->val.p, n->val.len, &off, &x)) keys++;
        k += (size_t)snprintf(o + k, cap - k,
                              "%s{\"name\":\"%s\",\"card\":%s,\"held\":%s,\"ro\":%s,\"keys\":%d,\"bytes\":%u,"
                              "\"base\":%u,\"seq\":%u,\"seq_hi\":%u,\"repair\":%u,\"pending\":%s,"
                              "\"copies\":[[\"%s\",%u],[\"%s\",%u],[\"%s\",%u]]}",
                              i ? "," : "", n->def->name, n->card ? "true" : "false", n->held ? "true" : "false",
                              n->ro ? "true" : "false", keys, (unsigned)n->val.len, (unsigned)n->base.len,
                              (unsigned)n->seq, (unsigned)n->seq_hi, n->repair, n->gen != n->done ? "true" : "false",
                              cword(n->cp[0].st), (unsigned)n->cp[0].seq, cword(n->cp[1].st),
                              (unsigned)n->cp[1].seq, cword(n->cp[2].st), (unsigned)n->cp[2].seq);
    }
    if (k < cap) k += (size_t)snprintf(o + k, cap - k, "]}");
    return k < cap ? k : cap - 1;
}

static kv_ns_t *ns_named(const char *name)
{
    for (int i = 0; i < kv_n; i++)
        if (name && !strcmp(kv_all[i].def->name, name)) return &kv_all[i];
    return NULL;
}

static void panic_cb(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "KVTEST PANIC now");
    abort();
}

/* One byte of a copy's records turned over, the namespace written and quiet:
 * its CRC fails at the next start. Closing the file rewrites its folder's
 * entry, as a PC's write would. */
static esp_err_t corrupt(kv_ns_t *n, int slot, char *say, size_t cap)
{
    if (!n->card || n->gen != n->done) {
        snprintf(say, cap, "%s: not on the card, or not yet written", n->def->name);
        return ESP_ERR_INVALID_STATE;
    }
    char up[KV_NS_MAX + 1];
    size_t i = 0;
    for (; n->def->name[i] && i < KV_NS_MAX; i++) up[i] = (char)toupper((unsigned char)n->def->name[i]);
    up[i] = 0;
    char p[48];
    snprintf(p, sizeof p, "%s/VFO-CFG/%c%u/%s.KV", sdc_drive() ? sdc_drive() : "0:", 'A' + slot, kvg.gen[slot], up);
    static EXT_RAM_BSS_ATTR FIL f;
    static EXT_RAM_BSS_ATTR uint8_t hdr[KV_HDR];
    UINT br = 0, bw = 0;
    if (f_open(&f, p, FA_READ | FA_WRITE) != FR_OK) {
        snprintf(say, cap, "%s: not opened", p);
        return ESP_FAIL;
    }
    kv_hdr_t h;
    FRESULT r = f_read(&f, hdr, KV_HDR, &br);
    const uint32_t at = r == FR_OK && br == KV_HDR && kv_hdr_get(hdr, &h) && h.len > 8 ? KV_HDR + h.len / 2 : 9;
    uint8_t b = 0;
    if (r == FR_OK) r = f_lseek(&f, at);
    if (r == FR_OK) r = f_read(&f, &b, 1, &br);
    b ^= 0x5A;
    if (r == FR_OK) r = f_lseek(&f, at);
    if (r == FR_OK) r = f_write(&f, &b, 1, &bw);
    const FRESULT c = f_close(&f);
    snprintf(say, cap, "%s: byte %u turned over (%d, %d) -- restart to see it", p, (unsigned)at, r, c);
    return r == FR_OK && c == FR_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t kv_test_do(const char *what, const char *ns, const char *slots, char *say, size_t cap)
{
    kv_ns_t *n = ns_named(ns);
    uint32_t bits = 0;
    int slot = -1;
    for (const char *s = slots ? slots : ""; *s; s++)
        if (*s >= 'A' && *s <= 'C') {
            if (slot < 0) slot = *s - 'A';
            if (n) bits |= 1u << ((n - kv_all) * 3 + (*s - 'A'));
        }
    if (!strcmp(what, "hammer")) {
        s_run = true;
        snprintf(say, cap, "writing from %u on, and again after every start, until stop", (unsigned)s_count);
    } else if (!strcmp(what, "stop")) {
        s_run = false;
        kv_handle_t h;
        if (kv_open(NS, &h) == ESP_OK) {
            kv_set_u8(h, "auto", 0);
            kv_commit_wait(h, 3000);
        }
        snprintf(say, cap, "stopped at %u", (unsigned)s_count);
    } else if (!strcmp(what, "panic")) {
        const esp_timer_create_args_t a = { .callback = panic_cb, .name = "kvpanic" };
        if (!s_panic) esp_timer_create(&a, &s_panic);
        const uint32_t us = 300000 + esp_random() % 1200000;
        esp_timer_start_once(s_panic, us);
        snprintf(say, cap, "a panic in %u ms", (unsigned)(us / 1000));
    } else if (!strcmp(what, "pace")) {
        /* ns= the idle after each save, ms; 0 nonstop. Kept for the next starts. */
        s_pace_ms = ns ? (uint32_t)strtoul(ns, NULL, 10) : 0;
        kv_handle_t h;
        if (kv_open(NS, &h) == ESP_OK) {
            kv_set_u32(h, "pace", s_pace_ms);
            kv_commit_wait(h, 3000);
        }
        snprintf(say, cap, "one save, then %u ms idle", (unsigned)s_pace_ms);
    } else if (!strcmp(what, "tear")) {
        kv_tear = true;
        snprintf(say, cap, "the next copy written: torn halfway, then a crash");
    } else if (!strcmp(what, "nocard") || !strcmp(what, "foreign") || !strcmp(what, "ioerr") ||
               !strcmp(what, "crashload")) {
        if (!strcmp(what, "ioerr") && !bits) {
            snprintf(say, cap, "ioerr: ns= and slots= (ABC)");
            return ESP_ERR_INVALID_ARG;
        }
        if (s_rtc.magic != MAGIC) memset(&s_rtc, 0, sizeof s_rtc);
        s_rtc.magic = MAGIC;
        if (!strcmp(what, "nocard")) s_rtc.nocard = 1;
        else if (!strcmp(what, "foreign")) s_rtc.foreign = 1;
        else if (!strcmp(what, "crashload")) s_rtc.crashload = 1;
        else s_rtc.ioerr |= bits;
        snprintf(say, cap, "%s at the next start (a restart, not a power cut)", what);
    } else if (!strcmp(what, "corrupt")) {
        if (!n || slot < 0) {
            snprintf(say, cap, "corrupt: ns= and slots= (one of A, B, C)");
            return ESP_ERR_INVALID_ARG;
        }
        return corrupt(n, slot, say, cap);
    } else if (!strcmp(what, "rewrite")) {
        if (!n) {
            snprintf(say, cap, "rewrite: ns=");
            return ESP_ERR_INVALID_ARG;
        }
        kv_lock(n);
        n->gen++;
        kv_unlock(n);
        kv_commit_now(n);
        snprintf(say, cap, "%s written again", n->def->name);
    } else {
        snprintf(say, cap, "do=hammer|stop|panic|tear|crashload|nocard|foreign|ioerr|corrupt|rewrite");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGW(TAG, "%s", say);
    return ESP_OK;
}

#endif
