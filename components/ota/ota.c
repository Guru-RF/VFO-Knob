#include "ota.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ota";

#ifndef OTA_REPO
#define OTA_REPO "Guru-RF/VFO-Knob"
#endif
#define OTA_LATEST_URL "https://api.github.com/repos/" OTA_REPO "/releases/latest"
/* The firmware asset is found by suffix rather than by an exact name, so the
 * release can carry several images without this needing to know the scheme. */
#define OTA_ASSET_SUFFIX ".bin"

static ota_status_t    s_st = { .phase = OTA_IDLE };
static portMUX_TYPE    s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool   s_busy;

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

static bool find_asset_url(const char *json, char *out, size_t len)
{
    const char *p = json;
    while ((p = strstr(p, "\"browser_download_url\"")) != NULL) {
        char url[256];
        if (!json_string_field(p, "browser_download_url", url, sizeof url)) return false;
        size_t n = strlen(url), s = strlen(OTA_ASSET_SUFFIX);
        if (n > s && strcmp(url + n - s, OTA_ASSET_SUFFIX) == 0) {
            strlcpy(out, url, len);
            return true;
        }
        p += 22;
    }
    return false;
}

/* ------------------------------------------------------------- the worker */

static esp_err_t fetch_latest(char *body, size_t cap, int *out_len)
{
    esp_http_client_config_t c = {
        .url               = OTA_LATEST_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return ESP_ERR_NO_MEM;
    /* GitHub rejects requests without one, and the API version header keeps
     * the response shape stable. */
    esp_http_client_set_header(h, "User-Agent", "VFO-Knob");
    esp_http_client_set_header(h, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_open(h, 0);
    if (err != ESP_OK) goto out;
    esp_http_client_fetch_headers(h);
    if (esp_http_client_get_status_code(h) != 200) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto out;
    }
    int n = esp_http_client_read_response(h, body, cap - 1);
    if (n <= 0) { err = ESP_FAIL; goto out; }
    body[n] = 0;
    *out_len = n;
out:
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return err;
}

static void ota_task(void *arg)
{
    const bool install = (bool)(intptr_t)arg;

    /* The feed is tens of kB and only two fields are wanted, so it is read
     * into PSRAM rather than competing for internal RAM with the TLS session. */
    const size_t cap = 24 * 1024;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!body) body = malloc(cap);
    if (!body) { set_phase(OTA_FAILED, "out of memory"); goto done; }

    set_phase(OTA_CHECKING, "checking GitHub for a newer release");
    int len = 0;
    if (fetch_latest(body, cap, &len) != ESP_OK) {
        set_phase(OTA_FAILED, "could not reach the release feed");
        goto done;
    }

    char tag[32] = { 0 };
    if (!json_string_field(body, "tag_name", tag, sizeof tag)) {
        set_phase(OTA_FAILED, "no tag_name in the release feed");
        goto done;
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_st.available, tag, sizeof s_st.available);
    portEXIT_CRITICAL(&s_lock);

    if (!is_newer(tag, s_st.running)) {
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

    char url[256];
    if (!find_asset_url(body, url, sizeof url)) {
        set_phase(OTA_FAILED, "release has no " OTA_ASSET_SUFFIX " asset");
        goto done;
    }
    free(body);
    body = NULL;                      /* the TLS session wants the room back */

    set_phase(OTA_DOWNLOADING, "downloading update");
    esp_http_client_config_t hc = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t oc = { .http_config = &hc };

    esp_https_ota_handle_t oh = NULL;
    if (esp_https_ota_begin(&oc, &oh) != ESP_OK || !oh) {
        set_phase(OTA_FAILED, "could not start the download");
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
    set_phase(OTA_DONE_REBOOT_NEEDED, "update installed; reboot to run it");

done:
    free(body);
    s_busy = false;
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------- upload */

static esp_ota_handle_t       s_up;
static const esp_partition_t *s_up_part;
static size_t                 s_up_written;

esp_err_t ota_upload_begin(void)
{
    if (s_busy || s_up) return ESP_ERR_INVALID_STATE;
    s_up_part = esp_ota_get_next_update_partition(NULL);
    if (!s_up_part) return ESP_ERR_NOT_FOUND;
    s_up_written = 0;
    esp_err_t err = esp_ota_begin(s_up_part, OTA_WITH_SEQUENTIAL_WRITES, &s_up);
    if (err != ESP_OK) { s_up = 0; return err; }
    s_busy = true;
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
        set_phase(OTA_FAILED, err == ESP_ERR_OTA_VALIDATE_FAILED
                                  ? "rejected: bad signature or bad image"
                                  : "rejected: image did not validate");
        return err;
    }
    err = esp_ota_set_boot_partition(s_up_part);
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
    set_phase(OTA_FAILED, "upload interrupted");
}

/* ------------------------------------------------------------------ public */

esp_err_t ota_init(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
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
    if (xTaskCreatePinnedToCore(ota_task, "ota", 8192,
                                (void *)(intptr_t)install, 4, NULL, 0)
        != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
