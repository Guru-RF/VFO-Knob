/* The firmware images on the knob's microSD card. See sd_cache.h. */
#include "sd_cache.h"

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board_pins.h"
#include "ff.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

#define MNT     "/sd"
#define OURS    MNT "/VFO-KNOB"

static StaticSemaphore_t s_mx_buf;
static SemaphoreHandle_t s_mx;
static portMUX_TYPE      s_init = portMUX_INITIALIZER_UNLOCKED;
static sdmmc_card_t     *s_card;
static int               s_users;
static bool              s_said;           /* "no card" logged */
static esp_err_t         s_err = ESP_OK;   /* the last mount's */
static void             *s_bounce;         /* 512 B of internal DMA RAM, made once, kept */
static char              s_drive[8];
#define FLUSHERS 4
static void            (*s_flush[FLUSHERS])(void);
static int               s_nflush;

static const esp_vfs_fat_mount_config_t MOUNT_CFG = {
    .format_if_mount_failed = false,       /* never by itself: see sdc_format() */
    .max_files              = 4,
    .allocation_unit_size   = 16 * 1024,
};

static SemaphoreHandle_t mx(void)
{
    if (!s_mx) {
        taskENTER_CRITICAL(&s_init);
        if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
        taskEXIT_CRITICAL(&s_init);
    }
    return s_mx;
}

/* Mounted, with `cfg`: under mx(). */
static esp_err_t mount_locked(const esp_vfs_fat_mount_config_t *cfg)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* Transfers from PSRAM (the settings' images, the update's buffers) go
     * a sector at a time through one fixed bounce: no allocation per write,
     * and nothing that can fail for want of internal RAM mid-way. */
    if (!s_bounce) s_bounce = heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    host.dma_aligned_buffer = s_bounce;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = BOARD_PIN_SD_CLK;
    slot.cmd = BOARD_PIN_SD_CMD;
    slot.d0  = BOARD_PIN_SD_D0;
    slot.d1  = BOARD_PIN_SD_D1;
    slot.d2  = BOARD_PIN_SD_D2;
    slot.d3  = BOARD_PIN_SD_D3;
    const esp_err_t e = esp_vfs_fat_sdmmc_mount(MNT, &host, &slot, cfg, &s_card);
    s_err = e;
    if (e != ESP_OK) {
        s_card = NULL;
        if (!s_said) ESP_LOGW(TAG, "no card: %s", esp_err_to_name(e));
        s_said = true;
        return e;
    }
    if (s_said || !s_drive[0]) {             /* once a boot, and after a card came back */
        const sdmmc_cid_t *c = &s_card->cid;
        ESP_LOGI(TAG, "card: \"%.8s\" maker 0x%02x OEM 0x%04x rev %d.%d made %d-%02d, %llu MB, %s",
                 c->name, c->mfg_id, c->oem_id, c->revision >> 4, c->revision & 15, 2000 + (c->date >> 4),
                 c->date & 15, (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024),
                 s_card->is_mmc ? "MMC" : (s_card->ocr & (1u << 30)) ? "SDHC/SDXC" : "SDSC");
    }
    s_said = false;
    s_users = 1;
    snprintf(s_drive, sizeof s_drive, "%u:", (unsigned)ff_diskio_get_pdrv_card(s_card));
    mkdir(OURS, 0777);
    return ESP_OK;
}

bool sdc_mount(void)
{
    xSemaphoreTake(mx(), portMAX_DELAY);
    if (s_card) {
        s_users++;
        xSemaphoreGive(mx());
        return true;
    }
    const bool ok = mount_locked(&MOUNT_CFG) == ESP_OK;
    xSemaphoreGive(mx());
    return ok;
}

esp_err_t sdc_prepare(void)
{
    xSemaphoreTake(mx(), portMAX_DELAY);
    if (s_card) {
        s_users++;
        xSemaphoreGive(mx());
        return ESP_OK;
    }
    esp_vfs_fat_mount_config_t cfg = MOUNT_CFG;
    cfg.format_if_mount_failed = true;          /* the owner's word: see sd_cache.h */
    const esp_err_t e = mount_locked(&cfg);
    if (e == ESP_OK) ESP_LOGW(TAG, "card prepared: a new FAT on it");
    xSemaphoreGive(mx());
    return e;
}

esp_err_t sdc_last_error(void) { return s_err; }

const char *sdc_drive(void) { return s_card ? s_drive : NULL; }

uint32_t sdc_cid(void)
{
    xSemaphoreTake(mx(), portMAX_DELAY);
    uint32_t h = 0;
    if (s_card) {
        const sdmmc_cid_t *c = &s_card->cid;
        h = 2166136261u;
        const int v[] = { c->mfg_id, c->oem_id, c->serial };
        for (size_t i = 0; i < sizeof v / sizeof v[0]; i++)
            for (int b = 0; b < 4; b++) h = (h ^ (uint8_t)(v[i] >> (8 * b))) * 16777619u;
        for (size_t i = 0; i < sizeof c->name && c->name[i]; i++) h = (h ^ (uint8_t)c->name[i]) * 16777619u;
        if (!h) h = 1;
    }
    xSemaphoreGive(mx());
    return h;
}

static void sdc_shutdown(void)
{
    for (int i = 0; i < s_nflush; i++) s_flush[i]();
}

esp_err_t sdc_on_restart(void (*fn)(void))
{
    if (!fn) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mx(), portMAX_DELAY);
    esp_err_t e = ESP_OK;
    if (s_nflush >= FLUSHERS) e = ESP_ERR_NO_MEM;
    else {
        if (!s_nflush) e = esp_register_shutdown_handler(sdc_shutdown);
        if (e == ESP_OK) s_flush[s_nflush++] = fn;
    }
    xSemaphoreGive(mx());
    if (e != ESP_OK) ESP_LOGE(TAG, "a restart flusher not registered: %s", esp_err_to_name(e));
    return e;
}

void sdc_unmount(void)
{
    xSemaphoreTake(mx(), portMAX_DELAY);
    if (s_card && --s_users <= 0) {
        esp_vfs_fat_sdcard_unmount(MNT, s_card);
        s_card  = NULL;
        s_users = 0;
    }
    xSemaphoreGive(mx());
}

/* VFO-KNOB/<the radio's name in at most 8 letters and digits>.<ext> */
static void path(char *out, size_t cap, const char *radio, const char *ext)
{
    char code[9];
    int n = 0;
    for (const char *p = radio; *p && n < 8; p++)
        if (isalnum((unsigned char)*p)) code[n++] = (char)toupper((unsigned char)*p);
    code[n] = 0;
    snprintf(out, cap, OURS "/%s.%s", code, ext);
}

static esp_err_t read_all(const char *p, char *buf, size_t cap)
{
    FILE *f = fopen(p, "r");
    if (!f) return ESP_ERR_NOT_FOUND;
    const size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t write_all(const char *p, const char *text)
{
    FILE *f = fopen(p, "w");
    if (!f) return ESP_FAIL;
    const size_t n = strlen(text);
    const bool ok = fwrite(text, 1, n, f) == n;
    return fclose(f) == 0 && ok ? ESP_OK : ESP_FAIL;
}

esp_err_t sdc_manifest(const char *radio, char *buf, size_t cap)
{
    char p[40];
    path(p, sizeof p, radio, "JSN");
    return read_all(p, buf, cap);
}

FILE *sdc_image_open(const char *radio, size_t *size)
{
    char p[40];
    path(p, sizeof p, radio, "BIN");
    struct stat st;
    if (stat(p, &st) != 0 || st.st_size <= 0) return NULL;
    if (size) *size = (size_t)st.st_size;
    return fopen(p, "rb");
}

bool sdc_image_ok(const char *radio, const char *sha256_hex)
{
    if (!sha256_hex || strlen(sha256_hex) != 64) return false;
    FILE *f = sdc_image_open(radio, NULL);
    if (!f) return false;
    enum { CHUNK = 8192 };
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return false;
    }
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    size_t n;
    while ((n = fread(buf, 1, CHUNK, f)) > 0) mbedtls_sha256_update(&c, buf, n);
    uint8_t d[32];
    mbedtls_sha256_finish(&c, d);
    mbedtls_sha256_free(&c);
    fclose(f);
    free(buf);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    return strcasecmp(hex, sha256_hex) == 0;
}

FILE *sdc_image_create(const char *radio)
{
    char p[40];
    path(p, sizeof p, radio, "TMP");
    return fopen(p, "wb");
}

esp_err_t sdc_image_commit(const char *radio, const char *manifest)
{
    char tmp[40], bin[40], jsn[40];
    path(tmp, sizeof tmp, radio, "TMP");
    path(bin, sizeof bin, radio, "BIN");
    path(jsn, sizeof jsn, radio, "JSN");
    /* The manifest goes first and comes last: an image without one, power
     * lost between the two, is taken for none. */
    unlink(jsn);
    unlink(bin);
    if (rename(tmp, bin) != 0) return ESP_FAIL;
    return write_all(jsn, manifest);
}

void sdc_image_discard(const char *radio)
{
    char p[40];
    path(p, sizeof p, radio, "TMP");
    unlink(p);
}

bool sdc_room(const char *radio, size_t bytes)
{
    uint64_t total = 0, avail = 0;
    const uint64_t need = (uint64_t)bytes + 256 * 1024;
    if (esp_vfs_fat_info(MNT, &total, &avail) != ESP_OK) return false;
    if (avail >= need) return true;
    char p[40];
    path(p, sizeof p, radio, "JSN");
    unlink(p);
    path(p, sizeof p, radio, "BIN");
    unlink(p);
    return esp_vfs_fat_info(MNT, &total, &avail) == ESP_OK && avail >= need;
}

esp_err_t sdc_index_save(const char *json)
{
    return write_all(OURS "/INDEX.JSN", json);
}

esp_err_t sdc_index_load(char *buf, size_t cap)
{
    return read_all(OURS "/INDEX.JSN", buf, cap);
}

void sdc_log_state(void)
{
    if (!sdc_mount()) return;
    int n = 0;
    DIR *d = opendir(OURS);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            const size_t l = strlen(e->d_name);
            if (l > 4 && !strcasecmp(e->d_name + l - 4, ".BIN")) n++;
        }
        closedir(d);
    }
    uint64_t total = 0, avail = 0;
    esp_vfs_fat_info(MNT, &total, &avail);
    ESP_LOGI(TAG, "card: %d firmware image%s, %llu MB free of %llu", n, n == 1 ? "" : "s",
             (unsigned long long)(avail >> 20), (unsigned long long)(total >> 20));
    sdc_unmount();
}

esp_err_t sdc_format(void)
{
    xSemaphoreTake(mx(), portMAX_DELAY);
    const bool held = s_card && s_users > 0;
    xSemaphoreGive(mx());
    if (held) {
        ESP_LOGW(TAG, "not emptied: the card is in use (the settings let go of it first)");
        return ESP_ERR_INVALID_STATE;
    }
    if (!sdc_mount()) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(mx(), portMAX_DELAY);
    esp_vfs_fat_mount_config_t cfg = MOUNT_CFG;
    const esp_err_t e = esp_vfs_fat_sdcard_format_cfg(MNT, s_card, &cfg);
    if (e == ESP_OK) mkdir(OURS, 0777);
    uint64_t total = 0, avail = 0;
    esp_vfs_fat_info(MNT, &total, &avail);
    ESP_LOGW(TAG, "card emptied: %s, %llu MB free of %llu", esp_err_to_name(e),
             (unsigned long long)(avail >> 20), (unsigned long long)(total >> 20));
    xSemaphoreGive(mx());
    sdc_unmount();
    return e;
}
