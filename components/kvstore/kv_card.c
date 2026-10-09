/* The settings' copies on the SD card, through FatFs directly
 * (SETTINGS-ON-CARD-PLAN.md §3-§5): files of one cluster, made once and then
 * written in place -- no FAT sector, no directory sector, no allocation in a
 * save -- and read back. Called by kv_init, then by kvwr alone. */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ff.h"
#include "kv_priv.h"
#include "sd_cache.h"

static const char *TAG = "kv";

#define ROOT "VFO-CFG"

static EXT_RAM_BSS_ATTR FIL s_fil;

/* What the last start was doing on the card, kept across a crash. */
RTC_NOINIT_ATTR static uint32_t s_rtc_magic, s_rtc_stage;
#define RTC_MAGIC 0x4B565354u

bool kvc_rtc_skip(void)
{
    const esp_reset_reason_t r = esp_reset_reason();
    const bool crash = r == ESP_RST_PANIC || r == ESP_RST_TASK_WDT || r == ESP_RST_INT_WDT || r == ESP_RST_WDT;
    const bool skip = crash && s_rtc_magic == RTC_MAGIC && s_rtc_stage != KV_STAGE_IDLE;
    s_rtc_magic = RTC_MAGIC;
    s_rtc_stage = KV_STAGE_IDLE;
    return skip;
}

void kvc_stage(int stage)
{
    s_rtc_magic = RTC_MAGIC;
    s_rtc_stage = stage;
}

static const char *drive(void)
{
    const char *d = sdc_drive();
    return d ? d : "0:";
}

static void folder(char *out, size_t n, int slot, unsigned gen)
{
    snprintf(out, n, "%s/" ROOT "/%c%u", drive(), 'A' + slot, gen);
}

static void file(char *out, size_t n, const kv_ns_t *ns, int slot)
{
    char up[KV_NS_MAX + 1];
    size_t i = 0;
    for (; ns->def->name[i] && i < KV_NS_MAX; i++) up[i] = (char)toupper((unsigned char)ns->def->name[i]);
    up[i] = 0;
    snprintf(out, n, "%s/" ROOT "/%c%u/%s.KV", drive(), 'A' + slot, kvg.gen[slot], up);
}

bool kvc_scan(uint8_t gen[KV_SLOTS])
{
    char p[24];
    snprintf(p, sizeof p, "%s/" ROOT, drive());
    FF_DIR d;
    if (f_opendir(&d, p) != FR_OK) return false;
    FILINFO fi;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (!(fi.fattrib & AM_DIR) || strlen(fi.fname) != 2) continue;
        const int s = fi.fname[0] - 'A', g = fi.fname[1] - '0';
        if (s >= 0 && s < KV_SLOTS && g >= 0 && g <= 9 && g > gen[s]) gen[s] = (uint8_t)g;
    }
    f_closedir(&d);
    return true;
}

static void mkdirs(int slot)
{
    char p[24];
    snprintf(p, sizeof p, "%s/" ROOT, drive());
    f_mkdir(p);
    folder(p, sizeof p, slot, kvg.gen[slot]);
    f_mkdir(p);
}

kv_cstate_t kvc_read(kv_ns_t *n, int slot, kv_hdr_t *h, bool *parsed)
{
    char p[40];
    file(p, sizeof p, n, slot);
    *parsed = false;
    FRESULT r = f_open(&s_fil, p, FA_READ);
    if (r == FR_NO_FILE || r == FR_NO_PATH) return KV_ABSENT;
    if (r != FR_OK) {
        ESP_LOGW(TAG, "%s: not opened (%d)", p, r);
        if (r == FR_INT_ERR) {
            kvg.damaged |= 1u << slot;
            return KV_CORRUPT;
        }
        return KV_UNREADABLE;
    }
    n->clu[slot] = s_fil.obj.sclust;
    const FSIZE_t size = f_size(&s_fil);
    if (size != KV_FILE) {
        const bool stub = !size && !s_fil.obj.sclust;    /* a cut while it was being made */
        f_close(&s_fil);
        if (!stub) ESP_LOGW(TAG, "%s: %u B, not a copy's size", p, (unsigned)size);
        return stub ? KV_ABSENT : KV_CORRUPT;
    }
    UINT br = 0;
    r = f_read(&s_fil, kv_img, 512, &br);
    size_t want = 512;
    kv_hdr_t hh;
    if (r == FR_OK && br == 512 && kv_hdr_get(kv_img, &hh) && hh.len <= KV_ROOM)
        want = (KV_HDR + hh.len + 511u) & ~511u;
    if (r == FR_OK && br == 512 && want > 512) {
        r = f_read(&s_fil, kv_img + 512, want - 512, &br);
        if (br != want - 512 && r == FR_OK) r = FR_DISK_ERR;
    } else if (r == FR_OK && br != 512) r = FR_DISK_ERR;
    f_close(&s_fil);                             /* opened to read: closing writes nothing */
    if (r != FR_OK) {
        ESP_LOGW(TAG, "%s: not read (%d)", p, r);
        return KV_UNREADABLE;
    }
    const kv_cstate_t st = kv_img_check(kv_img, want, KV_FILE, n->def->name, h, parsed);
    if (st != KV_VALID) ESP_LOGW(TAG, "%s: a bad copy", p);
    return st;
}

/* A new file of one copy's size, contiguous: the folder's sector and a FAT
 * sector written once. */
static FRESULT create(const char *p)
{
    FRESULT r = f_open(&s_fil, p, FA_WRITE | FA_CREATE_NEW);
    if (r != FR_OK) return r;
    r = f_expand(&s_fil, KV_FILE, 1);
    const FRESULT c = f_close(&s_fil);
    if (r == FR_OK) r = c;
    if (r != FR_OK) {
        ESP_LOGW(TAG, "%s: not made (%d)", p, r);
        f_unlink(p);                             /* owns nothing yet, or what f_expand gave back */
    } else ESP_LOGI(TAG, "%s: made", p);
    return r;
}

/* The entry, checked before anything is written through it (§4.2). */
static esp_err_t open_checked(kv_ns_t *n, int slot, const char *p)
{
    FRESULT r = f_open(&s_fil, p, FA_READ | FA_WRITE);
    if (r == FR_NO_PATH) {
        mkdirs(slot);
        r = f_open(&s_fil, p, FA_READ | FA_WRITE);
    }
    if (r == FR_NO_FILE) {
        if ((r = create(p)) != FR_OK) return r == FR_DENIED ? ESP_ERR_NO_MEM : ESP_FAIL;
        r = f_open(&s_fil, p, FA_READ | FA_WRITE);
    }
    if (r == FR_INT_ERR || r == FR_NO_PATH) return ESP_ERR_INVALID_STATE;
    if (r != FR_OK) return ESP_FAIL;
    const FATFS *fs = s_fil.obj.fs;
    const DWORD sc = s_fil.obj.sclust;
    if (!f_size(&s_fil) && !sc) {                /* a cut while it was being made: made again */
        f_close(&s_fil);
        f_unlink(p);
        if ((r = create(p)) != FR_OK) return ESP_FAIL;
        return open_checked(n, slot, p);
    }
    const char *bad = NULL;
    if (f_size(&s_fil) != KV_FILE) bad = "its size";
    else if (sc < 2 || sc >= fs->n_fatent) bad = "its cluster";
    else
        for (int i = 0; i < kv_n && !bad; i++)
            for (int s = 0; s < KV_SLOTS; s++)
                if ((&kv_all[i] != n || s != slot) && kv_all[i].clu[s] == sc) bad = "a cluster another copy has";
    const DWORD cl = (DWORD)fs->csize * fs->ssize;
    for (DWORD k = 1; !bad && cl < KV_FILE && k * cl < KV_FILE; k++)
        if (f_lseek(&s_fil, k * cl) != FR_OK || s_fil.clust != sc + k) bad = "its clusters, no longer in a row";
    if (bad) {
        ESP_LOGE(TAG, "%s: %s -- the folder is taken as damaged", p, bad);
        return ESP_ERR_INVALID_STATE;            /* the FIL is dropped: nothing of it written */
    }
    n->clu[slot] = sc;
    return ESP_OK;
}

esp_err_t kvc_write(kv_ns_t *n, int slot, size_t bytes)
{
    char p[40];
    file(p, sizeof p, n, slot);
    esp_err_t e = ESP_FAIL;
    for (int t = 0; t < 2; t++) {
        if (t) vTaskDelay(pdMS_TO_TICKS(200));
        e = open_checked(n, slot, p);
        if (e == ESP_ERR_INVALID_STATE || e == ESP_ERR_NO_MEM) return e;
        if (e != ESP_OK) continue;
        UINT bw = 0, br = 0;
        FRESULT r = f_lseek(&s_fil, 0);
#if VFO_KV_TEST
        if (kv_tear && r == FR_OK) {         /* a power cut's torn copy, made up */
            f_write(&s_fil, kv_img, (bytes / 1024) * 512, &bw);
            ESP_LOGE(TAG, "test: %s torn after %u B, and a crash", p, (unsigned)bw);
            abort();
        }
#endif
        if (r == FR_OK) r = f_write(&s_fil, kv_img, bytes, &bw);
        if (r == FR_OK && bw == bytes) r = f_lseek(&s_fil, 0);
        if (r == FR_OK && bw == bytes) r = f_read(&s_fil, kv_chk, bytes, &br);
        /* Dropped, not closed: the entry is as it was, and FatFs (no
         * FF_FS_LOCK) keeps nothing of an open file. */
        if (r == FR_OK && bw == bytes && br == bytes && !memcmp(kv_img, kv_chk, bytes)) return ESP_OK;
        ESP_LOGW(TAG, "%s: %s (%d)", p, r != FR_OK ? "not written" : "read back different", r);
        e = r == FR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    return e;
}

bool kvc_next_gen(int slot)
{
    if (kvg.gen[slot] >= 9) {
        ESP_LOGE(TAG, "copy %c: no folder generation left; that copy is off", 'A' + slot);
        return false;
    }
    kvg.gen[slot]++;
    mkdirs(slot);
    ESP_LOGW(TAG, "copy %c moves to folder %c%u; the old one is left as it is", 'A' + slot, 'A' + slot, kvg.gen[slot]);
    for (int i = 0; i < kv_n; i++) {
        kv_all[i].clu[slot] = 0;
        if (slot < kv_slots(&kv_all[i]) && kv_all[i].card) {
            kv_all[i].cp[slot] = (kv_copy_t){ KV_ABSENT, false, 0 };
            kv_all[i].repair |= 1u << slot;
        }
    }
    return true;
}
