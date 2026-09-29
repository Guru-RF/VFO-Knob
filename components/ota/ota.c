#include "ota.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"

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

/* ------------------------------------------------------------- the worker */

static esp_err_t fetch_latest(char *body, size_t cap, int *out_len)
{
    char url[160];
    snprintf(url, sizeof url, "%smanifest.json", s_base);
    esp_http_client_config_t c = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 8000,        /* a few hundred bytes; do not linger */
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return ESP_ERR_NO_MEM;
    /* raw.githubusercontent.com rejects requests with no User-Agent. */
    esp_http_client_set_header(h, "User-Agent", "VFO-Knob");

    esp_err_t err = esp_http_client_open(h, 0);
    if (err != ESP_OK) goto out;
    esp_http_client_fetch_headers(h);
    const int status = esp_http_client_get_status_code(h);
    if (status != 200) {
        err = status == 404 ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_RESPONSE;
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

static void ota_run(bool install)
{
    /* The manifest is a few hundred bytes; 2 kB is generous. PSRAM: a check
     * may run beside a live TCI session, and anything under 16 kB would
     * otherwise be taken from internal RAM first. */
    const size_t cap = 2048;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!body) body = malloc(cap);
    if (!body) { set_phase(OTA_FAILED, "out of memory"); goto done; }

    set_phase(OTA_CHECKING, "checking for a newer release");
    int len = 0;
    const esp_err_t got = fetch_latest(body, cap, &len);
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

    if (!newer) {
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
    snprintf(url, sizeof url, "%s%s", s_base, file);
    free(body);
    body = NULL;                      /* the TLS session wants the room back */

    set_phase(OTA_DOWNLOADING, "downloading update");
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
        !same_product(nd.project_name, esp_app_get_description()->project_name)) {
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
    set_phase(OTA_DONE_REBOOT_NEEDED, "update installed; reboot to run it");

done:
    free(body);
    portENTER_CRITICAL(&s_lock);
    s_st.checks++;
    portEXIT_CRITICAL(&s_lock);
    s_busy = false;
}

/* Installing writes flash, and a task whose stack is in PSRAM must not, so an
 * install gets its own internal-stack task for the one run. */
static void ota_install_task(void *arg)
{
    (void)arg;
    ota_run(true);
    vTaskDelete(NULL);
}

/* Looking only runs on one worker, made once and never deleted: its stack in
 * PSRAM, its control block static and internal. Never deleted, because
 * deleting a task with a PSRAM stack makes IDF allocate a helper task in
 * internal RAM -- and abort() if it cannot, which is exactly the resource a
 * check beside a live TCI session is short of. */
static StaticTask_t s_chk_tcb;
static TaskHandle_t s_chk;

static void ota_check_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ota_run(false);
    }
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
        set_phase(OTA_FAILED, err == ESP_ERR_OTA_VALIDATE_FAILED
                                  ? "rejected: bad signature or bad image"
                                  : "rejected: image did not validate");
        return err;
    }
    if (!upload_is_wanted()) return ESP_ERR_NOT_SUPPORTED;
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

/* Periodic check, WiFi only in practice.
 *
 * It only looks. Whether to install is the operator's call, asked on the dial
 * -- updating a transmitter's control head unattended is not something an
 * update mechanism gets to decide, and neither is staging an image that then
 * takes over at the next power cycle without anyone having said yes. */
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
        if (xTaskCreatePinnedToCore(ota_install_task, "ota", 8192, NULL, 4,
                                    NULL, 0) != pdPASS) {
            s_busy = false;
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }
    if (!s_chk) {
        /* IDF's StackType_t is a byte, so the depth is in bytes. */
        StackType_t *stack = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
        if (stack)
            s_chk = xTaskCreateStaticPinnedToCore(ota_check_task, "otachk",
                                                  8192, NULL, 4, stack,
                                                  &s_chk_tcb, 0);
        if (!s_chk) {
            heap_caps_free(stack);
            s_busy = false;
            return ESP_ERR_NO_MEM;
        }
    }
    xTaskNotifyGive(s_chk);
    return ESP_OK;
}
