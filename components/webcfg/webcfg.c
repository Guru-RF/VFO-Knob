#include "webcfg.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "audio_out.h"
#include "net_prov.h"
#include "ota.h"
#include "ptt_fsm.h"
#include "tci_client.h"
#include "ui.h"
#include "usb_net.h"

static const char *TAG = "webcfg";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_srv;

/* ------------------------------------------------------------------ auth */

/* HTTP Basic, default admin/admin.
 *
 * Basic sends the password in the clear, which is honest about what this is:
 * a guard against a curious housemate on the same LAN, not against an attacker
 * on the path. TLS on the device would need a certificate the owner cannot
 * meaningfully verify, so it would buy warm feelings rather than security.
 * What matters more is that the page refuses to stop nagging while the
 * credentials are still the shipped ones. */
static bool authorized(httpd_req_t *r)
{
    char hdr[128];
    if (httpd_req_get_hdr_value_str(r, "Authorization", hdr, sizeof hdr) != ESP_OK)
        return false;
    if (strncmp(hdr, "Basic ", 6) != 0) return false;

    unsigned char dec[96];
    size_t n = 0;
    if (mbedtls_base64_decode(dec, sizeof dec - 1, &n,
                              (const unsigned char *)hdr + 6,
                              strlen(hdr + 6)) != 0)
        return false;
    dec[n] = 0;

    char want[64];
    int w = snprintf(want, sizeof want, "%s:%s",
                     net_prov_web_user(), net_prov_web_pass());
    if (w <= 0 || w >= (int)sizeof want) return false;
    return strcmp((char *)dec, want) == 0;
}

static esp_err_t deny(httpd_req_t *r)
{
    httpd_resp_set_status(r, "401 Unauthorized");
    httpd_resp_set_hdr(r, "WWW-Authenticate", "Basic realm=\"VFO-Knob\"");
    return httpd_resp_sendstr(r, "authentication required");
}

#define REQUIRE_AUTH(r) do { if (!authorized(r)) return deny(r); } while (0)

/* --------------------------------------------------------------- helpers */

static esp_err_t send_json(httpd_req_t *r, const char *json)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, json);
}

/* Address of a netif by its key, or "" if it has none. */
static void netif_addr(const char *key, char *out, size_t len)
{
    out[0] = 0;
    esp_netif_t *n = esp_netif_get_handle_from_ifkey(key);
    esp_netif_ip_info_t ip;
    if (n && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr)
        snprintf(out, len, IPSTR, IP2STR(&ip.ip));
}

/* ---------------------------------------------------------------- status */

static esp_err_t status_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    tci_status_t st;
    tci_get_status(&st);

    audio_stats_t a;
    audio_out_stats(&a);

    static const char *L[] = { "DOWN", "CONNECTING", "GREETING", "READY",
                               "DEGRADED" };
    const char *link = st.link < 5 ? L[st.link] : "?";

    char usb_ip[20], wifi_ip[20];
    netif_addr("ETH_DEF", usb_ip,  sizeof usb_ip);   /* the USB netif */
    netif_addr("WIFI_STA_DEF", wifi_ip, sizeof wifi_ip);

    const esp_app_desc_t *app = esp_app_get_description();

    /* The S-meter is a float and LVGL's snprintf cannot do %f, but this one is
     * newlib's and can -- still, keep it integral so the page never shows a
     * literal "f" the way the display once did. */
    char buf[1100];
    int n = snprintf(buf, sizeof buf,
        "{\"version\":\"%s\",\"uptime_s\":%lld,"
        "\"link\":\"%s\",\"freq\":%lld,\"mode\":\"%s\","
        "\"filt_lo\":%ld,\"filt_hi\":%ld,\"smeter\":%d,\"ptt\":\"%s\","
        "\"transport\":\"%s\",\"usb_ip\":\"%s\",\"wifi_ip\":\"%s\","
        "\"sends\":%u,\"echoes\":%u,\"connects\":%u,\"closes\":%u,"
        "\"rejects\":%u,\"reconciles\":%u,"
        "\"aud_frames\":%u,\"aud_dropped\":%u,"
        "\"heap_internal\":%u,\"heap_psram\":%u,\"boots\":%u}",
        app->version,
        (long long)(esp_timer_get_time() / 1000000),
        link, (long long)st.f_display, st.mode,
        (long)st.filt_lo, (long)st.filt_hi, (int)st.smeter_dbm,
        ptt_state_name((ptt_state_t)st.ptt_state),
        usb_ip[0] ? "USB cable" : (wifi_ip[0] ? "WiFi" : "offline"),
        usb_ip, wifi_ip,
        (unsigned)st.sends, (unsigned)st.echoes,
        (unsigned)st.connects, (unsigned)st.closes,
        (unsigned)st.rejects, (unsigned)st.reconciles,
        (unsigned)a.frames, (unsigned)a.dropped,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)net_prov_boot_count());
    if (n < 0 || n >= (int)sizeof buf) return httpd_resp_send_500(r);
    return send_json(r, buf);
}

/* ---------------------------------------------------------------- config */

static esp_err_t config_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    const vfo_cfg_t *c = net_prov_cfg();
    char buf[320];
    /* The password is deliberately not returned. The page sends one only when
     * the field is non-empty, so a save does not have to round-trip it. */
    snprintf(buf, sizeof buf,
             "{\"host\":\"%s\",\"port\":%u,\"ssid\":\"%s\","
             "\"vol\":%u,\"mic\":%u,\"tot\":%u,"
             "\"user\":\"%s\",\"defaultpw\":%s,\"otah\":%u,"
             "\"fwbase\":\"%s\"}",
             c->tci_host, (unsigned)c->tci_port, c->ssid,
             (unsigned)net_prov_volume(), (unsigned)net_prov_mic_gain(),
             (unsigned)net_prov_tot_s(),
             net_prov_web_user(),
             net_prov_web_is_default() ? "true" : "false",
             (unsigned)net_prov_ota_hours(), ota_base_url());
    return send_json(r, buf);
}

/* Reads one form field, URL-decoded, or leaves `out` untouched if absent. */
static bool field(const char *body, const char *key, char *out, size_t len)
{
    char raw[128];
    if (httpd_query_key_value(body, key, raw, sizeof raw) != ESP_OK) return false;
    /* httpd_query_key_value does not decode; do the two cases that matter for
     * an SSID or a passphrase. */
    size_t o = 0;
    for (size_t i = 0; raw[i] && o + 1 < len; i++) {
        if (raw[i] == '+') { out[o++] = ' '; }
        else if (raw[i] == '%' && raw[i + 1] && raw[i + 2]) {
            char h[3] = { raw[i + 1], raw[i + 2], 0 };
            out[o++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else out[o++] = raw[i];
    }
    out[o] = 0;
    return true;
}

static bool field_num(const char *body, const char *key, long *out)
{
    char v[24];
    if (!field(body, key, v, sizeof v) || !v[0]) return false;
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v) return false;
    *out = n;
    return true;
}

static long clampl(long v, long lo, long hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static esp_err_t config_post(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[512];
    int total = r->content_len;
    if (total <= 0 || total >= (int)sizeof body) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body size");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int k = httpd_req_recv(r, body + got, total - got);
        if (k <= 0) return ESP_FAIL;
        got += k;
    }
    body[got] = 0;

    vfo_cfg_t cfg = *net_prov_cfg();
    field(body, "host", cfg.tci_host, sizeof cfg.tci_host);
    field(body, "ssid", cfg.ssid,     sizeof cfg.ssid);
    field(body, "pass", cfg.pass,     sizeof cfg.pass);   /* absent = unchanged */

    long v;
    if (field_num(body, "port", &v)) cfg.tci_port = (uint16_t)clampl(v, 1, 65535);
    if (net_prov_save_cfg(&cfg) != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
        return ESP_FAIL;
    }

    /* Bounded the same way the PTT FSM is: a time-out outside this range is
     * either useless or not a time-out at all. */
    if (field_num(body, "tot", &v)) net_prov_save_tot((uint16_t)clampl(v, 30, 600));
    if (field_num(body, "otah", &v)) {
        net_prov_save_ota_hours((uint16_t)clampl(v, 0, 720));
        ota_set_interval(net_prov_ota_hours());
    }

    uint8_t vol = net_prov_volume(), mic = net_prov_mic_gain();
    if (field_num(body, "vol", &v)) vol = (uint8_t)clampl(v, 0, 100);
    if (field_num(body, "mic", &v)) mic = (uint8_t)clampl(v, 0, 100);
    /* Credentials last: changing them invalidates the browser's cached
     * Authorization for the NEXT request, so everything else must already be
     * committed by the time that happens. */
    char user[24] = { 0 }, pass[33] = { 0 };
    bool got_user = field(body, "user", user, sizeof user);
    bool got_pass = field(body, "webpass", pass, sizeof pass);
    if ((got_user && user[0]) || (got_pass && pass[0]))
        net_prov_save_web(got_user ? user : NULL, got_pass ? pass : NULL);

    net_prov_save_audio(vol, mic);
    ui_set_levels(vol, mic);      /* audio is the one thing that applies live */

    ESP_LOGI(TAG, "config saved: host=%s:%u ssid=\"%s\" vol=%u mic=%u tot=%u",
             cfg.tci_host, (unsigned)cfg.tci_port, cfg.ssid,
             (unsigned)vol, (unsigned)mic, (unsigned)net_prov_tot_s());
    return httpd_resp_sendstr(r, "ok");
}

/* ------------------------------------------------------------------- ota */

static esp_err_t ota_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    ota_status_t o;
    ota_get_status(&o);
    static const char *P[] = { "idle", "checking", "downloading",
                               "installed", "uptodate", "failed" };
    char buf[320];
    snprintf(buf, sizeof buf,
             "{\"phase\":\"%s\",\"percent\":%d,\"running\":\"%s\","
             "\"available\":\"%s\",\"message\":\"%s\"}",
             o.phase < 6 ? P[o.phase] : "?", o.percent,
             o.running, o.available, o.message);
    return send_json(r, buf);
}

static esp_err_t ota_post(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    /* ?install=1 downloads and applies; without it this only looks. */
    char q[32] = { 0 }, v[8] = { 0 };
    bool install = false;
    if (httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "install", v, sizeof v) == ESP_OK)
        install = (v[0] == '1');

    esp_err_t err = ota_start_check(install);
    if (err == ESP_ERR_INVALID_STATE)
        return httpd_resp_sendstr(r, "already running");
    if (err != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "could not start");
        return ESP_FAIL;
    }
    return httpd_resp_sendstr(r, "started");
}

static void reboot_cb(void *arg);

static esp_err_t ota_upload_post(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    esp_err_t err = ota_upload_begin();
    if (err != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "an update is already running");
        return ESP_FAIL;
    }
    /* Streamed straight to flash: the image is over 1.5 MB and there is
     * nowhere to buffer it. */
    char *buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(2048);
    if (!buf) { ota_upload_abort(); return httpd_resp_send_500(r); }

    /* Hand the whole device over to the transfer. RX audio is a continuous
     * ~96 kB/s inbound stream on the same socket and the same USB pipe, and
     * with both running the upload broke midway -- thousands of dropped audio
     * frames and a truncated image. The screen says so, and being a separate
     * screen it also puts PTT out of reach while the flash is rewritten. */
    tci_audio_suspend(true);
    ui_updating_show();

    const int total = r->content_len;
    int remaining = total;
    int last_pct = -1;
    while (remaining > 0) {
        int n = httpd_req_recv(r, buf, remaining > 2048 ? 2048 : remaining);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { free(buf); ota_upload_abort(); goto failed; }
        if (ota_upload_write(buf, n) != ESP_OK) {
            free(buf);
            ota_upload_abort();
            httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
            goto failed_sent;
        }
        remaining -= n;
        const int pct = total > 0 ? (int)((int64_t)(total - remaining) * 100 / total) : 0;
        if (pct != last_pct) { last_pct = pct; ui_updating_progress(pct); }
    }
    free(buf);

    if (ota_upload_end() != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                            "image rejected -- wrong signature or not a "
                            "VFO-Knob build");
        goto failed_sent;
    }

    /* An upload is something an operator is standing over, unlike the
     * background check, so finishing the job is what they expect. */
    ui_updating_result(true, "Restarting");
    httpd_resp_sendstr(r, "installed");
    const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "otaboot" };
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 1500 * 1000);
    return ESP_OK;

failed:
    httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "transfer interrupted");
failed_sent:
    ui_updating_result(false, "Update failed");
    /* Leave the message up briefly, then give the dial back. */
    vTaskDelay(pdMS_TO_TICKS(2500));
    ui_updating_hide();
    tci_audio_suspend(false);
    return ESP_FAIL;
}

/* ---------------------------------------------------------------- reboot */

static void reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t reboot_post(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    httpd_resp_sendstr(r, "rebooting");
    /* Answer first, then restart from a timer, so the browser sees the reply
     * rather than a dropped connection. */
    const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "wcreboot" };
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 300 * 1000);
    return ESP_OK;
}

/* ------------------------------------------------------------------ page */

static esp_err_t root_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, index_html_start,
                           index_html_end - index_html_start - 1);
}

esp_err_t webcfg_start(void)
{
    if (s_srv) return ESP_OK;

    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port      = 80;
    c.lru_purge_enable = true;
    c.max_uri_handlers = 9;
    c.stack_size       = 4608;
    /* Below LVGL and the knob: a page refresh must never cost a detent. */
    c.task_priority    = 3;
    c.core_id          = 0;

    esp_err_t err = httpd_start(&s_srv, &c);
    if (err != ESP_OK) {
        s_srv = NULL;
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = root_get },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/config",  .method = HTTP_GET,  .handler = config_get },
        { .uri = "/api/config",  .method = HTTP_POST, .handler = config_post },
        { .uri = "/api/ota",     .method = HTTP_GET,  .handler = ota_get },
        { .uri = "/api/ota",     .method = HTTP_POST, .handler = ota_post },
        { .uri = "/api/ota/upload", .method = HTTP_POST, .handler = ota_upload_post },
        { .uri = "/api/reboot",  .method = HTTP_POST, .handler = reboot_post },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++)
        httpd_register_uri_handler(s_srv, &uris[i]);

    ESP_LOGI(TAG, "configuration page on http://<device>/ (port 80)");
    return ESP_OK;
}
