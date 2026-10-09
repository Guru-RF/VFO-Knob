#include "ota.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "sd_cache.h"

static const char *TAG = "ota";

#ifndef OTA_REPO
#define OTA_REPO "Guru-RF/VFO-Knob"
#endif
#ifndef OTA_BRANCH
#define OTA_BRANCH "firmware"
#endif

/* Updates are published as a small manifest plus a signed image under
 * firmware/ in the repo, served by raw.githubusercontent.com -- NOT as GitHub
 * release assets.
 *
 * The reason is CORS. The configuration page needs to fetch the image itself
 * when the knob is on the USB cable, because there the knob is the DHCP server
 * and has no route to the internet: the browser has to do the downloading.
 * Release assets redirect to release-assets.githubusercontent.com, which sends
 * no Access-Control-Allow-Origin, so a page served from 10.55.42.1 cannot read
 * them. raw.githubusercontent.com sends "*", so one source works for both the
 * device (over WiFi) and the browser (over USB). Measured, not assumed.
 *
 * One channel per radio: the image vfo-knob-aethersdr is published under
 * firmware/aethersdr/, so a knob is only ever offered its own releases. */
#define OTA_ROOT_URL "https://raw.githubusercontent.com/" OTA_REPO \
                     "/" OTA_BRANCH "/firmware/"
#define OTA_PREFIX   "vfo-knob-"

static char            s_base[128];       /* OTA_ROOT_URL + radio + "/" */
/* The radio whose channel an install takes its image from: "" for this
 * firmware's own (an update), another's for a switch (ota_start_switch). */
static char            s_switch[16];
static ota_status_t    s_st = { .phase = OTA_IDLE };
static portMUX_TYPE    s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool   s_busy;
/* An install or an upload writes this chip's flash, from before its first
 * erase to its end: what stops the second chip's update (ota_writing()).
 * In PSRAM, as all the second chip's here: tasks only, never code that runs
 * with the flash cache off, and internal RAM is what sessions run short of. */
EXT_RAM_BSS_ATTR static volatile bool s_writing;
EXT_RAM_BSS_ATTR static uint32_t      s_hours;     /* the automatic check's interval; 0: off */

/* The second chip's firmware (ota.h): what is to be had, and the image
 * fetched for it -- all under s_lock, in PSRAM. The manifest's text goes
 * onto the SD card with the image, as published. */
#define COMP            "companion"       /* its channel, and its name on the card */
#define COMP_MANIFEST   1024
EXT_RAM_BSS_ATTR static ota_comp_offer_t s_offer;
EXT_RAM_BSS_ATTR static char             s_offer_json[COMP_MANIFEST];
EXT_RAM_BSS_ATTR static uint8_t         *s_comp_img;        /* fetched, not yet taken */
EXT_RAM_BSS_ATTR static size_t           s_comp_len;
EXT_RAM_BSS_ATTR static uint8_t          s_comp_sha[32];
EXT_RAM_BSS_ATTR static volatile bool    s_comp_busy;       /* a fetch asked for, or running */
EXT_RAM_BSS_ATTR static volatile bool    s_comp_stop;       /* ...to stop now: */
EXT_RAM_BSS_ATTR static const char      *s_comp_stop_why;   /* ...for this */
EXT_RAM_BSS_ATTR static bool             s_comp_network, s_comp_session;   /* the fetch asked for */
EXT_RAM_BSS_ATTR static void           (*s_flash_hook)(void);

static void set_phase(ota_phase_t p, const char *msg)
{
    portENTER_CRITICAL(&s_lock);
    s_st.phase = p;
    if (msg) strlcpy(s_st.message, msg, sizeof s_st.message);
    portEXIT_CRITICAL(&s_lock);
    if (msg) ESP_LOGI(TAG, "%s", msg);
}

void ota_get_status(ota_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    portEXIT_CRITICAL(&s_lock);
}

/* ------------------------------------------------------- version compare */

/* Tags are "v1.2.3" or "1.2.3"; anything unparsable compares as older so a
 * malformed tag can never trigger an install. */
static bool parse_ver(const char *s, int v[3])
{
    if (!s) return false;
    while (*s && (*s < '0' || *s > '9')) s++;
    v[0] = v[1] = v[2] = 0;
    return sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) >= 2;
}

static bool is_newer(const char *candidate, const char *running)
{
    int a[3], b[3];
    if (!parse_ver(candidate, a)) return false;
    if (!parse_ver(running, b))   return true;   /* unversioned dev build */
    for (int i = 0; i < 3; i++) {
        if (a[i] > b[i]) return true;
        if (a[i] < b[i]) return false;
    }
    return false;
}

bool ota_is_newer(const char *candidate, const char *running) { return is_newer(candidate, running); }

/* ------------------------------------------------------------ which radio */

/* The image's project name says which radio's firmware it is. 1.5.0 and
 * earlier were built as plain "vfo-knob": the AetherSDR firmware under its old
 * name, so going back to one is an update, not a switch. */
static const char *product_of(const char *project)
{
    return strcmp(project, "vfo-knob") == 0 ? OTA_PREFIX "aethersdr" : project;
}

static bool same_product(const char *a, const char *b)
{
    return strcmp(product_of(a), product_of(b)) == 0;
}

const char *ota_radio(void)
{
    const char *p = product_of(esp_app_get_description()->project_name);
    const size_t n = strlen(OTA_PREFIX);
    return strncmp(p, OTA_PREFIX, n) == 0 ? p + n : p;
}

/* --------------------------------------------------- tiny JSON extraction */

/* The release feed is large and deeply nested, and a full parser is not worth
 * the internal RAM here. Two fields are needed: "tag_name", and the
 * "browser_download_url" whose value ends in .bin. */
static bool json_string_field(const char *json, const char *key,
                              char *out, size_t len)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    while (*p && *p != '"') p++;
    if (!*p) return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < len) out[i++] = *p++;
    out[i] = 0;
    return i > 0;
}

/* A number's value, as a manifest's "size"; 0 when there is none. */
static uint32_t json_number_field(const char *json, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':'))) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return (*p >= '0' && *p <= '9') ? (uint32_t)strtoul(p, NULL, 10) : 0;
}

/* -------------------------------------------- the SD card's copies (sd_cache) */

/* What an image says it is, from its first bytes: the app description sits
 * past the image header and its first segment's, in every ESP-IDF image.
 * This chip's too: the second chip's firmware, an ESP32's, is signed with
 * the same key, and must never get as far as an erase here. */
static bool image_is(const uint8_t *head, size_t n, const char *want)
{
    const size_t at = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (n < at + sizeof(esp_app_desc_t)) return false;
    if (((const esp_image_header_t *)head)->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) return false;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(head + at);
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) return false;
    char name[sizeof d->project_name + 1];
    memcpy(name, d->project_name, sizeof d->project_name);
    name[sizeof d->project_name] = 0;
    return same_product(name, want);
}

/* The second chip's firmware, by its form alone: what the knob can tell
 * before sending it. Its signature is the chip's to check -- against the
 * key of the firmware it runs -- and the chip's slot is what bounds it. */
bool ota_companion_image_ok(const uint8_t *img, size_t n, char *ver, size_t cap, uint8_t app_sha8[8])
{
    const size_t at = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (!img || n < 2 * 4096 || n % 4096 || n > OTA_COMPANION_SLOT) return false;
    const esp_image_header_t *h = (const esp_image_header_t *)img;
    if (h->magic != ESP_IMAGE_HEADER_MAGIC || h->chip_id != ESP_CHIP_ID_ESP32) return false;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(img + at);
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) return false;
    if (strncmp(d->project_name, OTA_PREFIX "companion", sizeof d->project_name)) return false;
    /* Signed: the last sector is the Secure Boot v2 signature's, which
     * starts with its magic byte. */
    if (img[n - 4096] != 0xE7) return false;
    if (ver && cap) {
        const size_t k = strnlen(d->version, sizeof d->version);
        const size_t m = k < cap - 1 ? k : cap - 1;
        memcpy(ver, d->version, m);
        ver[m] = 0;
    }
    if (app_sha8) memcpy(app_sha8, d->app_elf_sha256, 8);
    return true;
}

/* A radio's image from the card into the update slot -- the signature
 * checked at the end, as for any image -- the progress from pct0 to 100. */
static esp_err_t install_from_card(const char *radio, const char *want, int pct0)
{
    size_t size = 0;
    FILE *f = sdc_image_open(radio, &size);
    if (!f) return ESP_ERR_NOT_FOUND;
    enum { CHUNK = 4096 };
    /* Internal: flash is written from it, and the card reads straight in. */
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t oh = 0;
    esp_err_t err = ESP_ERR_NO_MEM;
    if (!buf || !part) goto out;
    err = ESP_ERR_INVALID_SIZE;
    if (size > part->size) goto out;
    set_phase(OTA_DOWNLOADING, "installing from the SD card");
    /* The room erased up front, in blocks: see ota_upload_begin(). */
    err = esp_ota_begin(part, size, &oh);
    if (err != ESP_OK) {
        oh = 0;
        goto out;
    }
    for (size_t done = 0; done < size; ) {
        const size_t n = fread(buf, 1, CHUNK, f);
        if (!n) { err = ESP_FAIL; break; }
        if (!done && !image_is(buf, n, want)) { err = ESP_ERR_INVALID_VERSION; break; }
        if ((err = esp_ota_write(oh, buf, n)) != ESP_OK) break;
        done += n;
        portENTER_CRITICAL(&s_lock);
        s_st.percent = pct0 + (int)((uint64_t)done * (uint64_t)(100 - pct0) / size);
        portEXIT_CRITICAL(&s_lock);
    }
    if (err != ESP_OK) goto out;
    err = esp_ota_end(oh);                /* where the signature is checked */
    oh = 0;
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
out:
    if (oh) esp_ota_abort(oh);
    free(buf);
    fclose(f);
    return err;
}

/* The SHA-256 of `n` bytes, a piece at a time. */
static void sha_of(const uint8_t *data, size_t n, uint8_t d[32])
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    for (size_t at = 0; at < n; at += 8192)
        mbedtls_sha256_update(&c, data + at, n - at < 8192 ? n - at : 8192);
    mbedtls_sha256_finish(&c, d);
    mbedtls_sha256_free(&c);
}

/* Whether a digest is the one given in hex. */
static bool sha_hex_is(const uint8_t d[32], const char *sha_hex)
{
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    return sha_hex && strcasecmp(hex, sha_hex) == 0;
}

/* Whether `n` bytes hash to the sha256 given (hex). */
static bool sha_is(const uint8_t *data, size_t n, const char *sha_hex)
{
    uint8_t d[32];
    sha_of(data, n, d);
    return sha_hex_is(d, sha_hex);
}

/* An image onto the card with its manifest, the card mounted -- or nothing
 * left there. */
static esp_err_t card_write(const char *radio, const uint8_t *img, size_t size, const char *manifest)
{
    if (!sdc_room(radio, size)) return ESP_ERR_NO_MEM;
    FILE *f = sdc_image_create(radio);
    if (!f) return ESP_FAIL;
    /* 4 kB at a time: one 1.9 MB write would hold the card (FatFs's volume
     * lock) for seconds, and the settings (kvstore) share it. */
    esp_err_t err = ESP_OK;
    for (size_t o = 0; o < size && err == ESP_OK; o += 4096) {
        const size_t k = size - o < 4096 ? size - o : 4096;
        if (fwrite(img + o, 1, k, f) != k || fflush(f) != 0) err = ESP_FAIL;
    }
    if (fclose(f) != 0) err = ESP_FAIL;
    if (err == ESP_OK) err = sdc_image_commit(radio, manifest);
    if (err != ESP_OK) sdc_image_discard(radio);
    return err;
}

/* Internal RAM enough for a download, or the card, beside a radio's
 * session, whose own TLS wants the same RAM: 12 kB free, and a 4 kB DMA
 * block for the bounce buffers. */
static bool ram_floors(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= 12 * 1024 &&
           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) >= 4 * 1024;
}

/* How a download goes. */
typedef struct {
    size_t         max;          /* the most the file may be */
    int            buffer;       /* esp_http_client's receive buffer, internal RAM */
    int            timeout_ms;   /* each read's: how long a stop may wait */
    bool           floors;       /* each read only above ram_floors(), else ESP_ERR_NO_MEM */
    volatile bool *stop;         /* set: it ends early */
} dl_t;

/* A file downloaded whole into PSRAM, its connection closed after; *out is
 * then the caller's to free. Never onto the card at the same time: see
 * download_to_card(). */
static esp_err_t download_to_psram(const char *url, const char *what, const dl_t *d,
                                   uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    esp_http_client_config_t hc = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = d->timeout_ms,
        .buffer_size       = d->buffer,
    };
    esp_http_client_handle_t h = esp_http_client_init(&hc);
    if (!h) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(h, "User-Agent", "VFO-Knob");
    uint8_t *img = NULL;
    int64_t total = 0, got = 0;
    esp_err_t err = esp_http_client_open(h, 0);
    if (err == ESP_OK) {
        total = esp_http_client_fetch_headers(h);
        if (esp_http_client_get_status_code(h) != 200 || total <= 0 || total > (int64_t)d->max)
            err = ESP_ERR_INVALID_RESPONSE;
        else if (!(img = heap_caps_malloc((size_t)total, MALLOC_CAP_SPIRAM)))
            err = ESP_ERR_NO_MEM;
    }
    int64_t moved = esp_timer_get_time();
    while (err == ESP_OK && got < total) {
        if (d->stop && *d->stop) { err = ESP_ERR_INVALID_STATE; break; }
        if (d->floors && !ram_floors()) {
            ESP_LOGW(TAG, "%s: internal RAM ran short (%u free, largest DMA block %u): the download stopped at "
                          "%lld of %lld bytes", what, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                     (long long)got, (long long)total);
            err = ESP_ERR_NO_MEM;
            break;
        }
        const int n = esp_http_client_read(h, (char *)img + got,
                                           (int)(total - got < 8192 ? total - got : 8192));
        /* No data yet is not an error -- esp_https_ota reads on through it
         * too -- unless nothing comes for half a minute. */
        if (n == -ESP_ERR_HTTP_EAGAIN && esp_timer_get_time() - moved < 30000000) continue;
        if (n <= 0) {
            ESP_LOGW(TAG, "%s: the download stopped at %lld of %lld bytes (%d)", what,
                     (long long)got, (long long)total, n);
            err = ESP_FAIL;
            break;
        }
        moved = esp_timer_get_time();
        got += n;
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK) {
        free(img);
        return err;
    }
    *out     = img;
    *out_len = (size_t)total;
    return ESP_OK;
}

/* An image downloaded onto the card, its sha256 checked against the
 * manifest's -- or nothing left on the card. Into PSRAM first, and onto the
 * card only once its connection is closed, the card mounted only then: the
 * two at once failed, as TLS's hardware AES and the card's DMA each take
 * internal DMA RAM for their bounce buffers a little at a time, and the AES
 * went without ("esp-aes: Failed to allocate memory") a few per cent in.
 * `stop` ends it early. */
static esp_err_t download_to_card(const char *url, const char *radio, const char *sha_hex,
                                  const char *manifest, volatile bool *stop)
{
    const dl_t d = { .max = 4 * 1024 * 1024, .buffer = 4096, .timeout_ms = 20000, .stop = stop };
    uint8_t *img = NULL;
    size_t   n   = 0;
    esp_err_t err = download_to_psram(url, radio, &d, &img, &n);
    if (err == ESP_OK && !sha_is(img, n, sha_hex)) err = ESP_ERR_INVALID_CRC;
    if (err == ESP_OK) {
        if (sdc_mount()) {
            err = card_write(radio, img, n, manifest);
            sdc_unmount();
        } else {
            err = ESP_ERR_NOT_FOUND;
        }
    }
    free(img);
    return err;
}

/* The image just installed, from its update slot onto the card, the card not
 * mounted: for the next time. After the download, never beside it -- see
 * download_to_card(). */
static void card_keep(const char *radio, size_t size, const char *sha_hex, const char *manifest)
{
    const esp_partition_t *part = esp_ota_get_boot_partition();
    if (!part || !size || size > part->size || !sdc_mount()) return;
    set_phase(OTA_DOWNLOADING, "a copy onto the SD card");
    uint8_t *img = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    esp_err_t err = img ? esp_partition_read(part, 0, img, size) : ESP_ERR_NO_MEM;
    if (err == ESP_OK && !sha_is(img, size, sha_hex)) err = ESP_ERR_INVALID_CRC;
    if (err == ESP_OK) err = card_write(radio, img, size, manifest);
    free(img);
    sdc_unmount();
    ESP_LOGI(TAG, "SD card: a copy of %s: %s", radio, esp_err_to_name(err));
}

/* The card's manifest for a radio says the same image as the server's -- or,
 * with no sha256 to match, any -- and the image is there. */
static bool card_has(const char *radio, const char *sha_hex, char *ver, size_t cap)
{
    char *m = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    char msha[72] = "";
    if (m && sdc_manifest(radio, m, 1024) == ESP_OK) {
        json_string_field(m, "sha256", msha, sizeof msha);
        if (ver && cap) json_string_field(m, "version", ver, cap);
    }
    free(m);
    if (!msha[0] || (sha_hex && strcasecmp(msha, sha_hex))) return false;
    FILE *f = sdc_image_open(radio, NULL);
    if (f) fclose(f);
    return f != NULL;
}

/* An image from the card into PSRAM, whole, when the card's manifest is for
 * the sha256 given -- the second chip's, which is sent from RAM and never
 * installed here -- the card mounted meanwhile. *out is then the caller's to
 * free, its hash the caller's to check. `floors`: beside a radio's session,
 * each read only above ram_floors(), the card's bounce buffers being
 * internal RAM too. */
static esp_err_t card_read(const char *radio, const char *sha_hex, size_t max, bool floors,
                           volatile bool *stop, uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (floors && !ram_floors()) return ESP_ERR_NO_MEM;
    if (!sdc_mount()) return ESP_ERR_NOT_FOUND;
    esp_err_t err  = ESP_ERR_NOT_FOUND;
    size_t    size = 0;
    uint8_t  *img  = NULL;
    FILE *f = card_has(radio, sha_hex, NULL, 0) ? sdc_image_open(radio, &size) : NULL;
    if (f) {
        err = !size || size > max                                   ? ESP_ERR_INVALID_SIZE
            : !(img = heap_caps_malloc(size, MALLOC_CAP_SPIRAM)) ? ESP_ERR_NO_MEM : ESP_OK;
        for (size_t got = 0; err == ESP_OK && got < size; ) {
            const size_t k = size - got < 8192 ? size - got : 8192;
            if (stop && *stop)               err = ESP_ERR_INVALID_STATE;
            else if (floors && !ram_floors()) err = ESP_ERR_NO_MEM;
            else if (fread(img + got, 1, k, f) != k) err = ESP_FAIL;
            got += k;
        }
        fclose(f);
    }
    sdc_unmount();
    if (err != ESP_OK) {
        free(img);
        return err;
    }
    *out     = img;
    *out_len = size;
    return ESP_OK;
}

/* ------------------------------------------------------------- the worker */

/* One small file's answer, on a connection open or opened now. */
static esp_err_t get_small(esp_http_client_handle_t h, char *body, size_t cap, int *out_len)
{
    esp_err_t err = esp_http_client_open(h, 0);
    if (err != ESP_OK) return err;
    if (esp_http_client_fetch_headers(h) < 0) return ESP_FAIL;        /* no answer at all */
    const int status = esp_http_client_get_status_code(h);
    if (status != 200) return status == 404 ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_RESPONSE;
    const int n = esp_http_client_read_response(h, body, (int)cap - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = 0;
    *out_len = n;
    return ESP_OK;
}

/* A small file from the update server into `body`, NUL-terminated. With
 * `conn`, on the connection it holds: opened on first use and left open for
 * the next file from the same server -- a second TLS handshake costs about
 * as long as the first -- unless the server would not keep it, or this
 * answer was not read to its end. A kept connection the server has let go
 * meanwhile is opened again, once. Without `conn`, one of its own, closed
 * after. */
static esp_err_t fetch_on(esp_http_client_handle_t *conn, const char *url, char *body, size_t cap,
                          int *out_len)
{
    esp_http_client_handle_t h = conn ? *conn : NULL;
    const bool again = h != NULL;
    if (h) {
        if (esp_http_client_set_url(h, url) != ESP_OK) return ESP_ERR_INVALID_ARG;
    } else {
        esp_http_client_config_t c = {
            .url               = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms        = 8000,        /* a few hundred bytes; do not linger */
            .keep_alive_enable = false,
        };
        h = esp_http_client_init(&c);
        if (!h) return ESP_ERR_NO_MEM;
        /* raw.githubusercontent.com rejects requests with no User-Agent. */
        esp_http_client_set_header(h, "User-Agent", "VFO-Knob");
        if (conn) *conn = h;
    }
    esp_err_t err = get_small(h, body, cap, out_len);
    if (again && err != ESP_OK && err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_RESPONSE) {
        esp_http_client_close(h);
        err = get_small(h, body, cap, out_len);
    }
    if (!conn || err != ESP_OK || !esp_http_client_is_persistent_connection(h) ||
        !esp_http_client_is_complete_data_received(h))
        esp_http_client_close(h);
    if (!conn) esp_http_client_cleanup(h);
    return err;
}

/* The connection fetch_on() kept, let go. */
static void fetch_done(esp_http_client_handle_t *conn)
{
    if (!*conn) return;
    esp_http_client_close(*conn);
    esp_http_client_cleanup(*conn);
    *conn = NULL;
}

static esp_err_t fetch_url(const char *url, char *body, size_t cap, int *out_len)
{
    return fetch_on(NULL, url, body, cap, out_len);
}

static esp_err_t fetch_latest(esp_http_client_handle_t *conn, const char *base, char *body, size_t cap,
                              int *out_len)
{
    char url[160];
    snprintf(url, sizeof url, "%smanifest.json", base);
    return fetch_on(conn, url, body, cap, out_len);
}

/* ------------------------------------------------ the second chip's firmware
 *
 * The knob's update channel has a second-chip channel beside its radios':
 * firmware/companion/, with a manifest of its own -- the image's sha256, and
 * its identity, app_elf_sha256's first 8 bytes, as the chip reports its own
 * (bt_link_proto.h). It is looked up with every check of the knob's own,
 * fetched into PSRAM by the same worker, and handed to bt_link by the
 * caller (main/app_main.c), which sends it at a quiet moment. Never
 * installed on this chip: nothing here writes its flash. */

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* A second-chip manifest, read: the companion's project, a version, a file
 * in its channel, the image's sha256 and its identity, and its size when it
 * says one. */
static bool offer_parse(const char *m, ota_comp_offer_t *o)
{
    char project[40] = "", app[72] = "";
    memset(o, 0, sizeof *o);
    json_string_field(m, "project", project, sizeof project);
    if (strcmp(project, OTA_PREFIX COMP) != 0) return false;
    if (!json_string_field(m, "version", o->version, sizeof o->version) ||
        !json_string_field(m, "file", o->file, sizeof o->file) ||
        !json_string_field(m, "sha256", o->sha256, sizeof o->sha256) || strlen(o->sha256) != 64 ||
        !json_string_field(m, "app_sha256", app, sizeof app) || strlen(app) < 16)
        return false;
    for (int i = 0; i < 8; i++) {
        const int hi = hexval(app[2 * i]), lo = hexval(app[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        o->app_sha[i] = (uint8_t)(hi << 4 | lo);
    }
    /* Its file, in its own channel and nowhere else. */
    if (strchr(o->file, '/') || strchr(o->file, '\\') || strstr(o->file, "..")) return false;
    o->size = json_number_field(m, "size");
    if (o->size % 4096 || o->size > OTA_COMPANION_SLOT) return false;
    o->known = true;
    return true;
}

/* "v1.18.0" is "1.18.0": an image says its version with the v, a manifest
 * without. */
static bool same_version(const char *a, const char *b)
{
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    return strcmp(a, b) == 0;
}

#if !VFO_RADIO_SETUP
/* The second chip's firmware on the update server, looked up on the
 * connection of the check that has just reached it: the offer, the card's
 * given up for it. How long the look took is said, as it rides on every
 * check. */
static void companion_look(esp_http_client_handle_t *conn)
{
    char *m = heap_caps_malloc(COMP_MANIFEST, MALLOC_CAP_SPIRAM);
    if (!m) return;
    const int64_t t0 = esp_timer_get_time();
    int len = 0;
    esp_err_t e = fetch_on(conn, OTA_ROOT_URL COMP "/manifest.json", m, COMP_MANIFEST, &len);
    const unsigned ms = (unsigned)((esp_timer_get_time() - t0) / 1000);
    ota_comp_offer_t o;
    if (e == ESP_OK && !offer_parse(m, &o)) e = ESP_ERR_INVALID_RESPONSE;
    if (e == ESP_OK) {
        char a[17];
        for (int i = 0; i < 8; i++) sprintf(a + 2 * i, "%02x", o.app_sha[i]);
        ESP_LOGI(TAG, "second chip: the update server has %s [%s] (looked up in %u ms)", o.version, a, ms);
        portENTER_CRITICAL(&s_lock);
        o.route  = s_offer.route;
        o.looked = s_offer.looked;
        s_offer  = o;
        memcpy(s_offer_json, m, COMP_MANIFEST);
        portEXIT_CRITICAL(&s_lock);
    } else if (e == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "second chip: the update server has no firmware for it (looked up in %u ms)", ms);
        portENTER_CRITICAL(&s_lock);
        if (!s_offer.from_card) s_offer.known = false;     /* withdrawn: the card's stays */
        portEXIT_CRITICAL(&s_lock);
    } else {
        ESP_LOGW(TAG, "second chip: its firmware's manifest on the update server: %s", esp_err_to_name(e));
    }
    free(m);
}
#endif

/* The offer's image, fetched (ota_companion_fetch()) on the check's worker,
 * its stack in PSRAM: TLS and the card, never this chip's flash. The card
 * first, for its copy is a second's read; else the download, 1 kB at a
 * time through esp_http_client's buffer, for its internal RAM. Its hash is
 * taken once, here. */
static void companion_fetch(void)
{
    EXT_RAM_BSS_ATTR static ota_comp_offer_t o;
    EXT_RAM_BSS_ATTR static char             man[COMP_MANIFEST];
    portENTER_CRITICAL(&s_lock);
    o = s_offer;
    memcpy(man, s_offer_json, sizeof man);
    const bool network = s_comp_network, session = s_comp_session;
    portEXIT_CRITICAL(&s_lock);

    uint8_t *img = NULL;
    size_t   n   = 0;
    uint8_t  sha[32] = { 0 };
    bool     downloaded = false;
    esp_err_t e = ESP_ERR_NOT_FOUND;
    const int64_t t0   = esp_timer_get_time();
    const size_t  ram0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (o.known && !s_comp_stop) {
        e = card_read(COMP, o.sha256, OTA_COMPANION_SLOT, session, &s_comp_stop, &img, &n);
        if (e == ESP_OK) {
            sha_of(img, n, sha);
            if (!sha_hex_is(sha, o.sha256)) {
                ESP_LOGW(TAG, "second chip: the SD card's %s does not match its manifest", o.version);
                free(img);
                img = NULL;
                e   = ESP_ERR_INVALID_CRC;
            }
        }
    }
    if (!img && o.known && network && !s_comp_stop && e != ESP_ERR_NO_MEM) {
        char url[192];
        snprintf(url, sizeof url, OTA_ROOT_URL COMP "/%s", o.file);
        /* Each read waits 8 s at most: an install of this chip's own, which
         * stops this, waits 10. */
        const dl_t d = { .max = OTA_COMPANION_SLOT, .buffer = 1024, .timeout_ms = 8000, .floors = session,
                         .stop = &s_comp_stop };
        ESP_LOGI(TAG, "second chip: downloading %s (internal RAM %u free, largest DMA block %u)", o.version,
                 (unsigned)ram0, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        e = download_to_psram(url, "second chip", &d, &img, &n);
        if (e == ESP_OK) {
            sha_of(img, n, sha);
            downloaded = sha_hex_is(sha, o.sha256);
            if (!downloaded) {
                free(img);
                img = NULL;
                e   = ESP_ERR_INVALID_CRC;
            }
        }
    }
    /* What it is, by its own bytes: the manifest's release of the second
     * chip's firmware. */
    if (img) {
        char    ver[32];
        uint8_t app[8];
        const char *bad = !ota_companion_image_ok(img, n, ver, sizeof ver, app) ? "not a signed second-chip firmware"
                        : o.size && n != o.size                                 ? "not its manifest's size"
                        : !same_version(ver, o.version)                         ? "not its manifest's version"
                        : memcmp(app, o.app_sha, 8)                             ? "not its manifest's image"
                        : NULL;
        if (bad) {
            ESP_LOGW(TAG, "second chip: %s from %s not taken: %s", o.version,
                     downloaded ? "the update server" : "the SD card", bad);
            free(img);
            img = NULL;
            e   = ESP_ERR_INVALID_VERSION;
        }
    }
    const unsigned secs = (unsigned)((esp_timer_get_time() - t0 + 500000) / 1000000);
    if (img && downloaded) {
        /* A copy onto the card in the boot window, the connection closed:
         * never beside a radio's session (see download_to_card()). */
        esp_err_t ce = ESP_ERR_INVALID_STATE;
        if (!session && !s_comp_stop) {
            ce = ESP_ERR_NOT_FOUND;
            if (sdc_mount()) {
                ce = card_write(COMP, img, n, man);
                sdc_unmount();
            }
        }
        ESP_LOGI(TAG, "second chip: %s downloaded in %u s (internal RAM %u -> %u free)%s%s", o.version, secs,
                 (unsigned)ram0, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 session ? "" : "; a copy onto the SD card: ", session ? "" : esp_err_to_name(ce));
    } else if (img) {
        ESP_LOGI(TAG, "second chip: %s from the SD card, %u bytes, sha256 good", o.version, (unsigned)n);
    } else if (o.known) {
        const char *stop = s_comp_stop_why ? s_comp_stop_why : "stopped";
        ESP_LOGW(TAG, "second chip: %s not fetched: %s", o.version,
                 s_comp_stop || e == ESP_ERR_INVALID_STATE  ? stop
                 : e == ESP_ERR_NO_MEM                       ? "internal RAM ran short"
                 : e == ESP_ERR_NOT_FOUND && !network        ? "not on the SD card, and not to be downloaded now"
                 : esp_err_to_name(e));
    }
    portENTER_CRITICAL(&s_lock);
    s_comp_img = img;
    s_comp_len = n;
    memcpy(s_comp_sha, sha, sizeof s_comp_sha);
    portEXIT_CRITICAL(&s_lock);
    s_comp_busy = false;
}

static void ota_run(bool install)
{
    /* The manifest is a few hundred bytes; 2 kB is generous. PSRAM: a check
     * may run beside a live TCI session, and anything under 16 kB would
     * otherwise be taken from internal RAM first. */
    const size_t cap = 2048;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    char *keep = NULL;                /* the manifest, for the SD card's copy */
    /* A check keeps its connection for the second chip's look after it. */
    esp_http_client_handle_t conn = NULL;
    esp_err_t got = ESP_FAIL;
    if (!body) body = malloc(cap);
    if (!body) { set_phase(OTA_FAILED, "out of memory"); goto done; }

    /* A switch takes another radio's channel, and its image whatever its
     * version: it is a choice, not an update. */
    const bool sw = install && s_switch[0];
    char base[128], want[32];
    if (sw) {
        snprintf(base, sizeof base, "%s%s/", OTA_ROOT_URL, s_switch);
        snprintf(want, sizeof want, OTA_PREFIX "%s", s_switch);
    } else {
        strlcpy(base, s_base, sizeof base);
        strlcpy(want, esp_app_get_description()->project_name, sizeof want);
    }
    set_phase(OTA_CHECKING, sw ? "looking up the firmware" : "checking for a newer release");
    int len = 0;
    got = fetch_latest(install ? NULL : &conn, base, body, cap, &len);
    /* No update server, and a switch: the SD card's copy, if it has one --
     * a deliberate choice, and the card holds only what was published. */
    bool from_card = false;
    if (got != ESP_OK && sw && sdc_mount()) {
        if (sdc_manifest(s_switch, body, cap) == ESP_OK) {
            got = ESP_OK;
            from_card = true;
            ESP_LOGI(TAG, "no update server: the SD card's %s", s_switch);
        }
        sdc_unmount();
    }
    if (got == ESP_ERR_NOT_FOUND) {
        /* The server answered: this radio's channel has no release in it. */
        set_phase(OTA_FAILED, "no release published for this radio yet");
        goto done;
    }
    if (got != ESP_OK) {
        /* Over the USB cable this is expected and not a fault: the knob has no
         * gateway. The configuration page does the checking in that case. */
        set_phase(OTA_FAILED, "no route to the update server");
        goto done;
    }

    char tag[32] = { 0 };
    if (!json_string_field(body, "version", tag, sizeof tag)) {
        set_phase(OTA_FAILED, "manifest has no version");
        goto done;
    }
    const bool newer = is_newer(tag, s_st.running);
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_st.available, tag, sizeof s_st.available);
    s_st.newer = newer;
    portEXIT_CRITICAL(&s_lock);

    if (!newer && !sw) {
        char m[96];
        snprintf(m, sizeof m, "%s is the latest release", tag);
        set_phase(OTA_UP_TO_DATE, m);
        goto done;
    }
    if (!install) {
        char m[96];
        snprintf(m, sizeof m, "%s is available", tag);
        set_phase(OTA_IDLE, m);
        goto done;
    }

    char file[96], url[256];
    if (!json_string_field(body, "file", file, sizeof file)) {
        set_phase(OTA_FAILED, "manifest names no image");
        goto done;
    }
    snprintf(url, sizeof url, "%s%s", base, file);

    /* The SD card: the image if it has this one, its sha256 checked, and
     * installed from it. Else downloaded straight in as ever, and a copy put
     * on the card after, for the next time -- not downloaded onto the card
     * and installed from there, which ran TLS and the card side by side:
     * see download_to_card(). */
    char sha[72] = "";
    const char *radio = sw ? s_switch : ota_radio();
    json_string_field(body, "sha256", sha, sizeof sha);
    if (sha[0] && sdc_mount()) {
        bool have = false;
        if (card_has(radio, sha, NULL, 0)) {
            set_phase(OTA_CHECKING, "checking the SD card's copy");
            have = sdc_image_ok(radio, sha);
            if (!have) ESP_LOGW(TAG, "the SD card's %s does not match its manifest", radio);
        }
        if (have) {
            free(body);
            body = NULL;
            const esp_err_t e = install_from_card(radio, want, 0);
            sdc_unmount();
            if (e == ESP_OK)
                set_phase(OTA_DONE_REBOOT_NEEDED, "update installed; reboot to run it");
            else
                set_phase(OTA_FAILED, e == ESP_ERR_INVALID_VERSION   ? "the SD card offered another radio's firmware"
                                    : e == ESP_ERR_OTA_VALIDATE_FAILED ? "update rejected -- bad signature or bad image"
                                                                       : "could not install from the SD card");
            goto done;
        }
        sdc_unmount();
    }
    if (from_card) {
        set_phase(OTA_FAILED, "no route to the update server");
        goto done;
    }
    /* The manifest, for the card's copy; in PSRAM, as small as it is. */
    if (sha[0] && (keep = heap_caps_malloc(strlen(body) + 1, MALLOC_CAP_SPIRAM)))
        strcpy(keep, body);
    free(body);
    body = NULL;                      /* the TLS session wants the room back */

    set_phase(OTA_DOWNLOADING, sw ? "downloading the firmware" : "downloading update");
    /* Internal RAM is what a download runs short of: say what it starts with. */
    ESP_LOGI(TAG, "internal RAM free %u, largest DMA block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    esp_http_client_config_t hc = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .keep_alive_enable = true,
    };
    /* Erase the slot up front in blocks, not a sector per 4 kB as it arrives:
     * see ota_upload_begin(). */
    esp_https_ota_config_t oc = { .http_config = &hc, .bulk_flash_erase = true };

    esp_https_ota_handle_t oh = NULL;
    if (esp_https_ota_begin(&oc, &oh) != ESP_OK || !oh) {
        set_phase(OTA_FAILED, "could not start the download");
        goto done;
    }
    /* The channel is this radio's, so this should never trip; it is what
     * stands between a mistake in publishing and a knob that no longer talks
     * to its radio. The header is read before the rest is downloaded. */
    esp_app_desc_t nd = { 0 };
    if (esp_https_ota_get_img_desc(oh, &nd) != ESP_OK ||
        !same_product(nd.project_name, want)) {
        esp_https_ota_abort(oh);
        set_phase(OTA_FAILED, "the channel offered another radio's firmware");
        goto done;
    }
    int total = esp_https_ota_get_image_size(oh), err;
    while ((err = esp_https_ota_perform(oh)) ==
           ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (total > 0) {
            portENTER_CRITICAL(&s_lock);
            s_st.percent = esp_https_ota_get_image_len_read(oh) * 100 / total;
            portEXIT_CRITICAL(&s_lock);
        }
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(oh)) {
        esp_https_ota_abort(oh);
        set_phase(OTA_FAILED, "download failed or was truncated");
        goto done;
    }
    /* The signature is checked here. A bad one fails the finish, and the
     * running image is untouched. */
    if (esp_https_ota_finish(oh) != ESP_OK) {
        set_phase(OTA_FAILED, "update rejected -- bad signature or bad image");
        goto done;
    }
    if (keep && total > 0) card_keep(radio, (size_t)total, sha, keep);
    set_phase(OTA_DONE_REBOOT_NEEDED, "update installed; reboot to run it");

done:
    free(keep);
    free(body);
    s_switch[0] = 0;
    /* Whether the server answered -- this radio's channel empty is an answer
     * too -- for the second chip's look, which rides on it while the
     * automatic check is on. This firmware's own answer goes out first: the
     * boot check waits for it alone, within its own few seconds
     * (check_now()); the look follows on the same connection while the
     * worker is still busy (ota_busy()), which an install, and the boot
     * window's second chip, wait out. The setup firmware sends nothing to
     * the chip: it only keeps its image on the card (ota_cache()). */
    const bool answered = !install && (got == ESP_OK || got == ESP_ERR_NOT_FOUND);
    portENTER_CRITICAL(&s_lock);
    if (!install) {
        s_offer.looked = true;
        s_offer.route  = answered;
    }
    s_st.checks++;
    portEXIT_CRITICAL(&s_lock);
#if !VFO_RADIO_SETUP
    if (answered && s_hours) companion_look(&conn);
#endif
    fetch_done(&conn);
    s_busy = false;
}

/* This chip's flash is about to be written, by the calling task: the second
 * chip's update steps aside. A transfer to it stops (the hook: bt_link), and
 * so does a fetch of its image, which is waited for, 10 s at most -- its TLS
 * and the card's DMA want the internal RAM an install needs. This chip's
 * own update comes first. */
static void comp_yield(void)
{
    if (s_flash_hook) {
        s_flash_hook();
        /* A pass of the link's task, so that its ABORT is on the wire
         * before an erase stalls both cores. */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_comp_stop_why = "this chip's own update comes first";
    s_comp_stop     = true;
    for (int i = 0; i < 50 && s_comp_busy; i++) vTaskDelay(pdMS_TO_TICKS(200));
    if (s_comp_busy) ESP_LOGW(TAG, "second chip: the fetch of its firmware has not stopped in 10 s");
}

/* The task watchdog, relaxed while this chip's flash is written. An upload or
 * an install erases the image's whole room up front, stalling both cores with
 * the cache off, while the display redraws the progress ring: with a radio
 * session keeping both cores busy besides, core 1's idle task went 5 s
 * without running two seconds into an upload; and core 0's as long in the
 * setup firmware, erasing for SVXConnect (2.3 MB) from the SD card (v1.19.0):
 * it restarted into the setup firmware, nothing installed. Half a minute
 * while it runs, the usual 5 s back after. */
#ifdef CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
#define WDT_IDLE0 1
#else
#define WDT_IDLE0 0
#endif
#ifdef CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
#define WDT_IDLE1 2
#else
#define WDT_IDLE1 0
#endif
void ota_watchdog_relax(bool relaxed)
{
#ifdef CONFIG_ESP_TASK_WDT_INIT
    const esp_task_wdt_config_t c = {
        .timeout_ms     = relaxed ? 30000 : CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = WDT_IDLE0 | WDT_IDLE1,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&c);
#else
    (void)relaxed;
#endif
}

/* Installing writes flash, and a task whose stack is in PSRAM must not, so an
 * install gets its own internal-stack task for the one run. */
static void ota_install_task(void *arg)
{
    (void)arg;
    comp_yield();
    ota_watchdog_relax(true);
    ota_run(true);
    ota_watchdog_relax(false);
    s_writing = false;
    vTaskDelete(NULL);
}

/* Looking only runs on one worker, made once and never deleted: its stack in
 * PSRAM, its control block static and internal. Never deleted, because
 * deleting a task with a PSRAM stack makes IDF allocate a helper task in
 * internal RAM -- and abort() if it cannot, which is exactly the resource a
 * check beside a live TCI session is short of. It runs the second chip's
 * fetches too, as jobs of their own: a check asked for while one runs goes
 * right after it, and never fails for it. */
static StaticTask_t s_chk_tcb;
static TaskHandle_t s_chk;

#define JOB_CHECK     (1u << 0)           /* ota_run(false) */
#define JOB_COMPANION (1u << 1)           /* companion_fetch() */

static void ota_check_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t jobs = 0;
        xTaskNotifyWait(0, UINT32_MAX, &jobs, portMAX_DELAY);
        /* The check first: a fetch takes the image its look found. */
        if (jobs & JOB_CHECK)     ota_run(false);
        if (jobs & JOB_COMPANION) companion_fetch();
    }
}

/* The worker, made on its first job. Two tasks asking at once -- a check
 * from the page, a fetch from the supervisor -- make one. */
static bool worker(void)
{
    EXT_RAM_BSS_ATTR static bool making;
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        const bool have = s_chk != NULL, mine = !have && !making;
        if (mine) making = true;
        portEXIT_CRITICAL(&s_lock);
        if (have) return true;
        if (mine) break;
        vTaskDelay(1);
    }
    /* IDF's StackType_t is a byte, so the depth is in bytes. */
    StackType_t *stack = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    TaskHandle_t t = stack ? xTaskCreateStaticPinnedToCore(ota_check_task, "otachk", 8192, NULL, 4, stack,
                                                           &s_chk_tcb, 0)
                           : NULL;
    if (!t) heap_caps_free(stack);
    portENTER_CRITICAL(&s_lock);
    s_chk  = t;
    making = false;
    portEXIT_CRITICAL(&s_lock);
    return t != NULL;
}

/* --------------------------------------------------------------- upload */

static esp_ota_handle_t       s_up;
static const esp_partition_t *s_up_part;
static size_t                 s_up_written;
static char                   s_up_want[32];     /* the project it must be */

bool ota_busy(void) { return s_busy || s_up; }

esp_err_t ota_upload_begin(const char *radio, size_t size)
{
    if (s_busy || s_up) return ESP_ERR_INVALID_STATE;
    /* The second chip's firmware is no radio's, and never this chip's: it
     * goes over the link (POST /api/bt/update), refused before any erase. */
    if (radio && strcmp(radio, COMP) == 0) return ESP_ERR_INVALID_ARG;
    if (radio && *radio)
        snprintf(s_up_want, sizeof s_up_want, OTA_PREFIX "%s", radio);
    else
        strlcpy(s_up_want, esp_app_get_description()->project_name,
                sizeof s_up_want);
    s_up_part = esp_ota_get_next_update_partition(NULL);
    if (!s_up_part) return ESP_ERR_NOT_FOUND;
    if (size > s_up_part->size) return ESP_ERR_INVALID_SIZE;
    s_up_written = 0;
    s_busy = true;                  /* before the erase: nothing else starts */
    s_writing = true;
    comp_yield();

    /* Erase the image's room first, in 64 kB blocks, rather than a 4 kB
     * sector at a time as the data arrives. Every erase stalls both cores
     * with the cache off. Sector by sector, erasing was most of an upload's
     * 23 s and ran all through it, and core 1 -- TinyUSB and LVGL -- was left
     * so little time that the task watchdog reset the knob mid-upload. Up
     * front in blocks it takes 4 s and yields between blocks, and the
     * transfer after it only programs pages: 10 s, the cores 5-24% idle
     * (measured). Without a length, erase as it goes, as before. */
    set_phase(OTA_DOWNLOADING, size ? "erasing the update slot"
                                    : "receiving uploaded image");
    esp_err_t err = esp_ota_begin(s_up_part,
                                  size ? size : OTA_WITH_SEQUENTIAL_WRITES,
                                  &s_up);
    if (err != ESP_OK) {
        s_up = 0;
        s_busy = false;
        s_writing = false;
        set_phase(OTA_FAILED, "could not prepare the update slot");
        return err;
    }
    set_phase(OTA_DOWNLOADING, "receiving uploaded image");
    return ESP_OK;
}

esp_err_t ota_upload_write(const void *data, size_t len)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_ota_write(s_up, data, len);
    if (err != ESP_OK) return err;
    s_up_written += len;
    /* The partition size is the only bound available -- a browser upload has
     * no trustworthy length -- so report against that. */
    portENTER_CRITICAL(&s_lock);
    s_st.percent = (int)(s_up_written * 100 / s_up_part->size);
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

/* Every radio's firmware is signed with the same key, so the signature alone
 * would install any of them. Read back once it has been checked: the name is
 * covered by the signature too.
 *
 * A function of its own, so these buffers are not on the stack while
 * esp_ota_end() checks the RSA signature -- the deepest point of an upload, on
 * the web server's task. Declared inside ota_upload_end() they overflowed it. */
static __attribute__((noinline)) bool upload_is_wanted(void)
{
    esp_app_desc_t d = { 0 };
    if (esp_ota_get_partition_description(s_up_part, &d) == ESP_OK &&
        same_product(d.project_name, s_up_want))
        return true;
    char m[96];
    snprintf(m, sizeof m, "rejected: %.32s is not %s", d.project_name,
             s_up_want);
    set_phase(OTA_FAILED, m);
    return false;
}

esp_err_t ota_upload_end(void)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    /* esp_ota_end() is where the image is validated and, with
     * CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT, where the RSA signature
     * is checked. A bad image fails here and the running one is untouched. */
    esp_err_t err = esp_ota_end(s_up);
    s_up = 0;
    s_busy = false;
    if (err != ESP_OK) {
        s_writing = false;
        set_phase(OTA_FAILED, err == ESP_ERR_OTA_VALIDATE_FAILED
                                  ? "rejected: bad signature or bad image"
                                  : "rejected: image did not validate");
        return err;
    }
    if (!upload_is_wanted()) {
        s_writing = false;
        return ESP_ERR_NOT_SUPPORTED;
    }
    err = esp_ota_set_boot_partition(s_up_part);
    s_writing = false;
    if (err != ESP_OK) {
        set_phase(OTA_FAILED, "could not switch to the new image");
        return err;
    }
    set_phase(OTA_DONE_REBOOT_NEEDED, "update installed; reboot to run it");
    return ESP_OK;
}

void ota_upload_abort(void)
{
    if (!s_up) return;
    esp_ota_abort(s_up);
    s_up = 0;
    s_busy = false;
    s_writing = false;
    set_phase(OTA_FAILED, "upload interrupted");
}

/* ------------------------------------------------------------------ public */

/* Periodic check, WiFi only in practice.
 *
 * It only looks. Whether to install is the operator's call, asked on the dial
 * -- updating a transmitter's control head unattended is not something an
 * update mechanism gets to decide, and neither is staging an image that then
 * takes over at the next power cycle without anyone having said yes.
 *
 * With one exception, on purpose: the second chip's firmware, looked up with
 * the same check (companion_look()), is fetched and sent to the chip without
 * asking. It is not this chip's, it never transmits, and it goes only while
 * no headset is connected and the radio is idle; the chip checks its
 * signature against the firmware it runs, keeps the one before until the new
 * one has started and talked to the knob, and goes back to it by itself if
 * the new one does not settle (bt_link.h, companion/main/upd.c). An interval
 * of 0 stops that look too. */
static esp_timer_handle_t s_periodic;
static volatile bool      s_due;

/* Only marks the check as due: the caller starts it when the radio is idle
 * and internal RAM has room, which this timer cannot know. */
static void periodic_cb(void *arg)
{
    (void)arg;
    s_due = true;
}

bool ota_check_due(void)  { return s_due; }
void ota_clear_due(void)  { s_due = false; }

esp_err_t ota_set_interval(uint32_t hours)
{
    s_hours = hours;
    if (s_periodic) {
        esp_timer_stop(s_periodic);
        esp_timer_delete(s_periodic);
        s_periodic = NULL;
    }
    if (!hours) return ESP_OK;          /* 0 disables it */
    const esp_timer_create_args_t a = { .callback = periodic_cb,
                                        .name = "otachk" };
    esp_err_t err = esp_timer_create(&a, &s_periodic);
    if (err != ESP_OK) return err;
    return esp_timer_start_periodic(s_periodic,
                                    (uint64_t)hours * 3600ULL * 1000000ULL);
}

esp_err_t ota_start_switch(const char *radio)
{
    if (!radio || !*radio || strlen(radio) >= sizeof s_switch) return ESP_ERR_INVALID_ARG;
    /* The second chip's firmware: no radio's, never this chip's. */
    if (strcmp(radio, COMP) == 0) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    strlcpy(s_switch, radio, sizeof s_switch);
    const esp_err_t e = ota_start_check(true);
    if (e != ESP_OK) s_switch[0] = 0;
    return e;
}

esp_err_t ota_fetch_version(const char *radio, char *ver, size_t cap)
{
    if (!radio || !ver || !cap) return ESP_ERR_INVALID_ARG;
    char base[128];
    snprintf(base, sizeof base, "%s%s/", OTA_ROOT_URL, radio);
    const size_t bcap = 1024;
    char *body = heap_caps_malloc(bcap, MALLOC_CAP_SPIRAM);
    if (!body) return ESP_ERR_NO_MEM;
    int len = 0;
    esp_err_t e = fetch_latest(NULL, base, body, bcap, &len);
    if (e == ESP_OK && !json_string_field(body, "version", ver, cap)) e = ESP_ERR_INVALID_RESPONSE;
    free(body);
    return e;
}

esp_err_t ota_fetch_index(char *buf, size_t cap)
{
    if (!buf || cap < 2) return ESP_ERR_INVALID_ARG;
    int len = 0;
    const esp_err_t e = fetch_url(OTA_ROOT_URL "index.json", buf, cap, &len);
    if (e == ESP_OK && sdc_mount()) {
        sdc_index_save(buf);              /* for when there is no server */
        sdc_unmount();
    }
    return e;
}

esp_err_t ota_card_index(char *buf, size_t cap)
{
    if (!buf || cap < 2) return ESP_ERR_INVALID_ARG;
    if (!sdc_mount()) return ESP_ERR_NOT_FOUND;
    const esp_err_t e = sdc_index_load(buf, cap);
    sdc_unmount();
    return e;
}

bool ota_card_has(const char *radio, char *ver, size_t cap)
{
    if (!radio || !*radio || !sdc_mount()) return false;
    const bool have = card_has(radio, NULL, ver, cap);
    sdc_unmount();
    return have;
}

esp_err_t ota_cache(const char *radio, volatile bool *stop)
{
    if (!radio || !*radio) return ESP_ERR_INVALID_ARG;
    char base[128], url[256], file[96] = "", sha[72] = "", ver[32] = "";
    snprintf(base, sizeof base, "%s%s/", OTA_ROOT_URL, radio);
    const size_t cap = 1024;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!body) return ESP_ERR_NO_MEM;
    int len = 0;
    esp_err_t e = fetch_latest(NULL, base, body, cap, &len);
    if (e == ESP_OK && (!json_string_field(body, "file", file, sizeof file) ||
                        !json_string_field(body, "sha256", sha, sizeof sha)))
        e = ESP_ERR_INVALID_RESPONSE;
    json_string_field(body, "version", ver, sizeof ver);
    if (e == ESP_OK && !sdc_mount()) e = ESP_ERR_NOT_FOUND;
    if (e == ESP_OK) {
        const bool have = card_has(radio, sha, NULL, 0);
        sdc_unmount();                    /* not through the download: its RAM is TLS's */
        if (have) {
            ESP_LOGI(TAG, "SD card: %s %s, already", radio, ver);
        } else {
            snprintf(url, sizeof url, "%s%s", base, file);
            const int64_t t = esp_timer_get_time();
            e = download_to_card(url, radio, sha, body, stop);
            ESP_LOGI(TAG, "SD card: %s %s %s in %lld s", radio, ver,
                     e == ESP_OK ? "fetched" : esp_err_to_name(e),
                     (long long)((esp_timer_get_time() - t) / 1000000));
        }
    }
    free(body);
    return e;
}

const char *ota_base_url(void) { return s_base; }
const char *ota_root_url(void) { return OTA_ROOT_URL; }

esp_err_t ota_init(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(s_base, sizeof s_base, "%s%s/", OTA_ROOT_URL, ota_radio());
    strlcpy(s_st.running, d->version, sizeof s_st.running);
    strlcpy(s_st.message, "idle", sizeof s_st.message);
    return ESP_OK;
}

void ota_mark_valid(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(p, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        ESP_LOGW(TAG, "update confirmed healthy; rollback cancelled");
}

esp_err_t ota_start_check(bool install)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_busy = true;
    s_st.percent = 0;
    /* TLS needs a lot of stack, and this runs on core 0 with the rest of the
     * networking so it cannot disturb the knob or the display. */
    if (install) {
        s_writing = true;                 /* the second chip's update steps aside from here */
        if (xTaskCreatePinnedToCore(ota_install_task, "ota", 8192, NULL, 4,
                                    NULL, 0) != pdPASS) {
            s_writing = false;
            s_busy = false;
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }
    if (!worker()) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    xTaskNotify(s_chk, JOB_CHECK, eSetBits);
    return ESP_OK;
}

/* ------------------------------------------------ the second chip's, public */

void ota_companion_offer(ota_comp_offer_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_offer;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t ota_companion_card_look(void)
{
    char *m = heap_caps_malloc(COMP_MANIFEST, MALLOC_CAP_SPIRAM);
    if (!m) return ESP_ERR_NO_MEM;
    esp_err_t e = ESP_ERR_NOT_FOUND;
    if (sdc_mount()) {
        if (card_has(COMP, NULL, NULL, 0)) e = sdc_manifest(COMP, m, COMP_MANIFEST);
        sdc_unmount();
    }
    ota_comp_offer_t o;
    if (e == ESP_OK && !offer_parse(m, &o)) {
        ESP_LOGW(TAG, "second chip: the SD card's COMPANIO.JSN is not a second-chip manifest");
        e = ESP_ERR_INVALID_RESPONSE;
    }
    if (e == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        if (!s_offer.known || s_offer.from_card) {
            o.from_card = true;
            o.route     = s_offer.route;
            o.looked    = s_offer.looked;
            s_offer     = o;
            memcpy(s_offer_json, m, COMP_MANIFEST);
        }
        portEXIT_CRITICAL(&s_lock);
    }
    free(m);
    return e;
}

esp_err_t ota_companion_fetch(bool network, bool in_session)
{
    portENTER_CRITICAL(&s_lock);
    const bool known = s_offer.known;
    const bool ok    = known && !s_comp_busy && !s_comp_img && !s_writing;
    if (ok) {
        s_comp_busy    = true;
        s_comp_stop    = false;
        s_comp_network = network;
        s_comp_session = in_session;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!known) return ESP_ERR_NOT_FOUND;
    if (!ok) return ESP_ERR_INVALID_STATE;
    if (!worker()) {
        s_comp_busy = false;
        return ESP_ERR_NO_MEM;
    }
    xTaskNotify(s_chk, JOB_COMPANION, eSetBits);
    return ESP_OK;
}

bool ota_companion_busy(void) { return s_comp_busy; }

bool ota_companion_stop(const char *why)
{
    if (!s_comp_busy) return false;
    s_comp_stop_why = why ? why : "stopped";
    s_comp_stop     = true;
    return true;
}

uint8_t *ota_companion_take(size_t *len, uint8_t sha256[32])
{
    portENTER_CRITICAL(&s_lock);
    uint8_t *img = s_comp_img;
    if (img) {
        if (len) *len = s_comp_len;
        if (sha256) memcpy(sha256, s_comp_sha, sizeof s_comp_sha);
    }
    s_comp_img = NULL;
    portEXIT_CRITICAL(&s_lock);
    return img;
}

bool ota_writing(void) { return s_writing; }

void ota_set_flash_hook(void (*before)(void)) { s_flash_hook = before; }
