#include "webcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

#include "audio_in.h"
#include "audio_out.h"
#include "board.h"
#include "bt_level.h"
#include "bt_link.h"
#include "net_prov.h"
#include "ota.h"
#include "ptt_fsm.h"
#include "radio.h"
#if VFO_HAS_SDR || VFO_RADIO_KIWI
#include "kiwi_mark.h"
#include "sdr_rx.h"
#endif
#if VFO_RADIO_UBERSDR
#include "uber.h"
#endif
/* The kiwi firmware's receivers are the web SDRs' list, one in use. */
#if VFO_RADIO_KIWI
#include "kiwi.h"
#include "kiwi_sess.h"
#endif
#include "ui.h"
#include "usb_net.h"
#include "esp_task_wdt.h"

static const char *TAG = "webcfg";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char radio_html_start[] asm("_binary_radio_html_start");
extern const char radio_html_end[]   asm("_binary_radio_html_end");

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

/* The address the browser is talking to us from, and whether that is the far
 * end of the USB cable. The server listens on an IPv6 socket, so an IPv4
 * client arrives as ::ffff:a.b.c.d.
 *
 * The cable case matters because the address is then 10.55.42.2 -- where the
 * knob already finds AetherSDR by itself over USB, and which is unreachable
 * from WiFi, the only transport the configured host is used for. */
static void client_addr(httpd_req_t *r, char *out, size_t len, bool *on_usb)
{
    out[0]  = 0;
    *on_usb = false;

    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (getpeername(httpd_req_to_sockfd(r), (struct sockaddr *)&ss, &sl) != 0)
        return;

    struct in_addr a;
    if (ss.ss_family == AF_INET) {
        a = ((struct sockaddr_in *)&ss)->sin_addr;
    } else if (ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)&ss;
        if (!IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) return;
        memcpy(&a.s_addr, &s6->sin6_addr.s6_addr[12], 4);
    } else {
        return;
    }
    inet_ntoa_r(a, out, len);

    esp_netif_t *n = esp_netif_get_handle_from_ifkey("ETH_DEF");   /* USB */
    esp_netif_ip_info_t ip;
    if (n && esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr)
        *on_usb = (a.s_addr & ip.netmask.addr) == (ip.ip.addr & ip.netmask.addr);
}

/* A receiver's name on the pages (kiwi_rx_label, sdr_rx_label): what the
 * dial calls it, with room for more than the dial's 15 bytes -- a long name,
 * antenna or address shows whole. */
#define PAGE_LABEL 48

/* For text from elsewhere -- a reflector's talkgroup names -- going into JSON:
 * quotes and backslashes escaped, control characters dropped. */
static void json_esc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 2 < cap; in++) {
        const unsigned char c = (unsigned char)*in;
        if (c < 0x20) continue;
        if (c == '"' || c == '\\') out[o++] = '\\';
        out[o++] = (char)c;
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------- status */

static esp_err_t status_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    radio_status_t st;
    radio_get_status(&st);

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
    /* A reflector's talkgroup and who is on it, where a radio has a
     * frequency: the page shows those instead. */
    char tgname[72], talker[40];
    json_esc(st.tg_name, tgname, sizeof tgname);
    json_esc(st.talker[0] ? st.talker : st.last_talker, talker, sizeof talker);
    /* A second receiver and the antennas, where the radio has them -- by
     * their own names where the radio names them. */
    char ant[12] = "";
    if (st.n_ant && st.have_ant && !radio_list_item(st.ant_names, st.ant, ant, sizeof ant))
        snprintf(ant, sizeof ant, "ANT%u%s", st.ant + 1u, st.ant_rx ? "+RX" : "");
    /* The knob's own power (board.h): on USB or not -- null while its first
     * readings settle -- the rail as last read, mV (-1 none), and the charge
     * the face shows on the battery (-1 on USB, where it cannot be read). */
    board_power_t pw;
    board_power_get(&pw);

    char buf[1300];
    int n = snprintf(buf, sizeof buf,
        "{\"version\":\"%s\",\"uptime_s\":%lld,"
        "\"link\":\"%s\",\"freq\":%lld,\"mode\":\"%s\","
        "\"filt_lo\":%ld,\"filt_hi\":%ld,\"smeter\":%d,\"ptt\":\"%s\","
        "\"transport\":\"%s\",\"usb_ip\":\"%s\",\"wifi_ip\":\"%s\","
        "\"sends\":%u,\"echoes\":%u,\"connects\":%u,\"closes\":%u,"
        "\"rejects\":%u,\"reconciles\":%u,"
        "\"aud_frames\":%u,\"aud_dropped\":%u,"
        "\"heap_internal\":%u,\"heap_psram\":%u,\"boots\":%u,"
        "\"reflector\":%s,\"tg\":%lu,\"tgname\":\"%s\",\"talker\":\"%s\","
        "\"talking\":%s,\"rx\":\"%s\",\"ant\":\"%s\",\"vol\":%u,"
        "\"on_usb\":%s,\"rail_mv\":%d,\"batt\":%d}",
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
        (unsigned)net_prov_boot_count(),
        st.reflector ? "true" : "false", (unsigned long)st.tg, tgname, talker,
        st.talker[0] ? "true" : "false",
        st.n_rx > 1 ? (st.rx ? "SUB" : "MAIN") : "", ant, (unsigned)net_prov_volume(),
        pw.src == KNOB_PWR_USB ? "true" : pw.src == KNOB_PWR_BATTERY ? "false" : "null", (int)pw.mv,
        (int)pw.pct);
    if (n < 0 || n >= (int)sizeof buf) return httpd_resp_send_500(r);
    return send_json(r, buf);
}

/* ---------------------------------------------------------------- config */

static esp_err_t config_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    const vfo_cfg_t *c = net_prov_cfg();
    char client[16];
    bool client_usb;
    client_addr(r, client, sizeof client, &client_usb);

    char buf[640];
    /* The password is deliberately not returned. The page sends one only when
     * the field is non-empty, so a save does not have to round-trip it. */
    int n = snprintf(buf, sizeof buf,
             "{\"host\":\"%s\",\"port\":%u,\"ssid\":\"%s\","
             "\"vol\":%u,\"mic\":%u,\"mich\":%u,"
             "\"user\":\"%s\",\"defaultpw\":%s,\"otah\":%u,\"dim\":%u,\"blank\":%u,"
             "\"radio\":\"%s\",\"fwbase\":\"%s\",\"fwroot\":\"%s\","
             "\"ruser\":\"%s\",\"rpass\":%s,\"link\":\"%s\","
             "\"client\":\"%s\",\"client_usb\":%s}",
             c->radio_host, (unsigned)c->radio_port, c->ssid,
             (unsigned)net_prov_volume(), (unsigned)net_prov_mic_gain(),
             (unsigned)net_prov_mic_gain_headset(),
             net_prov_web_user(),
             net_prov_web_is_default() ? "true" : "false",
             (unsigned)net_prov_ota_hours(), (unsigned)net_prov_dim_min(),
             (unsigned)net_prov_blank_min(), ota_radio(), ota_base_url(),
             ota_root_url(), c->radio_user, c->radio_pass[0] ? "true" : "false",
             radio_link_name(), client, client_usb ? "true" : "false");
    if (n < 0 || n >= (int)sizeof buf) return httpd_resp_send_500(r);
    return send_json(r, buf);
}

/* Reads one form field, URL-decoded: ESP_OK; else, `out` untouched,
 * ESP_ERR_NOT_FOUND when it is absent and ESP_ERR_HTTPD_RESULT_TRUNC when it
 * is too long to read whole. */
static esp_err_t field_e(const char *body, const char *key, char *out, size_t len)
{
    char raw[128];
    const esp_err_t e = httpd_query_key_value(body, key, raw, sizeof raw);
    if (e != ESP_OK) return e;
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
    return ESP_OK;
}

/* Reads one form field, URL-decoded, or leaves `out` untouched if absent. */
static bool field(const char *body, const char *key, char *out, size_t len)
{
    return field_e(body, key, out, len) == ESP_OK;
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
    field(body, "host", cfg.radio_host, sizeof cfg.radio_host);
    field(body, "ssid", cfg.ssid,     sizeof cfg.ssid);
    field(body, "pass", cfg.pass,     sizeof cfg.pass);   /* absent = unchanged */
    /* The radio's own login, for radios that have one. Like the WiFi
     * passphrase, the password is only ever written: absent = unchanged. */
    field(body, "ruser", cfg.radio_user, sizeof cfg.radio_user);
    field(body, "rpass", cfg.radio_pass, sizeof cfg.radio_pass);

    long v;
    if (field_num(body, "port", &v)) cfg.radio_port = (uint16_t)clampl(v, 1, 65535);
    if (net_prov_save_cfg(&cfg) != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
        return ESP_FAIL;
    }

    {   /* Both stages are written together so one cannot be validated against
         * a stale copy of the other. */
        long dim = net_prov_dim_min(), blank = net_prov_blank_min();
        bool touched = field_num(body, "dim", &dim);
        touched |= field_num(body, "blank", &blank);
        if (touched) {
            net_prov_save_dim((uint16_t)clampl(dim, 0, 1440),
                              (uint16_t)clampl(blank, 0, 1440));
            ui_dim_set_minutes(net_prov_dim_min(), net_prov_blank_min());
        }
    }
    if (field_num(body, "otah", &v)) {
        net_prov_save_ota_hours((uint16_t)clampl(v, 0, 720));
        ota_set_interval(net_prov_ota_hours());
    }

    uint8_t vol = net_prov_volume(), mic = net_prov_mic_gain(), mich = net_prov_mic_gain_headset();
    if (field_num(body, "vol", &v)) vol = (uint8_t)clampl(v, 0, 100);
    /* Up to 200%, as on the dial: the PDM element is quiet. Clamping to 100
     * here used to halve a gain set on the dial whenever the page was saved. */
    if (field_num(body, "mic", &v)) mic = (uint8_t)clampl(v, 0, 200);
    if (field_num(body, "mich", &v)) mich = (uint8_t)clampl(v, 0, 200);
    /* Credentials last: changing them invalidates the browser's cached
     * Authorization for the NEXT request, so everything else must already be
     * committed by the time that happens. */
    char user[24] = { 0 }, pass[33] = { 0 };
    bool got_user = field(body, "user", user, sizeof user);
    bool got_pass = field(body, "webpass", pass, sizeof pass);
    if ((got_user && user[0]) || (got_pass && pass[0]))
        net_prov_save_web(got_user ? user : NULL, got_pass ? pass : NULL);

    net_prov_save_audio(vol, mic, mich);
    /* The gain in use: a headset's while one is connected. */
    const uint8_t live = bt_link_headset_connected() ? mich : mic;
    ui_set_levels(vol, live);     /* audio is the one thing that applies live */
    audio_out_set_volume(vol);    /* directly too: with no display, no UI task */
    audio_in_set_gain(live);

    ESP_LOGI(TAG, "config saved: host=%s:%u ssid=\"%s\" vol=%u mic=%u",
             cfg.radio_host, (unsigned)cfg.radio_port, cfg.ssid,
             (unsigned)vol, (unsigned)mic);
    return httpd_resp_sendstr(r, "ok");
}

/* ----------------------------------------------------------- radio's own */

/* A radio client with settings of its own -- the svxconnect firmware's
 * reflector station -- brings endpoints for them. The others have none. */
__attribute__((weak)) size_t radio_web_endpoints(const httpd_uri_t **out)
{
    *out = NULL;
    return 0;
}

/* Behind the page's login, like everything else here. */
static esp_err_t radio_endpoint(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    const httpd_uri_t *u = r->user_ctx;
    r->user_ctx = u->user_ctx;
    return u->handler(r);
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

/* Six consecutive recv timeouts at the server's 5 s wait: half a minute of
 * silence from a client that is supposed to be streaming 1.7 MB. */
#define kUploadStalls 6

static void reboot_cb(void *arg);

/* The stored core dump, raw, for `esp-coredump info_corefile -t raw` against
 * the ELF of the build that crashed. The USB build has no console, and flash
 * is out of reach once TinyUSB owns the pads, so this is the only way to read
 * one out. It holds task stacks, so it sits behind the login like the rest. */
static esp_err_t coredump_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "no core dump stored");
        return ESP_FAIL;
    }
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Content-Disposition",
                       "attachment; filename=\"coredump.bin\"");
    static char buf[1024];            /* the server runs one handler at a time */
    for (size_t off = 0; off < size; ) {
        size_t n = size - off < sizeof buf ? size - off : sizeof buf;
        if (esp_flash_read(NULL, buf, addr + off, n) != ESP_OK ||
            httpd_resp_send_chunk(r, buf, n) != ESP_OK) {
            httpd_resp_send_chunk(r, NULL, 0);
            return ESP_FAIL;
        }
        off += n;
    }
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* ?radio=<name> is the page switching the knob to that radio's firmware, on
 * purpose. Without it, an upload is an update and must be this radio's own
 * firmware. A name is a short run of [a-z0-9]; anything else is refused
 * rather than guessed at, and so is "companion": the second chip's firmware
 * goes over the link (POST /api/bt/update), never into this chip's flash.
 *
 * Parsed in a function of its own, into a static: the upload's deepest point,
 * the RSA check at the end, leaves this task very little stack, and anything
 * the handler itself keeps is on it the whole way down. */
static char s_up_radio[16];               /* the server runs one handler at a time */

static __attribute__((noinline)) bool upload_radio(httpd_req_t *r)
{
    char q[48] = { 0 };
    s_up_radio[0] = 0;
    if (httpd_req_get_url_query_str(r, q, sizeof q) != ESP_OK) return true;
    const esp_err_t e = httpd_query_key_value(q, "radio", s_up_radio,
                                              sizeof s_up_radio);
    bool ok = e == ESP_OK || e == ESP_ERR_NOT_FOUND;       /* not: truncated */
    for (const char *p = s_up_radio; ok && *p; p++)
        ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
    if (e != ESP_OK) s_up_radio[0] = 0;
    return ok && strcmp(s_up_radio, "companion") != 0;
}

static esp_err_t ota_upload_run(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    if (!upload_radio(r)) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "no such radio");
        return ESP_FAIL;
    }
    if (ota_busy()) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "an update is already running");
        return ESP_FAIL;
    }
    /* Streamed straight to flash: the image is over 1.5 MB and there is
     * nowhere to buffer it. */
    char *buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(2048);
    if (!buf) return httpd_resp_send_500(r);

    /* Hand the whole device over to the transfer. RX audio is a continuous
     * ~96 kB/s inbound stream on the same socket and the same USB pipe, and
     * with both running the upload broke midway -- thousands of dropped audio
     * frames and a truncated image. The screen says so, and being a separate
     * screen it also puts PTT out of reach while the flash is rewritten. It
     * goes up before the slot is erased, which takes a few seconds. */
    radio_audio_suspend(true);
    ui_updating_show();

    const int total = r->content_len;
    const int64_t t_begin = esp_timer_get_time();
    esp_err_t err = ota_upload_begin(s_up_radio[0] ? s_up_radio : NULL,
                                     total > 0 ? (size_t)total : 0);
    if (err != ESP_OK) {
        free(buf);
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                            err == ESP_ERR_INVALID_SIZE
                                ? "image too large for the update slot"
                                : "could not start the update");
        goto failed_sent;
    }
    /* Each core's idle time across the transfer, for the log at the end. */
    const int64_t  t_xfer = esp_timer_get_time();
    const uint32_t idle0  = ulTaskGetIdleRunTimeCounterForCore(0);
    const uint32_t idle1  = ulTaskGetIdleRunTimeCounterForCore(1);

    int remaining = total;
    int last_pct = -1;
    int stalls = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(r, buf, remaining > 2048 ? 2048 : remaining);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            /* Bounded, because an unbounded retry here is not a stall -- it
             * is a permanent wedge. esp_http_server serves from a small
             * socket pool, so a handler that never returns takes the whole
             * configuration page down with it, and the only way back is the
             * log port's reboot command. That happened: a client that went
             * away mid-upload left this loop spinning on timeouts forever. */
            if (++stalls > kUploadStalls) {
                free(buf);
                ota_upload_abort();
                goto failed;
            }
            continue;
        }
        stalls = 0;
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

    err = ota_upload_end();
    /* Past the deepest point: how close it came to this task's stack, and
     * how much room the cores had while the flash was written. */
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t_xfer) | 1;
    ESP_LOGI(TAG, "upload: erase %u ms, transfer %u ms, core 0/1 idle %u%%/%u%%; "
             "web server stack %u bytes never used",
             (unsigned)((t_xfer - t_begin) / 1000), (unsigned)(us / 1000),
             (unsigned)((uint64_t)(ulTaskGetIdleRunTimeCounterForCore(0) - idle0) * 100 / us),
             (unsigned)((uint64_t)(ulTaskGetIdleRunTimeCounterForCore(1) - idle1) * 100 / us),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (err == ESP_ERR_NOT_SUPPORTED) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
                            s_up_radio[0] ? "image rejected -- not that "
                                            "radio's firmware"
                                          : "image rejected -- that is another "
                                            "radio's firmware; switch radios "
                                            "under Firmware to install it");
        goto failed_sent;
    }
    if (err != ESP_OK) {
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
    radio_audio_suspend(false);
    return ESP_FAIL;
}

/* The task watchdog, relaxed while an upload runs. Its flash erase and writes
 * stall the other core while the display redraws the progress ring, and with
 * a radio session keeping both cores busy besides, core 1's idle task went
 * 5 s without running: the watchdog reset the knob two seconds into an
 * upload. The upload has its own stall timeout (kUploadStalls); the watchdog
 * gets half a minute while it runs, and its usual 5 s back after. */
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
static void upload_watchdog(bool relaxed)
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

static esp_err_t ota_upload_post(httpd_req_t *r)
{
    upload_watchdog(true);
    const esp_err_t e = ota_upload_run(r);
    upload_watchdog(false);
    return e;
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
    /* An operator asking for a restart has been using this image, so confirm
     * it first. A new image restarted before its 20 s "boot looks healthy"
     * mark counts as a failed boot, and the bootloader goes back to the old
     * one -- which is what saving the radio's settings and rebooting straight
     * after an update did. */
    ota_mark_valid();
    httpd_resp_sendstr(r, "rebooting");
    /* Answer first, then restart from a timer, so the browser sees the reply
     * rather than a dropped connection. */
    const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "wcreboot" };
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 300 * 1000);
    return ESP_OK;
}

/* ------------------------------------------------------------------ page */

#define PORTAL_URIS 3
/* ------------------------------------------------------------ the portal */

/* WiFi setup, for a phone on the knob's own hotspot -- the setup firmware's,
 * and any firmware's with none of its networks in reach: open, since the
 * phone has nothing to log in with yet, and there only while the hotspot is
 * up. On the network the knob then joins, it is gone and the page is the
 * usual one, behind its login. */
extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[]   asm("_binary_portal_html_end");

static esp_err_t scan_get(httpd_req_t *r)
{
    if (!net_prov_ap_active()) return httpd_resp_send_404(r);
    enum { MAXN = 20 };
    net_prov_net_t *nets = heap_caps_calloc(MAXN, sizeof *nets, MALLOC_CAP_SPIRAM);
    char *buf = heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    if (!nets || !buf) {
        free(nets);
        free(buf);
        return httpd_resp_send_500(r);
    }
    const int n = net_prov_scan(nets, MAXN);
    size_t o = 0;
    buf[o++] = '[';
    for (int i = 0; i < n && o < 2900; i++) {
        char e[70];
        json_esc(nets[i].ssid, e, sizeof e);
        o += snprintf(buf + o, 3072 - o, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                      i ? "," : "", e, nets[i].rssi, nets[i].open ? "true" : "false");
    }
    buf[o++] = ']';
    buf[o] = 0;
    const esp_err_t err = send_json(r, buf);
    free(nets);
    free(buf);
    return err;
}

static esp_err_t join_post(httpd_req_t *r)
{
    if (!net_prov_ap_active()) return httpd_resp_send_404(r);
    char body[256];
    const int total = r->content_len;
    if (total <= 0 || total >= (int)sizeof body) return httpd_resp_send_500(r);
    int got = 0;
    while (got < total) {
        const int k = httpd_req_recv(r, body + got, total - got);
        if (k <= 0) return ESP_FAIL;
        got += k;
    }
    body[got] = 0;
    char ssid[33] = "", pass[65] = "";
    field(body, "ssid", ssid, sizeof ssid);
    field(body, "pass", pass, sizeof pass);
    if (!ssid[0]) return httpd_resp_send_500(r);
    net_prov_join(ssid, pass);
    return httpd_resp_sendstr(r, "joining");
}

static esp_err_t join_get(httpd_req_t *r)
{
    if (!net_prov_ap_active()) return httpd_resp_send_404(r);
    static const char *ST[] = { "idle", "trying", "ok", "failed" };
    char ssid[33], why[48], es[70], ew[100], buf[240];
    const net_join_t s = net_prov_join_state(ssid, sizeof ssid, why, sizeof why);
    json_esc(ssid, es, sizeof es);
    json_esc(why, ew, sizeof ew);
    /* The page says what comes next: the firmware list on the setup
     * firmware's dial, or the radio, carrying on. */
#if VFO_RADIO_SETUP
    static const char *const after = "true";
#else
    static const char *const after = "false";
#endif
    snprintf(buf, sizeof buf, "{\"state\":\"%s\",\"ssid\":\"%s\",\"why\":\"%s\",\"setup\":%s}",
             ST[s], es, ew, after);
    return send_json(r, buf);
}

/* Anything else, while the hotspot is up, is a phone asking whether it is
 * online (generate_204, hotspot-detect.html, connecttest.txt): sent to the
 * portal, which is what makes it open the page by itself. */
static esp_err_t portal_404(httpd_req_t *r, httpd_err_code_t err)
{
    (void)err;
    if (!net_prov_ap_active()) return httpd_resp_send_404(r);
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    /* iOS wants a body, not only the redirect, to see a portal. */
    return httpd_resp_sendstr(r, "The knob's WiFi setup");
}

/* ------------------------------------------------------------ the WiFi list */

#define WIFI_URIS 2

/* The networks the knob knows, the one joined last first; whether each has a
 * password, never the password. */
static esp_err_t wifi_get_h(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char buf[640], e[70];
    size_t o = (size_t)snprintf(buf, sizeof buf, "{\"list\":[");
    for (int i = 0; i < net_prov_wifi_count() && o < sizeof buf - 100; i++) {
        net_wifi_t w;
        if (!net_prov_wifi_get(i, &w)) break;
        json_esc(w.ssid, e, sizeof e);
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s{\"ssid\":\"%s\",\"pass\":%s}",
                              i ? "," : "", e, w.pass[0] ? "true" : "false");
    }
    json_esc(net_prov_wifi_now(), e, sizeof e);
    snprintf(buf + o, sizeof buf - o, "],\"now\":\"%s\",\"max\":%d}", e, NET_PROV_WIFIS);
    return send_json(r, buf);
}

/* n, then ssidN and passN for each: a password not in the form is the one the
 * knob has for that network, as everywhere on the page. */
static esp_err_t wifi_post_h(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    EXT_RAM_BSS_ATTR static char body[1536];
    EXT_RAM_BSS_ATTR static net_wifi_t list[NET_PROV_WIFIS];
    const int total = r->content_len;
    if (total <= 0 || total >= (int)sizeof body) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body");
    int got = 0;
    while (got < total) {
        const int k = httpd_req_recv(r, body + got, total - got);
        if (k <= 0) return ESP_FAIL;
        got += k;
    }
    body[got] = 0;
    long n = 0;
    field_num(body, "n", &n);
    n = clampl(n, 0, NET_PROV_WIFIS);
    int k = 0;
    for (int i = 0; i < n; i++) {
        char key[8];
        net_wifi_t w = { 0 };
        snprintf(key, sizeof key, "ssid%d", i);
        if (!field(body, key, w.ssid, sizeof w.ssid) || !w.ssid[0]) continue;
        snprintf(key, sizeof key, "pass%d", i);
        if (!field(body, key, w.pass, sizeof w.pass)) {
            for (int j = 0; j < net_prov_wifi_count(); j++) {
                net_wifi_t old;
                if (net_prov_wifi_get(j, &old) && !strcmp(old.ssid, w.ssid)) {
                    strlcpy(w.pass, old.pass, sizeof w.pass);
                    break;
                }
            }
        }
        list[k++] = w;
    }
    if (net_prov_wifis_save(list, k) != ESP_OK) return httpd_resp_send_500(r);
    return send_json(r, "{\"ok\":true}");
}

#if !VFO_RADIO_SETUP && !VFO_RADIO_SVXCONNECT && !VFO_RADIO_PHONE && !VFO_RADIO_KIWI
/* ------------------------------------------------------------- the radios
 *
 * The radios the knob knows, one in use (see net_prov.h):
 *
 *   GET  /api/radios          {"sel":0,"list":[{"name","host","port","user",
 *                             "pass":true}, ...]} -- passwords only as set
 *   POST /api/radios          n=, then name0 host0 port0 user0 pass0 was0 ...,
 *                             and use= (the one in use, in the new list): the
 *                             list, saved. A password left out is the one the
 *                             radio `was` had, at the same address. The radio
 *                             in use changes on the next boot.
 *   POST /api/radios/switch   to=N: that one in use now -- the knob restarts
 *                             into it, as a swipe up does. Not on the air.
 */
#define RADIOS_URIS 3

/* The one in use, counting the configured radios first and then those the
 * client found itself (radio.h: SmartLink's). */
static int radios_sel(void)
{
    const int f = radio_found_active();
    return f >= 0 ? net_prov_radio_count() + f : net_prov_radio_active();
}

/* ,"radios":{"sel":0,"names":[...],"via":[...]} -- for the radio's JSON:
 * every radio, and how it is reached ("LAN", "SmartLink"): the configured
 * ones and those the client found on the LAN, then the others. */
static size_t radios_names_json(char *j, size_t cap)
{
    const int nd = net_prov_radio_count(), nf = radio_found_count(), nl = nd + radio_found_lan();
    int o = snprintf(j, cap, ",\"radios\":{\"sel\":%d,\"names\":[", radios_sel());
    for (int i = 0; i < nd + nf && o > 0 && (size_t)o < cap; i++) {
        static net_radio_t r;
        char nm[24] = "", n[68];
        if (i < nd) {
            if (!net_prov_radio_get(i, &r)) break;
            strlcpy(nm, r.name[0] ? r.name : r.host[0] ? net_prov_host_shown(r.host) : "NO ADDRESS",
                    sizeof nm);
        } else if (!radio_found_get(i - nd, nm, sizeof nm)) {
            break;
        }
        json_esc(nm, n, sizeof n);
        o += snprintf(j + o, cap - o, "%s\"%s\"", i ? "," : "", n);
    }
    if (o > 0 && (size_t)o < cap) o += snprintf(j + o, cap - o, "],\"via\":[");
    for (int i = 0; i < nd + nf && o > 0 && (size_t)o < cap; i++)
        o += snprintf(j + o, cap - o, "%s\"%s\"", i ? "," : "", i < nl ? "LAN" : radio_found_via());
    if (o > 0 && (size_t)o < cap) o += snprintf(j + o, cap - o, "]}");
    return o > 0 && (size_t)o < cap ? (size_t)o : 0;
}

static esp_err_t radios_get_h(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    EXT_RAM_BSS_ATTR static char j[1600];
    /* sel is -1 while a radio the client found (SmartLink) is in use. */
    int o = snprintf(j, sizeof j, "{\"sel\":%d,\"list\":[",
                     radio_found_active() >= 0 ? -1 : net_prov_radio_active());
    for (int i = 0; i < net_prov_radio_count() && (size_t)o < sizeof j; i++) {
        EXT_RAM_BSS_ATTR static net_radio_t e;
        if (!net_prov_radio_get(i, &e)) break;
        char n[52], h[132], u[70];
        json_esc(e.name, n, sizeof n);
        json_esc(e.host, h, sizeof h);
        json_esc(e.user, u, sizeof u);
        o += snprintf(j + o, sizeof j - o, "%s{\"name\":\"%s\",\"host\":\"%s\",\"port\":%u,"
                      "\"user\":\"%s\",\"pass\":%s}", i ? "," : "", n, h, (unsigned)e.port,
                      u, e.pass[0] ? "true" : "false");
    }
    if ((size_t)o >= sizeof j - 4) return httpd_resp_send_500(r);
    snprintf(j + o, sizeof j - o, "]}");
    return send_json(r, j);
}

static esp_err_t radios_post_h(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    EXT_RAM_BSS_ATTR static char body[1800];
    EXT_RAM_BSS_ATTR static net_radio_t list[NET_PROV_RADIOS], old;
    const int total = r->content_len;
    if (total <= 0 || total >= (int)sizeof body) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body size");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        const int k = httpd_req_recv(r, body + got, total - got);
        if (k <= 0) return ESP_FAIL;
        got += k;
    }
    body[got] = 0;
    long n = 0, use = -1;
    if (!field_num(body, "n", &n) || n < 1 || n > NET_PROV_RADIOS) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "n: 1 to 4 radios");
        return ESP_FAIL;
    }
    field_num(body, "use", &use);
    /* use=-1: none chosen here -- the one in use stays, configured or
     * found; otherwise that configured one, and a found one is given up. */
    const int cur = net_prov_radio_active();
    int k = 0, in_use = 0;
    for (int i = 0; i < n; i++) {
        net_radio_t *e = &list[k];
        char key[12], v[96] = "";
        memset(e, 0, sizeof *e);
        snprintf(key, sizeof key, "host%d", i);
        if (!field(body, key, v, sizeof v)) continue;
        /* "http://host:port/" and "host:port" too, as for the SDRs. An
         * UberSDR keeps its scheme with the name: https:// is TLS and http://
         * in the clear, whatever the port -- a name alone is TLS on 443 only,
         * as knobs kept it before. */
        const char *s = strstr(v, "://");
        const char *a = v + strspn(v, " ");
#if VFO_RADIO_UBERSDR
        const char *scheme = !s ? "" : !strncasecmp(a, "https://", 8) ? "https://"
                           : !strncasecmp(a, "http://", 7) ? "http://" : "";
#else
        const char *scheme = "";
#endif
        s = s ? s + 3 : a;
        const size_t hl = strcspn(s, ":/ ");
        if (!hl || strlen(scheme) + hl >= sizeof e->host) continue;
        snprintf(e->host, sizeof e->host, "%s%.*s", scheme, (int)hl, s);
        long port = 0;
        snprintf(key, sizeof key, "port%d", i);
        if (field_num(body, key, &port) && port > 0 && port < 65536) e->port = (uint16_t)port;
        else if (s[hl] == ':') e->port = (uint16_t)clampl(strtol(s + hl + 1, NULL, 10), 1, 65535);
        else if (scheme[0]) e->port = scheme[4] == 's' ? 443 : 80;
        else e->port = net_prov_cfg()->radio_port;
        snprintf(key, sizeof key, "name%d", i);
        field(body, key, e->name, sizeof e->name);
        snprintf(key, sizeof key, "user%d", i);
        field(body, key, e->user, sizeof e->user);
        /* A password not sent is the one it had -- at the same address only,
         * so a radio moved elsewhere never gets the old one's. */
        long was = -1;
        snprintf(key, sizeof key, "was%d", i);
        field_num(body, key, &was);
        const bool had = was >= 0 && net_prov_radio_get((int)was, &old) &&
                         !strcasecmp(old.host, e->host) && old.port == e->port;
        snprintf(key, sizeof key, "pass%d", i);
        if (!field(body, key, e->pass, sizeof e->pass) && had)
            strlcpy(e->pass, old.pass, sizeof e->pass);
        if (use >= 0 ? i == use : was == cur) in_use = k;
        k++;
    }
    if (!k) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "no radio with an address");
        return ESP_FAIL;
    }
    if (net_prov_radios_save(list, k, in_use) != ESP_OK) return httpd_resp_send_500(r);
    if (use >= 0) radio_found_use(-1);
    return radios_get_h(r);
}

static void switch_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t radios_switch_h(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char q[48] = "";
    const int total = r->content_len;
    if (total > 0 && total < (int)sizeof q) {
        int got = 0;
        while (got < total) {
            const int k = httpd_req_recv(r, q + got, total - got);
            if (k <= 0) return ESP_FAIL;
            got += k;
        }
        q[got] = 0;
    } else if (total > 0 || httpd_req_get_url_query_str(r, q, sizeof q) != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "to=N");
        return ESP_FAIL;
    }
    long to = -1;
    static net_radio_t e;
    const int nd = net_prov_radio_count();
    char name[24] = "";
    /* to counts the configured radios, then those the client found. */
    if (!field_num(q, "to", &to) ||
        (to < nd ? !net_prov_radio_get((int)to, &e) : !radio_found_get((int)to - nd, name, sizeof name))) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "to=N: a radio in the list");
        return ESP_FAIL;
    }
    if (to < nd) strlcpy(name, e.name[0] ? e.name : net_prov_host_shown(e.host), sizeof name);
    if (to == radios_sel()) return httpd_resp_sendstr(r, "already in use");
    if (radio_on_air()) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "the radio is transmitting");
    }
    ESP_LOGW(TAG, "web: switching to %s (%s)", name, to < nd ? e.host : radio_found_via());
    /* As the dial's: the image confirmed and the boot counted healthy first,
     * then the restart, after the answer has gone. */
    ota_mark_valid();
    net_prov_boot_ok();
    esp_err_t err;
    if (to < nd) {
        err = radio_found_use(-1);
        if (err == ESP_OK) err = net_prov_radio_activate((int)to);
    } else {
        err = radio_found_use((int)to - nd);
    }
    if (err != ESP_OK) return httpd_resp_send_500(r);
    ui_switching(name);
    httpd_resp_sendstr(r, "switching");
    const esp_timer_create_args_t a = { .callback = switch_cb, .name = "wcswitch" };
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 1200 * 1000);
    return ESP_OK;
}
#elif VFO_RADIO_KIWI
/* The kiwi firmware's receivers are the web SDRs' list below, and another is
 * taken over live, with no restart: the radio page switches with receiver=. */
#define RADIOS_URIS 0

/* ,"radios":{"sel":0,"live":true,"names":[...],"via":[...]} -- for the
 * radio's JSON: the receivers by their names on the dial, at more length. */
static size_t radios_names_json(char *j, size_t cap)
{
    EXT_RAM_BSS_ATTR static char nm[PAGE_LABEL], e[2 * PAGE_LABEL];    /* this task's stack is tight */
    const int n = sdr_count();
    int o = snprintf(j, cap, ",\"radios\":{\"sel\":%d,\"live\":true,\"names\":[", kiwi_rx_active());
    for (int i = 0; i < n && o > 0 && (size_t)o < cap; i++) {
        nm[0] = 0;
        kiwi_rx_label(i, nm, sizeof nm);
        json_esc(nm, e, sizeof e);
        o += snprintf(j + o, cap - o, "%s\"%s\"", i ? "," : "", e);
    }
    if (o > 0 && (size_t)o < cap) o += snprintf(j + o, cap - o, "],\"via\":[");
    for (int i = 0; i < n && o > 0 && (size_t)o < cap; i++)
        o += snprintf(j + o, cap - o, "%s\"Kiwi\"", i ? "," : "");
    if (o > 0 && (size_t)o < cap) o += snprintf(j + o, cap - o, "]}");
    return o > 0 && (size_t)o < cap ? (size_t)o : 0;
}
#else
#define RADIOS_URIS 0
#endif

#if VFO_HAS_SDR || VFO_RADIO_KIWI
/* ------------------------------------------------------------- web SDRs
 *
 *   GET  /api/sdr        the receivers (their passwords only as set or not,
 *                        each with its name on the dial, at more length than
 *                        the dial has room for: label), the one listened to
 *                        and how that goes, the balance, and who their
 *                        owners see (ident)
 *   POST /api/sdr        n=, then name0 host0 pass0 ipl0 was0, name1 ... : the
 *                        list, saved. host as the page takes it: host:port
 *                        (8073 for a host alone), or an http:// or https://
 *                        link whole -- https:// is TLS, 443 unless it says. A
 *                        password left out is the one receiver `was` had, at
 *                        the same address -- or at the one an https://
 *                        redirect moved it from, the move kept; empty is
 *                        none.
 *                        sel= (local, or 0-3) and balance= (-100..100) too:
 *                        sel= is the operator choosing, even the receiver
 *                        already chosen (sdr_rx_choose). ident= on its own or
 *                        with the list: who the owners see, "" for VFO-Knob
 *                        -- told to a receiver logged in at once, no list
 *                        saved and no session ended for it.
 *   POST /api/sdr/test   host pass ipl was: one receiver tried -- reached,
 *                        what it calls itself, its users, the password, and
 *                        a day-limit mark; never a login to a marked one. An
 *                        http:// one's redirect to https:// on its own host is
 *                        followed (tls, port: where it was spoken -- said
 *                        with an error after it too) and kept for the
 *                        receiver at that address in the list
 *
 * Each receiver says whether it is spoken to over TLS (tls: an https://
 * address), and its day-limit mark, where it has one. The one listened
 * to says where it tunes, once it has (range, Hz; 0 0 not yet): its state
 * "out of range" is a dial it cannot reach, and the pages say what it
 * covers. On the kiwi firmware these are its receivers: sel= is the one in
 * use, taken over at once, and each says what holds it back until chosen
 * again (hold); the right ear is "right" (its sel, state, streaming, smeter,
 * range) and sdr= (off, or 0-3) chooses it, balance= mixes the two. One
 * receiver is never in both ears: a choice of the other ear's is refused,
 * 409; a place not in the list, 400, and nothing saved.
 */
#define SDR_URIS 3

#if VFO_RADIO_KIWI
/* A receiver's place, as receiver=, sdr= and sel= give it, not among the
 * `count` there are: answered 400, and true -- nothing is to be done. */
static bool no_such_rx(httpd_req_t *r, long i, int count)
{
    if (i >= 0 && i < count) return false;
    char msg[64];
    if (count > 0) snprintf(msg, sizeof msg, "no receiver %ld: they go from 0 to %d", i, count - 1);
    else           snprintf(msg, sizeof msg, "no receivers yet: the configuration page adds them");
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, msg);
    return true;
}
#endif

/* ,"sdr":{...} -- for the radio's JSON, and without the key for /api/sdr. */
static size_t sdr_json(char *j, size_t cap)
{
    char st[48];
    /* Static: this task serves one request at a time, on a tight stack. */
    EXT_RAM_BSS_ATTR static char who[2 * KIWI_IDENT_MAX], id[KIWI_IDENT_MAX], lb[PAGE_LABEL], l[2 * PAGE_LABEL];
    sdr_ident(id, sizeof id);
    json_esc(id, who, sizeof who);
#if VFO_RADIO_KIWI
    EXT_RAM_BSS_ATTR static kiwi_info_t ki;      /* this task's stack is tight */
    EXT_RAM_BSS_ATTR static sdr_status_t rs;
    kiwi_info(&ki);
    sdr_rx_status(&rs);
    json_esc(ki.state, st, sizeof st);
    char rst[48];
    json_esc(rs.state, rst, sizeof rst);
    int o = snprintf(j, cap, ",\"sdr\":{\"sel\":%d,\"state\":\"%s\",\"streaming\":%s,\"ident\":\"%s\","
                     "\"right\":{\"sel\":%d,\"state\":\"%s\",\"streaming\":%s,\"smeter\":%.1f,\"range\":[%lld,%lld]},"
                     "\"balance\":%d,\"list\":[",
                     kiwi_rx_active(), st, radio_is_ready() ? "true" : "false", who,
                     sdr_rx_selected(), rst, rs.streaming ? "true" : "false", (double)rs.smeter_dbm,
                     (long long)rs.lo_hz, (long long)rs.hi_hz, sdr_rx_balance());
#else
    EXT_RAM_BSS_ATTR static sdr_status_t s;
    sdr_rx_status(&s);
    json_esc(s.state, st, sizeof st);
    int o = snprintf(j, cap, ",\"sdr\":{\"sel\":%d,\"state\":\"%s\",\"streaming\":%s,"
                     "\"smeter\":%.1f,\"range\":[%lld,%lld],\"balance\":%d,\"ident\":\"%s\",\"list\":[",
                     sdr_rx_selected(), st, s.streaming ? "true" : "false",
                     (double)s.smeter_dbm, (long long)s.lo_hz, (long long)s.hi_hz, sdr_rx_balance(), who);
#endif
    for (int i = 0; i < sdr_count() && o > 0 && (size_t)o < cap; i++) {
        sdr_cfg_t c;
        if (!sdr_get(i, &c)) break;
        char n[52], h[132];
        json_esc(c.name, n, sizeof n);
        json_esc(c.host, h, sizeof h);
        /* Its name on the dial, whole here: the page's, else what it says it
         * is, else its address. */
        lb[0] = 0;
#if VFO_RADIO_KIWI
        kiwi_rx_label(i, lb, sizeof lb);
#else
        sdr_rx_label(i, lb, sizeof lb);
#endif
        json_esc(lb, l, sizeof l);
        o += snprintf(j + o, cap - o, "%s{\"name\":\"%s\",\"label\":\"%s\",\"host\":\"%s\",\"port\":%u,"
                      "\"tls\":%s,\"pass\":%s,\"ipl\":%s", i ? "," : "", n, l, h, (unsigned)c.port,
                      c.tls ? "true" : "false", c.pass[0] ? "true" : "false", c.ipl[0] ? "true" : "false");
        /* Its day-limit mark: the knob's refused logins, the tries left
         * when it is chosen again, whether it is held for good, and whether
         * it is only a login left unanswered. */
        kiwi_mark_t m;
        if (o > 0 && (size_t)o < cap && kiwi_mark_get(sdr_hp(&c), &m))
            o += snprintf(j + o, cap - o, ",\"mark\":{\"strikes\":%u,\"tries\":%d,\"held\":%s,\"unsure\":%s}",
                          (unsigned)m.strikes, m.strikes < KIWI_STRIKES_MAX ? KIWI_STRIKES_MAX - m.strikes : 0,
                          m.strikes >= KIWI_STRIKES_MAX ? "true" : "false",
                          m.flags & KIWI_MARK_UNSURE ? "true" : "false");
#if VFO_RADIO_KIWI
        char hw[16];
        if (o > 0 && (size_t)o < cap && kiwi_rx_hold(i, hw, sizeof hw))
            o += snprintf(j + o, cap - o, ",\"hold\":\"%s\"", hw);
#endif
        if (o > 0 && (size_t)o < cap) o += snprintf(j + o, cap - o, "}");
    }
    if (o > 0 && (size_t)o < cap) o += snprintf(j + o, cap - o, "]}");
    return o > 0 && (size_t)o < cap ? (size_t)o : 0;
}

static esp_err_t sdr_get_h(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    EXT_RAM_BSS_ATTR static char j[2048];     /* internal RAM is the scarce one */
    const size_t n = sdr_json(j, sizeof j);
    if (n < 8) return httpd_resp_send_500(r);
    return send_json(r, j + 7);                 /* past ,"sdr": */
}

/* A form body, whole, into `buf`; false, answered, if it does not fit. */
static bool recv_form(httpd_req_t *r, char *buf, size_t cap)
{
    const int total = r->content_len;
    if (total < 0 || total >= (int)cap) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body size");
        return false;
    }
    int got = 0;
    while (got < total) {
        const int k = httpd_req_recv(r, buf + got, total - got);
        if (k <= 0) return false;
        got += k;
    }
    buf[got] = 0;
    return true;
}

/* A receiver's fields from a form, their names ending in `sfx` ("0".."3", or
 * "" for the test): its address as the page takes it (sdr_parse_addr) --
 * host:port, or an http:// or https:// link. A password not in the form is
 * the one it had -- only at the same address, so a receiver moved elsewhere
 * never gets the old one's; and at the same address it is known by the port
 * it was, where an https:// redirect moved it (its kport). A page loaded
 * before such a move shows the address it had: that is the same receiver
 * too, and it stays https:// (sdr_same). */
static bool sdr_from_form(const char *body, const char *sfx, sdr_cfg_t *c, long *was_out)
{
    char key[12], hp[96] = "";
    memset(c, 0, sizeof *c);
    snprintf(key, sizeof key, "host%s", sfx);
    if (!field(body, key, hp, sizeof hp) || !sdr_parse_addr(hp, c)) return false;
    snprintf(key, sizeof key, "name%s", sfx);
    field(body, key, c->name, sizeof c->name);
    long was = -1;
    snprintf(key, sizeof key, "was%s", sfx);
    field_num(body, key, &was);
    if (was_out) *was_out = was;
    EXT_RAM_BSS_ATTR static sdr_cfg_t old;       /* this task's stack is tight */
    const bool had = was >= 0 && sdr_get((int)was, &old) && sdr_same(&old, c);
    snprintf(key, sizeof key, "pass%s", sfx);
    if (!field(body, key, c->pass, sizeof c->pass) && had) strlcpy(c->pass, old.pass, sizeof c->pass);
    snprintf(key, sizeof key, "ipl%s", sfx);
    if (!field(body, key, c->ipl, sizeof c->ipl) && had) strlcpy(c->ipl, old.ipl, sizeof c->ipl);
    return true;
}

static esp_err_t sdr_post_h(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    EXT_RAM_BSS_ATTR static char body[1600];
    if (!recv_form(r, body, sizeof body)) return ESP_FAIL;
    long n;
    char v[12];
    /* Who the receivers' owners see, with the list or on its own: read whole
     * before anything is saved -- longer than the knob keeps, it is said so,
     * never dropped or cut short unseen. */
    EXT_RAM_BSS_ATTR static char who[128];
    const esp_err_t ie = field_e(body, "ident", who, sizeof who);
    if (ie == ESP_ERR_HTTPD_RESULT_TRUNC || (ie == ESP_OK && strlen(who) >= KIWI_IDENT_MAX)) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "your name for their owners is too long: 31 bytes at most");
        return ESP_FAIL;
    }
    EXT_RAM_BSS_ATTR static sdr_cfg_t list[SDR_MAX];
    const bool has_list = field_num(body, "n", &n);
    int k = 0, sel = -1;
    if (has_list) {
        const int cur = sdr_rx_selected();
        for (int i = 0; i < clampl(n, 0, SDR_MAX); i++) {
            char sfx[4];
            long was;
            snprintf(sfx, sizeof sfx, "%d", i);
            if (!sdr_from_form(body, sfx, &list[k], &was)) continue;
            if (cur >= 0 && was == cur) sel = k;      /* the one listened to, still */
            k++;
        }
    }
#if VFO_RADIO_KIWI
    /* A receiver's place that is not in the list, as it is about to be: said,
     * and nothing saved. */
    const int count = has_list ? k : sdr_count();
    if (field(body, "sel", v, sizeof v) && v[0] >= '0' && v[0] <= '9' && no_such_rx(r, strtol(v, NULL, 10), count))
        return ESP_FAIL;
    if (field(body, "sdr", v, sizeof v) && v[0] >= '0' && v[0] <= '9' && no_such_rx(r, strtol(v, NULL, 10), count))
        return ESP_FAIL;
#endif
    if (has_list) {
        /* The one listened to -- on the kiwi firmware, the right ear's --
         * kept through the reshuffle, set with the list: no choice made, so
         * a receiver held back by its limits stays held. The kiwi firmware's
         * receiver in use follows its address wherever the list puts it, by
         * itself. */
        if (sdr_save(list, k, sel) != ESP_OK) return httpd_resp_send_500(r);
        ESP_LOGI(TAG, "web: %d web SDR%s", k, k == 1 ? "" : "s");
    }
    /* The name after the list: a flash with no room for it costs the list
     * nothing, and the page hears why. */
    const bool who_lost = ie == ESP_OK && sdr_ident_save(who) != ESP_OK;
    bool refused = false;
    if (field(body, "sel", v, sizeof v) && v[0]) {
#if VFO_RADIO_KIWI
        /* In use: the operator's act, even of the one in use -- not the
         * right ear's. */
        if (v[0] >= '0' && v[0] <= '9') refused |= !kiwi_rx_use(atoi(v), true);
#else
        if (v[0] >= '0' && v[0] <= '9') sdr_rx_choose(atoi(v));
        else if (!strcasecmp(v, "local")) sdr_rx_choose(-1);
#endif
    }
#if VFO_RADIO_KIWI
    /* The right ear: off, or a receiver -- not the one in use. */
    if (field(body, "sdr", v, sizeof v) && v[0]) {
        if (v[0] >= '0' && v[0] <= '9') refused |= !sdr_rx_choose(atoi(v));
        else if (!strcasecmp(v, "off") || !strcasecmp(v, "local")) sdr_rx_choose(-1);
    }
#endif
    if (field_num(body, "balance", &n)) sdr_rx_set_balance((int8_t)clampl(n, -100, 100));
    if (who_lost) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "your name for their owners was not kept: the knob's flash has no room for it");
        return ESP_FAIL;
    }
    if (refused) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "one receiver is never in both ears: that one is in the other");
    }
    return sdr_get_h(r);
}

#if VFO_RADIO_KIWI
/* The receiver at this address in either ear, its session on its way or
 * playing. */
static bool in_either_ear(uint32_t hp) { return kiwi_rx_in_session(hp) || sdr_rx_in_session(hp); }
#endif

/* Blocks this task for the test's few seconds -- a few more over TLS, its
 * handshakes on the Test's own task (sdr_test), 40 s at most: the page
 * waits for it. */
static esp_err_t sdr_test_h(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    EXT_RAM_BSS_ATTR static char body[512], j[1024];
    EXT_RAM_BSS_ATTR static sdr_cfg_t c;
    if (!recv_form(r, body, sizeof body)) return ESP_FAIL;
    if (!sdr_from_form(body, "", &c, NULL)) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "no address");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "web: testing %s%s:%u", c.tls ? "https://" : "", c.host, (unsigned)c.port);
    /* This test's answer, never the last one's. */
    j[0] = 0;
#if VFO_RADIO_KIWI
    /* The receiver in use in either ear, its session on its way or playing,
     * answers from that: no second login beside it -- two at once, each could
     * be refused. */
    const esp_err_t e = sdr_test(&c, in_either_ear, j, sizeof j, "kiwi");
#else
    const esp_err_t e = sdr_test(&c, NULL, j, sizeof j, NULL);
#endif
    if (e != ESP_OK && !j[0]) return httpd_resp_send_500(r);
    return send_json(r, j);
}
#else
#define SDR_URIS 0
#endif

/* ------------------------------------------------------------- the radio
 *
 * With the radio connected, the page opens on its controls: everything the
 * knob can set on it, and the API behind them -- for the page, and for
 * logging programs and anything else that wants the frequency and mode, or
 * to set them with a plain URL:
 *
 *   GET /api/radio                          the state, as JSON -- with no
 *                                           link, why, as the face says it
 *                                           ("NOT FOUND"); an UberSDR's, the
 *                                           receiver of the list it is on
 *   GET /api/radio/set?freq=14074000        Hz; 14.074 (a point) is MHz
 *       ...&mode=usb&filter=2&agc=mid&gain=1&rfgain=80&power=50
 *       ...&tuner=on&squelch=30&rx=sub&ant=2&rxant=1&rit=-120&lo=100&hi=2800
 *       ...&sdr=0&balance=-30    a web SDR beside it, "local" for none (see
 *                                above: the Icom, Xiegu and FlexRadio ones)
 *       ...&receiver=1           the kiwi firmware's receiver in use, by its
 *                                place in the list: at once, and even while
 *                                the one in use is down
 *   (POST, with the same fields as a form, does the same.)
 *
 * Behind the page's login like everything here. Nothing that transmits:
 * neither PTT nor a tune cycle, on the page or in the API, and no setting at
 * all while the radio is on the air. Not for the setup firmware, which has
 * no radio, nor svxconnect's reflector. */
#define RADIO_PAGE (!VFO_RADIO_SETUP && !VFO_RADIO_SVXCONNECT && !VFO_RADIO_PHONE)

/* ------------------------------------------ Bluetooth headset or speaker */

#if !VFO_RADIO_SETUP
#define BT_URIS 3

static void bda_text(const uint8_t *b, char *s)
{
    snprintf(s, 18, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

static bool bda_parse(const char *s, uint8_t *b)
{
    unsigned v[6];
    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) b[i] = (uint8_t)v[i];
    return true;
}

static void hex8_text(const uint8_t *b, char *s)
{
    for (int i = 0; i < 8; i++) snprintf(s + 2 * i, 3, "%02x", b[i]);
}

/* A device's services, as its scan answer listed them (BTL_SVC_*):
 * "a2dp,hfp", or "" for none known. */
static void svc_text(uint8_t v, char *s, size_t cap)
{
    static const struct { uint8_t bit; const char *name; } SVC[] = {
        { BTL_SVC_A2DP, "a2dp" }, { BTL_SVC_HFP, "hfp" }, { BTL_SVC_HSP, "hsp" }, { BTL_SVC_AVRCP, "avrcp" },
    };
    size_t o = 0;
    s[0] = 0;
    for (size_t i = 0; i < sizeof SVC / sizeof SVC[0] && o < cap; i++)
        if (v & SVC[i].bit) o += (size_t)snprintf(s + o, cap - o, "%s%s", o ? "," : "", SVC[i].name);
}

/* The second chip's own firmware, for /api/bt: what it runs and says of
 * itself -- its INFO -- and its update (bt_link.h). Identities are the
 * first 8 bytes of an image's app_elf_sha256, in hex.
 *
 *   blocked    every image the knob will not send it again: the last
 *              result's, the chip's own word, the knob's record
 *   offer      the release there is for it (ota.h), from the server or the
 *              SD card, or null
 *   can_fetch  the knob fetches it by itself: false on the USB cable, and
 *              when its last check did not reach the update server -- then
 *              the page hands it over */
static int bt_update_json(char *j, size_t cap, const bt_link_status_t *st)
{
    EXT_RAM_BSS_ATTR static bt_link_upd_t    u;
    EXT_RAM_BSS_ATTR static ota_comp_offer_t o;
    bt_link_update_status(&u);
    ota_companion_offer(&o);
    static const char *const phases[]  = { "idle", "waiting", "sending", "checking", "restarting", "trial" };
    static const char *const results[] = { "", "kept", "went back", "stopped", "failed", "refused", "unknown" };
    static const char *const backs[]   = { "", "power", "quiet", "crashed", "hung", "early", "guard" };
    /* Static, as the rest of this page's: this task's stack is tight. */
    EXT_RAM_BSS_ATTR static char last[200], back[160], offer[160], blocked[64];
    char app[17] = "", to[40];
    if (u.info) hex8_text(u.chip.app_sha, app);
    json_esc(u.phase ? u.to : "", to, sizeof to);
    json_esc(u.text, last, sizeof last);
    /* Gone back for one of these, the chip never takes that image again. */
    const uint8_t c    = u.info ? u.chip.back : BTL_BACK_NONE;
    const bool    real = c == BTL_BACK_CRASHED || c == BTL_BACK_HUNG || c == BTL_BACK_EARLY || c == BTL_BACK_GUARD;
    strlcpy(back, "null", sizeof back);
    if (c) {
        char bver[17], bv[40], bs[17];
        memcpy(bver, u.chip.back_ver, 16);
        bver[16] = 0;
        json_esc(bver, bv, sizeof bv);
        hex8_text(u.chip.back_sha, bs);
        snprintf(back, sizeof back, "{\"why\":\"%s\",\"ver\":\"%s\",\"sha\":\"%s\",\"real\":%s}",
                 c < 7 ? backs[c] : "?", bv, bs, real ? "true" : "false");
    }
    const uint8_t *bl[3];
    int nb = 0;
    if (u.block) bl[nb++] = u.last_sha;
    if (real) bl[nb++] = u.chip.back_sha;
    if (u.remembered && (u.rec.result != BT_UPD_NONE || u.rec.tries >= BT_UPD_TRIES)) bl[nb++] = u.rec.sha8;
    size_t bo = 0;
    blocked[0] = 0;
    for (int i = 0; i < nb; i++) {
        bool seen = false;
        for (int k = 0; k < i; k++) seen = seen || !memcmp(bl[k], bl[i], 8);
        if (seen) continue;
        char h[17];
        hex8_text(bl[i], h);
        bo += (size_t)snprintf(blocked + bo, sizeof blocked - bo, "%s\"%s\"", bo ? "," : "", h);
    }
    strlcpy(offer, "null", sizeof offer);
    if (o.known) {
        char ov[72], os[17];
        json_esc(o.version, ov, sizeof ov);
        hex8_text(o.app_sha, os);
        snprintf(offer, sizeof offer, "{\"ver\":\"%s\",\"sha\":\"%s\",\"from\":\"%s\"}", ov, os,
                 o.from_card ? "card" : "server");
    }
    /* Before its first check the knob can fetch for itself while WiFi is
     * its way: the USB cable has no route out. */
    char wifi[20];
    netif_addr("WIFI_STA_DEF", wifi, sizeof wifi);
    const bool can_fetch = o.looked ? o.route : wifi[0] != 0;
    return snprintf(j, cap,
                    "\"proto\":%u,\"updates\":%s,\"release\":%s,\"trial\":%s,\"boot\":%u,\"app\":\"%s\","
                    "\"upd\":{\"phase\":\"%s\",\"pct\":%u,\"to\":\"%s\",\"forced\":%s,\"result\":\"%s\","
                    "\"last\":\"%s\",\"back\":%s,\"blocked\":[%s],\"offer\":%s,\"can_fetch\":%s},",
                    st->proto, (st->flags & BTL_HELLO_UPDATE) ? "true" : "false",
                    u.info && (u.chip.flags & BTL_INFO_RELEASE) ? "true" : "false",
                    u.info && u.chip.state == BTL_RUN_TRIAL ? "true" : "false", u.info ? u.chip.boot_ver : 0, app,
                    u.phase < 6 ? phases[u.phase] : "?", u.percent, to, u.forced ? "true" : "false",
                    u.result < 7 ? results[u.result] : "?", last, back, blocked, offer,
                    can_fetch ? "true" : "false");
}

/* The headset or speaker, and what the last scan found.
 *
 *   speakers   the second chip plays to speakers (its HELLO says so); one
 *              that does not knows headsets only, and a found device's kind
 *              is "" from it
 *   kind       "headset" or "speaker", the device's ("" with none);
 *   kind_why   how the chip came to it: "class" (its class and services),
 *              "drops" (it hung up a call's audio at once), "no a2dp" (then
 *              had no A2DP), "user" (this page), "" (nothing known)
 *   svc        the services its scan answer listed: "a2dp,hfp,hsp,avrcp"
 *   delay      ms a speaker plays behind the jack; 0 unless one plays
 *   av_volume  the second chip sets a speaker's own volume at all (its HELLO)
 *   volume     what a speaker connected does with the knob's VOLUME: "knob"
 *              (its own volume is the VOLUME), "asking" (it takes it, which
 *              is on its way), "own" (it keeps its own: the knob scales what
 *              it sends), "refused" (it takes its source's, but answered the
 *              knob's louder than asked, or not at all: it keeps its own, the
 *              knob scales; the next VOLUME tries again), "" (no speaker
 *              connected)
 *   battery    the device's charge as it last reported it, 0-100 %; -1 not
 *              known (none connected, nothing said yet, or a second chip
 *              whose firmware came before batteries)
 *   level      the device's level, dB, on top of the knob's VOLUME: its own
 *              (do=level), or its kind's; null with no device
 *   level_default  ...its kind's: -12 a speaker's, 0 a headset's */
static esp_err_t bt_get_h(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    /* This task's stack is tight; internal RAM is the scarce one. */
    EXT_RAM_BSS_ATTR static bt_link_status_t st;
    EXT_RAM_BSS_ATTR static btl_found_t found[16];
    EXT_RAM_BSS_ATTR static char j[4096];
    bt_link_status(&st);
    const int nf = bt_link_found(found, 16);
    char name[72], ver[72], b[18] = "", svc[32];
    static const char *const links[]  = { "idle", "connecting", "connected" };
    static const char *const audios[] = { "", "CVSD 8 kHz", "mSBC 16 kHz", "SBC 44.1 kHz" };
    static const char *const whys[]   = { "", "class", "drops", "no a2dp", "user" };
    static const uint8_t none[6];
    const bool spks = (st.flags & BTL_HELLO_SPEAKERS) != 0;
    static const char *const vols[] = { "", "own", "asking", "knob", "refused" };
    const uint8_t sv = bt_link_speaker_volume();
    json_esc(st.hs.name, name, sizeof name);
    json_esc(st.version, ver, sizeof ver);
    if (memcmp(st.hs.bda, none, 6)) bda_text(st.hs.bda, b);
    svc_text(b[0] ? st.hs.svc : 0, svc, sizeof svc);
    const char *kind = !b[0] ? "" : st.hs.kind == BTL_KIND_SPEAKER ? "speaker" : "headset";
    char lv[8] = "null", lvd[8] = "null";
    if (b[0]) {
        snprintf(lv, sizeof lv, "%d", bt_link_level(st.hs.bda, st.hs.kind, NULL));
        snprintf(lvd, sizeof lvd, "%d", bt_level_default(st.hs.kind));
    }
    int o = snprintf(j, sizeof j,
                     "{\"companion\":%s,\"version\":\"%s\",\"speakers\":%s,\"link\":\"%s\",\"audio\":\"%s\","
                     "\"name\":\"%s\",\"bda\":\"%s\",\"kind\":\"%s\",\"kind_why\":\"%s\",\"svc\":\"%s\","
                     "\"delay\":%lu,\"av_volume\":%s,\"volume\":\"%s\",\"level\":%s,\"level_default\":%s,"
                     "\"battery\":%d,\"remembered\":%s,"
                     "\"scanning\":%s,\"spk\":%u,\"mic\":%u,\"presses\":%lu,\"mic_frames\":%lu,\"boom\":%s,",
                     st.companion ? "true" : "false", ver, spks ? "true" : "false", links[st.hs.link % 3],
                     st.hs.audio < 4 ? audios[st.hs.audio] : "?", name, b, kind,
                     b[0] && st.hs.kind_why < 5 ? whys[st.hs.kind_why] : "", svc,
                     (unsigned long)bt_link_speaker_delay_ms(), (st.flags & BTL_HELLO_AV_VOLUME) ? "true" : "false",
                     sv < 5 ? vols[sv] : "", lv, lvd, bt_link_battery(), st.hs.remembered ? "true" : "false",
                     st.hs.scanning ? "true" : "false", st.hs.spk, st.hs.mic, (unsigned long)st.presses,
                     (unsigned long)st.up_frames, bt_link_boom_ptt() ? "true" : "false");
    o += bt_update_json(j + o, sizeof j - o, &st);
    o += snprintf(j + o, sizeof j - o, "\"found\":[");
    /* An entry is 200 bytes at the most: room for it and the end. */
    for (int i = 0; i < nf && o < (int)sizeof j - 256; i++) {
        json_esc(found[i].name, name, sizeof name);
        bda_text(found[i].bda, b);
        svc_text(found[i].svc, svc, sizeof svc);
        const bool audio = ((found[i].cod >> 8) & 0x1F) == 4;   /* major class: audio/video */
        o += snprintf(j + o, sizeof j - o,
                      "%s{\"bda\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"audio\":%s,\"kind\":\"%s\",\"svc\":\"%s\"}",
                      i ? "," : "", b, name, found[i].rssi, audio ? "true" : "false",
                      !spks ? "" : found[i].kind == BTL_KIND_SPEAKER ? "speaker" : "headset", svc);
    }
    snprintf(j + o, sizeof j - o, "]}");
    return send_json(r, j);
}

/* do=scan, or do=connect|forget with bda=, or do=disconnect, or do=boom with
 * on=1|0, or do=kind with bda= and kind=headset|speaker: the page's choice
 * for that device, which the second chip keeps -- and the device connected
 * is called again as that -- or do=level with bda= and level=<dB>, -24 to
 * 12 in steps of 3: that device's level, which this chip keeps, heard at
 * once if it is the one there. */
static esp_err_t bt_post_h(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char body[96], act[16] = "", bs[24] = "";
    const int total = r->content_len;
    if (total <= 0 || total >= (int)sizeof body) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body");
    int got = 0;
    while (got < total) {
        const int k = httpd_req_recv(r, body + got, total - got);
        if (k <= 0) return ESP_FAIL;
        got += k;
    }
    body[got] = 0;
    field(body, "do", act, sizeof act);
    field(body, "bda", bs, sizeof bs);
    uint8_t bda[6];
    const bool have = bda_parse(bs, bda);
    char what[12] = "";                         /* the choice, for the log */
    if      (!strcmp(act, "scan"))               bt_link_scan(10);
    else if (!strcmp(act, "connect") && have)    bt_link_connect(bda);
    else if (!strcmp(act, "disconnect"))         bt_link_disconnect();
    else if (!strcmp(act, "forget") && have)     bt_link_forget(bda);
    else if (!strcmp(act, "boom")) {
        char on[4] = "";
        field(body, "on", on, sizeof on);
        bt_link_set_boom_ptt(on[0] == '1');
    }
    else if (!strcmp(act, "kind") && have) {
        field(body, "kind", what, sizeof what);
        const bool spk = !strcmp(what, "speaker");
        if (!spk && strcmp(what, "headset"))
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "kind: headset or speaker");
        if (!bt_link_set_kind(bda, spk ? BTL_KIND_SPEAKER : BTL_KIND_HEADSET)) {
            httpd_resp_send_custom_err(r, "409 Conflict", "the second chip's firmware knows headsets only");
            return ESP_OK;
        }
    }
    else if (!strcmp(act, "level") && have) {
        /* The whole field a number, one of the steps: 3.5 or 3x is none,
         * where field_num() would take the 3. */
        char lv[24] = "", *end = lv;
        field(body, "level", lv, sizeof lv);
        const long db = strtol(lv, &end, 10);
        if (end == lv || *end || !bt_level_ok(db) || !bt_link_set_level(bda, (int)db))
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "level: -24 to +12 dB, in steps of 3");
        snprintf(what, sizeof what, "%+ld dB", db);
    }
    else return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "do what");
    ESP_LOGI(TAG, "bluetooth: %s %s%s%s", act, bs, what[0] ? " " : "", what);
    return send_json(r, "{\"ok\":true}");
}

/* The second chip's firmware, handed over from the page or a computer:
 *
 *   POST /api/bt/update[?force=1]     the body: a signed second-chip image
 *
 * Taken into PSRAM here, its form checked, and handed to bt_link, which
 * sends it at a quiet moment; the chip checks its signature. No flash is
 * written on this chip, nor any RSA done. Without force, only to a chip
 * that runs a release, an image with a release's version newer than the
 * chip's, and not one that went back on it for a real failure; force sends
 * a development build, or an older one -- the chip still refuses one it
 * went back from for real.
 *
 * Read slowly, 2 kB at a time: on the USB cable the radio's audio comes
 * down the same pipe, and must keep coming. */
#define BT_UP_PACE_MS 10                /* between reads: ~200 kB/s at the most */

static esp_err_t bt_update_refuse(httpd_req_t *r, const char *status, const char *why)
{
    ESP_LOGW(TAG, "second chip: an image from the page not taken: %s", why);
    httpd_resp_send_custom_err(r, status, why);
    /* ESP_OK: what the client still sends is read and dropped, so that it
     * sees this answer rather than a closed connection. */
    return ESP_OK;
}

/* A version as tools/release.sh gives one, "v1.18.0". The release mark
 * itself the image does not show -- only a chip running it says so -- but a
 * development build's version says more ("v1.18.0-2-gabc1234-dirty"), and
 * reads as newer all the same: sent unforced, it would leave the chip on a
 * development build, which the knob then leaves alone. */
static bool release_version(const char *v)
{
    if (*v == 'v') v++;
    for (int part = 0; part < 3; part++) {
        if (*v < '0' || *v > '9') return false;
        while (*v >= '0' && *v <= '9') v++;
        if (part < 2 && *v++ != '.') return false;
    }
    return *v == 0;
}

static esp_err_t bt_update_h(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    EXT_RAM_BSS_ATTR static bt_link_status_t st;
    EXT_RAM_BSS_ATTR static bt_link_upd_t    u;
    EXT_RAM_BSS_ATTR static char             why[160];
    char q[32] = "", v[4] = "";
    const bool force = httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK &&
                       httpd_query_key_value(q, "force", v, sizeof v) == ESP_OK && v[0] == '1';
    bt_link_status(&st);
    bt_link_update_status(&u);
    const char *no = !st.companion                   ? "the second chip does not answer"
                   : !(st.flags & BTL_HELLO_UPDATE)  ? "the second chip takes no updates: it needs the bench once"
                   : !u.info                         ? "the second chip has not said yet what it runs: again in a moment"
                   : bt_link_update_holding()        ? "an update of the second chip is held or going already"
                   : NULL;
    if (no) return bt_update_refuse(r, "409 Conflict", no);
    const int len = r->content_len;
    if (len < 2 * 4096 || len > OTA_COMPANION_SLOT || len % 4096)
        return bt_update_refuse(r, HTTPD_400, "not a second-chip firmware: its size");
    uint8_t *img = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
    if (!img) return bt_update_refuse(r, HTTPD_500, "no memory for it");

    audio_stats_t a0, a1;
    audio_out_stats(&a0);
    const int64_t t0 = esp_timer_get_time();
    int got = 0, stalls = 0;
    while (got < len) {
        const int k = httpd_req_recv(r, (char *)img + got, len - got > 2048 ? 2048 : len - got);
        if (k == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++stalls > kUploadStalls) break;
            continue;
        }
        if (k <= 0) break;
        stalls = 0;
        got += k;
        vTaskDelay(pdMS_TO_TICKS(BT_UP_PACE_MS));
    }
    audio_out_stats(&a1);
    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (got < len) {
        free(img);
        ESP_LOGW(TAG, "second chip: an image from the page broke off after %d of %d bytes", got, len);
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "transfer interrupted");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "second chip: %d bytes from the configuration page in %lu.%lu s; radio audio frames "
                  "dropped meanwhile: %lu", len, (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 100),
             (unsigned long)(a1.dropped - a0.dropped));

    char ver[33];
    uint8_t app[8];
    if (!ota_companion_image_ok(img, (size_t)len, ver, sizeof ver, app)) {
        free(img);
        return bt_update_refuse(r, HTTPD_400, "not a signed second-chip firmware");
    }
    /* As things are now, after the seconds the upload took. */
    bt_link_status(&st);
    bt_link_update_status(&u);
    why[0] = 0;
    if (!memcmp(app, u.chip.app_sha, 8)) {
        snprintf(why, sizeof why, "the second chip runs %s already", ver);
    } else if (!force) {
        char blk[100];
        if (!(u.chip.flags & BTL_INFO_RELEASE))
            snprintf(why, sizeof why, "the second chip runs a development build (%s): ?force=1 sends it anyway",
                     st.version);
        else if (!release_version(ver))
            snprintf(why, sizeof why, "%s is a development build, not a release: ?force=1 sends it anyway", ver);
        else if (!ota_is_newer(ver, st.version))
            snprintf(why, sizeof why, "%s is not newer than the second chip's %s: ?force=1 sends it anyway", ver,
                     st.version);
        else if (bt_link_update_blocked(app, blk, sizeof blk))
            snprintf(why, sizeof why, "%s is not sent again: %s", ver, blk);
    }
    if (why[0]) {
        free(img);
        return bt_update_refuse(r, "409 Conflict", why);
    }
    /* Its SHA-256, for the chip to check the bytes against: on this task,
     * in pieces, as ota.c hashes its downloads. */
    uint8_t sha[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    for (int at = 0; at < len; at += 8192)
        mbedtls_sha256_update(&c, img + at, len - at < 8192 ? (size_t)(len - at) : 8192);
    mbedtls_sha256_finish(&c, sha);
    mbedtls_sha256_free(&c);
    if (bt_link_update_start(img, (size_t)len, sha, force) != ESP_OK) {
        free(img);
        return bt_update_refuse(r, "409 Conflict", "the second chip cannot take it now: again in a moment");
    }
    ESP_LOGI(TAG, "second chip: %s handed over from the configuration page%s", ver, force ? ", forced" : "");
    char v_esc[48], out[96];
    json_esc(ver, v_esc, sizeof v_esc);         /* the image's own words: its signature is the chip's to check */
    snprintf(out, sizeof out, "{\"ok\":true,\"queued\":\"%s\"}", v_esc);
    return send_json(r, out);
}
#else
#define BT_URIS 0
#endif

static esp_err_t send_page(httpd_req_t *r, const char *start, const char *end)
{
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, start, end - start - 1);
}

static esp_err_t config_page(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    return send_page(r, index_html_start, index_html_end);
}

#if RADIO_PAGE
#define RADIO_URIS 4

static esp_err_t radio_page(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    return send_page(r, radio_html_start, radio_html_end);
}

static void json_str(char *out, size_t cap, const char *s)
{
    /* The radio's own names: no quotes or backslashes to escape, but keep
     * anything unexpected out of the JSON rather than trust it. */
    size_t o = 0;
    for (; s && *s && o + 1 < cap; s++)
        if (*s >= 0x20 && *s != '"' && *s != '\\') out[o++] = *s;
    out[o] = 0;
}

#if VFO_RADIO_UBERSDR
/* ,"uber":{...} -- the receiver, the spots and voices on the dial's band,
 * and its SSTV gallery, for the radio page. */
static size_t uber_json(char *j, size_t cap)
{
    EXT_RAM_BSS_ATTR static uber_info_t in;
    EXT_RAM_BSS_ATTR static uber_spot_t sp[UBER_SPOTS];
    EXT_RAM_BSS_ATTR static char files[16][72];
    uber_info(&in);
    char nm[100], lc[132], url[96];
    json_esc(in.name, nm, sizeof nm);
    json_esc(in.location, lc, sizeof lc);
    uber_base_url(url, sizeof url);
    /* A guest's time left as the knob's slab counts it (-1: no limit), and
     * whose: the session's limit, the day's, an idle limit's last minute. */
    char by = 0;
    const int left = uber_time_left(&by);
    /* The receiver of the knob's list it is on -- the one in use, or the
     * next while that cannot be reached -- where there is more than one. */
    char rn[24], rx[52];
    const int at = uber_receiver(rn, sizeof rn);
    json_esc(rn, rx, sizeof rx);
    int o = snprintf(j, cap, ",\"uber\":{\"name\":\"%s\",\"callsign\":\"%s\",\"location\":\"%s\","
                     "\"version\":\"%s\",\"session_s\":%d,\"bypassed\":%s,\"time_left_s\":%d,"
                     "\"time_left_by\":\"%s\",\"url\":\"%s\",\"sstv_n\":%d,\"rx\":%d,\"rx_name\":\"%s\","
                     "\"spots\":[",
                     nm, in.callsign, lc, in.version, in.max_session_s, in.bypassed ? "true" : "false", left,
                     by == 'S' ? "session" : by == 'D' ? "day" : by == 'I' ? "idle" : "", url, uber_sstv_count(),
                     at, rx);
    const int n = uber_spots(sp, UBER_SPOTS, NULL);
    for (int i = 0; i < n && o > 0 && (size_t)o < cap; i++) {
        char call[16];
        json_str(call, sizeof call, sp[i].call);
        o += snprintf(j + o, cap - o, "%s{\"call\":\"%s\",\"hz\":%lu,\"mode\":\"%s\",\"kind\":\"%c\","
                      "\"age\":%u,\"snr\":%d,\"wpm\":%u,\"heard\":%s}", i ? "," : "", call,
                      (unsigned long)sp[i].hz, sp[i].mode, sp[i].kind, (unsigned)sp[i].age_s, sp[i].snr,
                      (unsigned)sp[i].wpm, sp[i].heard ? "true" : "false");
    }
    if (o > 0 && (size_t)o < cap) o += snprintf(j + o, cap - o, "],\"sstv\":[");
    const int nf = uber_sstv_files(files, 16);
    for (int i = 0; i < nf && o > 0 && (size_t)o < cap; i++)
        o += snprintf(j + o, cap - o, "%s\"%s\"", i ? "," : "", files[i]);
    if (o > 0 && (size_t)o < cap) o += snprintf(j + o, cap - o, "]}");
    return o > 0 && (size_t)o < cap ? (size_t)o : 0;
}
#endif

#if VFO_RADIO_KIWI
/* ,"kiwi":{...} -- what the receiver in use says of itself, for the radio
 * page: its model and software, antenna and whereabouts, its channels, its
 * audio's rate, its frequency offset, and its day-limit mark. */
static size_t kiwi_json(char *j, size_t cap)
{
    EXT_RAM_BSS_ATTR static kiwi_info_t ki;
    kiwi_info(&ki);
    char sw[48], an[100], lc[100], st[48];
    json_esc(ki.sw, sw, sizeof sw);
    json_esc(ki.antenna, an, sizeof an);
    json_esc(ki.loc, lc, sizeof lc);
    json_esc(ki.state, st, sizeof st);
    const int o = snprintf(j, cap, ",\"kiwi\":{\"model\":\"%s\",\"sw\":\"%s\",\"antenna\":\"%s\","
                           "\"loc\":\"%s\",\"users\":%d,\"users_max\":%d,\"rate\":%.3f,"
                           "\"offset_khz\":%.3f,\"state\":\"%s\",\"ovl\":%s,\"strikes\":%d,"
                           "\"held\":%s,\"unsure\":%s}",
                           ki.model, sw, an, lc, ki.users, ki.users_max, ki.rate, ki.offset_khz, st,
                           ki.ovl ? "true" : "false", ki.strikes, ki.held ? "true" : "false",
                           ki.unsure ? "true" : "false");
    return o > 0 && (size_t)o < cap ? (size_t)o : 0;
}
#endif

/* The radio's JSON: an UberSDR's carries its spots and gallery as well; the
 * others the web SDRs' list, with their marks, and a FlexRadio's antennas and
 * the radios it found on the LAN -- a Kiwi's the receivers' names twice over,
 * with what its own says and both ears. In PSRAM. */
#if VFO_RADIO_UBERSDR
#define RADIO_JSON_BYTES 8192
#elif VFO_RADIO_KIWI
#define RADIO_JSON_BYTES 6144
#else
#define RADIO_JSON_BYTES 4608
#endif

static esp_err_t radio_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    /* Static: this task serves one request at a time, and both are big. */
    static radio_status_t st;
    EXT_RAM_BSS_ATTR static char j[RADIO_JSON_BYTES];
    radio_get_status(&st);
    static const char *LINK[] = { "DOWN", "CONNECTING", "GREETING", "READY", "DEGRADED" };
    char mode[8], agc[8], model[16], mem[20], why[16];
    json_str(mode, sizeof mode, st.mode);
    json_str(agc, sizeof agc, st.agc);
    json_str(model, sizeof model, st.model);
    json_str(mem, sizeof mem, st.mem_name);
    json_str(why, sizeof why, st.link_why);
    const bool ready = st.link == RADIO_LINK_READY || st.link == RADIO_LINK_DEGRADED;
    snprintf(j, sizeof j,
        "{\"radio\":\"%s\",\"model\":\"%s\",\"link\":\"%s\",\"ready\":%s,\"why\":\"%s\","
        "\"freq\":%lld,\"f_max\":%lld,\"mode\":\"%s\",\"tx\":%s,\"smeter\":%.1f,"
        "\"filter\":%u,\"lo\":%ld,\"hi\":%ld,\"agc\":\"%s\",\"rit\":%ld,"
        "\"have_gain\":%s,\"gain\":%d,\"gain_min\":%d,\"gain_max\":%d,\"gain_step\":%d,"
        "\"levels\":%s,\"rfgain\":%u,\"power\":%u,\"max_w\":%u,"
        "\"tuner\":%s,\"tuner_on\":%s,"
        "\"n_rx\":%u,\"rx\":%u,\"n_ant\":%u,\"ant\":%u,\"has_rx_ant\":%s,\"ant_rx\":%s,"
        "\"memories\":%s,\"mem_state\":%u,\"mem_group\":%u,\"mem_ch\":%u,\"mem_name\":\"%s\"",
        ota_radio(), model, st.link <= RADIO_LINK_DEGRADED ? LINK[st.link] : "?",
        ready ? "true" : "false", ready ? "" : why,
        (long long)st.f_display, (long long)st.f_max, mode, st.tx ? "true" : "false",
        (double)st.smeter_dbm,
        (unsigned)st.filter_no, (long)st.filt_lo, (long)st.filt_hi, agc, (long)st.rit_hz,
        st.have_gain ? "true" : "false", st.gain, st.gain_min, st.gain_max, st.gain_step,
        st.has_levels && st.have_levels ? "true" : "false",
        (unsigned)st.rf_gain_pct, (unsigned)st.rf_power_pct, (unsigned)st.max_w,
        st.has_tuner && st.have_tuner ? "true" : "false", st.tuner_on ? "true" : "false",
        (unsigned)st.n_rx, (unsigned)st.rx, (unsigned)st.n_ant, (unsigned)st.ant,
        st.has_rx_ant ? "true" : "false", st.ant_rx ? "true" : "false",
        st.has_memories ? "true" : "false", (unsigned)st.mem_state,
        (unsigned)st.mem_group, (unsigned)st.mem_ch, mem);
    size_t o = strlen(j);
    /* A receiver's SNR, and a gain whose steps have names (the ubersdr
     * firmware's noise filter). */
    o += snprintf(j + o, sizeof j - o, ",\"have_snr\":%s,\"snr\":%.1f",
                  st.have_snr ? "true" : "false", (double)st.snr_db);
    /* A receiver, and its squelch (the IC-R8600). */
    o += snprintf(j + o, sizeof j - o, ",\"rx_only\":%s,\"has_squelch\":%s,\"squelch\":%u",
                  st.rx_only ? "true" : "false",
                  st.has_squelch && st.have_squelch ? "true" : "false",
                  (unsigned)st.squelch_pct);
    /* Antennas by the radio's own names, and a transmit antenna apart (the
     * FlexRadio's slice); memories in one list, with no groups (its too). */
    {
        char an[48], tn[40];
        json_str(an, sizeof an, st.ant_names);
        json_str(tn, sizeof tn, st.tx_ant_names);
        o += snprintf(j + o, sizeof j - o, ",\"ant_names\":\"%s\",\"n_tx_ant\":%u,\"tx_ant\":%u,"
                      "\"tx_ant_names\":\"%s\",\"mem_all\":%s", an, (unsigned)st.n_tx_ant,
                      (unsigned)st.tx_ant, tn, st.mem_all ? "true" : "false");
    }
    for (int i = 0; i < st.n_gain_names && i < RADIO_GAIN_NAMES && o < sizeof j - 16; i++) {
        char gn[8];
        json_str(gn, sizeof gn, st.gain_names[i]);
        o += snprintf(j + o, sizeof j - o, "%s\"%s\"", i ? "," : ",\"gain_names\":[", gn);
    }
    if (st.n_gain_names && o < sizeof j - 2) o += snprintf(j + o, sizeof j - o, "]");
    if (o >= sizeof j) o = sizeof j - 1;
#if VFO_RADIO_UBERSDR
    o += uber_json(j + o, sizeof j - o);
#endif
#if VFO_RADIO_KIWI
    o += kiwi_json(j + o, sizeof j - o);
#endif
#if VFO_HAS_SDR
    o += sdr_json(j + o, sizeof j - o);
#endif
    o += radios_names_json(j + o, sizeof j - o);
    snprintf(j + o, sizeof j - o, "}");
    return send_json(r, j);
}

/* Settings from the query string, or from a form: see above. */
static esp_err_t radio_set(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    static char q[320];
    q[0] = 0;
    if (r->method == HTTP_POST) {
        const int total = r->content_len;
        if (total <= 0 || total >= (int)sizeof q) {
            httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body size");
            return ESP_FAIL;
        }
        int got = 0;
        while (got < total) {
            const int k = httpd_req_recv(r, q + got, total - got);
            if (k <= 0) return ESP_FAIL;
            got += k;
        }
        q[got] = 0;
    } else if (httpd_req_get_url_query_str(r, q, sizeof q) != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "nothing to set");
        return ESP_FAIL;
    }
    char v[32];
    long n;
#if VFO_RADIO_KIWI
    /* Another receiver, at once -- even while the one in use is down, which
     * is when another is wanted most. The operator's act, even of the one
     * in use (kiwi.h). The right ear (sdr=) and the balance between the
     * two are the knob's own as well. One receiver is never in both ears:
     * the other ear's is refused (409). A place not in the list is said
     * (400), and nothing of the request done. */
    bool own = false, refused = false;
    long rx = -1;
    const bool by_rx = field_num(q, "receiver", &rx);
    const bool by_sdr = field(q, "sdr", v, sizeof v) && v[0];
    if ((by_rx && no_such_rx(r, rx, sdr_count())) ||
        (by_sdr && v[0] >= '0' && v[0] <= '9' && no_such_rx(r, strtol(v, NULL, 10), sdr_count())))
        return ESP_FAIL;
    if (by_rx) {
        own = true;
        refused |= !kiwi_rx_use((int)rx, true);
    }
    if (by_sdr) {
        own = true;
        if (v[0] >= '0' && v[0] <= '9') refused |= !sdr_rx_choose(atoi(v));
        else if (!strcasecmp(v, "off") || !strcasecmp(v, "local")) sdr_rx_choose(-1);
    }
    if (field_num(q, "balance", &n)) {
        own = true;
        sdr_rx_set_balance((int8_t)clampl(n, -100, 100));
    }
    if (refused) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "one receiver is never in both ears: that one is in the other");
    }
    if (!radio_is_ready() && !own) {
#else
    if (!radio_is_ready()) {
#endif
        httpd_resp_set_status(r, "503 Service Unavailable");
        return httpd_resp_sendstr(r, "the radio is not connected");
    }
    if (radio_on_air()) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "the radio is transmitting");
    }
    if (field(q, "freq", v, sizeof v) && v[0]) {
        /* Hz, or MHz with a point: 14074000 or 14.074. */
        const double f = strchr(v, '.') ? strtod(v, NULL) * 1e6 : strtod(v, NULL);
        /* Up to the radio's own top, where it says: the IC-905 tunes to
         * 10.5 GHz, the IC-R8600 to 3 GHz. 1.3 GHz where nothing is known. */
        EXT_RAM_BSS_ATTR static radio_status_t lim;
        radio_get_status(&lim);
        const double top = lim.f_max > 0 ? (double)lim.f_max : 1.3e9;
        if (f >= 10000.0 && f <= top) {
            ESP_LOGI(TAG, "web: freq -> %.0f", f);
            radio_goto_freq((int64_t)(f + 0.5));
        }
    }
    if (field(q, "mode", v, sizeof v) && v[0]) radio_set_mode(v);
    if (field_num(q, "filter", &n) && n >= 1 && n <= 3) radio_select_filter((uint8_t)n);
    long lo, hi;
    if (field_num(q, "lo", &lo) && field_num(q, "hi", &hi) && lo < hi) radio_set_filter(lo, hi);
    if (field(q, "agc", v, sizeof v) && v[0]) radio_set_agc(v);
    if (field_num(q, "gain", &n)) radio_set_gain((int8_t)clampl(n, -20, 60));
    if (field_num(q, "rfgain", &n)) radio_set_rf_gain((uint8_t)clampl(n, 0, 100));
    if (field_num(q, "power", &n)) radio_set_rf_power((uint8_t)clampl(n, 0, 100));
    if (field(q, "tuner", v, sizeof v) && v[0])
        radio_set_tuner(strcmp(v, "on") == 0 || strcmp(v, "1") == 0);
    if (field_num(q, "squelch", &n)) radio_set_squelch((uint8_t)clampl(n, 0, 100));
    if (field(q, "rx", v, sizeof v) && v[0])
        radio_select_rx(strcasecmp(v, "sub") == 0 || strcmp(v, "1") == 0);
    if (field_num(q, "ant", &n) && n >= 1 && n <= 12) {
        long rxant = 0;
        field_num(q, "rxant", &rxant);
        radio_set_antenna((uint8_t)(n - 1), rxant != 0);
    }
    /* The transmit antenna, where it is chosen apart: its place, from 1. */
    if (field_num(q, "txant", &n) && n >= 1 && n <= 12) radio_set_tx_antenna((uint8_t)(n - 1));
    if (field_num(q, "rit", &n)) radio_set_rit((int32_t)clampl(n, -9999, 9999));
#if VFO_HAS_SDR && !VFO_RADIO_KIWI
    /* What is heard: the radio alone ("local"), or a web SDR beside it -- by
     * its place in the configuration page's list, from 0 -- and the mix. A
     * choice, even of the one playing: see sdr_rx_choose. The kiwi
     * firmware's right ear is chosen above, with its receiver. */
    if (field(q, "sdr", v, sizeof v) && v[0]) {
        if (v[0] >= '0' && v[0] <= '9') sdr_rx_choose(atoi(v));
        else if (!strcasecmp(v, "local")) sdr_rx_choose(-1);
    }
    if (field_num(q, "balance", &n)) sdr_rx_set_balance((int8_t)clampl(n, -100, 100));
#endif
    /* The state as it stands: what was asked goes out to the radio as this
     * answers, so a reader wanting it confirmed asks again. */
    return radio_get(r);
}
#else
#define RADIO_URIS 0
#endif

static esp_err_t root_get(httpd_req_t *r)
{
    if (net_prov_ap_active()) {
        httpd_resp_set_type(r, "text/html");
        httpd_resp_set_hdr(r, "Cache-Control", "no-store");
        return httpd_resp_send(r, portal_html_start, portal_html_end - portal_html_start - 1);
    }
    REQUIRE_AUTH(r);
#if RADIO_PAGE
    /* The radio's controls while it is connected; the configuration page,
     * at /config, a button away. */
    if (radio_is_ready()) return send_page(r, radio_html_start, radio_html_end);
#endif
    return send_page(r, index_html_start, index_html_end);
}

esp_err_t webcfg_start(void)
{
    if (s_srv) return ESP_OK;

    const httpd_uri_t *extra = NULL;
    const size_t n_extra = radio_web_endpoints(&extra);

    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port      = 80;
    c.lru_purge_enable = true;
    c.max_uri_handlers = 10 + n_extra + PORTAL_URIS + WIFI_URIS + RADIO_URIS + SDR_URIS + RADIOS_URIS + BT_URIS;
    /* An upload ends in esp_ota_end() checking the RSA signature, on this
     * task: at 4608 that left 448 bytes (measured), and 416 more on the path
     * overflowed it. Internal RAM, because the same task writes flash. */
    c.stack_size       = 5632;
    /* Below LVGL and the knob: a page refresh must never cost a detent. */
    c.task_priority    = 3;
    c.recv_wait_timeout = 5;
    c.send_wait_timeout = 5;
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
        { .uri = "/api/coredump", .method = HTTP_GET, .handler = coredump_get },
        { .uri = "/config",      .method = HTTP_GET,  .handler = config_page },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++)
        httpd_register_uri_handler(s_srv, &uris[i]);
#if RADIO_PAGE
    static const httpd_uri_t radio_uris[] = {
        { .uri = "/radio",          .method = HTTP_GET,  .handler = radio_page },
        { .uri = "/api/radio",      .method = HTTP_GET,  .handler = radio_get },
        { .uri = "/api/radio/set",  .method = HTTP_GET,  .handler = radio_set },
        { .uri = "/api/radio/set",  .method = HTTP_POST, .handler = radio_set },
    };
    for (size_t i = 0; i < sizeof radio_uris / sizeof radio_uris[0]; i++)
        httpd_register_uri_handler(s_srv, &radio_uris[i]);
#endif
#if RADIOS_URIS
    static const httpd_uri_t radios_uris[] = {
        { .uri = "/api/radios",        .method = HTTP_GET,  .handler = radios_get_h },
        { .uri = "/api/radios",        .method = HTTP_POST, .handler = radios_post_h },
        { .uri = "/api/radios/switch", .method = HTTP_POST, .handler = radios_switch_h },
    };
    for (size_t i = 0; i < sizeof radios_uris / sizeof radios_uris[0]; i++)
        httpd_register_uri_handler(s_srv, &radios_uris[i]);
#endif
#if VFO_HAS_SDR || VFO_RADIO_KIWI
    static const httpd_uri_t sdr_uris[] = {
        { .uri = "/api/sdr",      .method = HTTP_GET,  .handler = sdr_get_h },
        { .uri = "/api/sdr",      .method = HTTP_POST, .handler = sdr_post_h },
        { .uri = "/api/sdr/test", .method = HTTP_POST, .handler = sdr_test_h },
    };
    for (size_t i = 0; i < sizeof sdr_uris / sizeof sdr_uris[0]; i++)
        httpd_register_uri_handler(s_srv, &sdr_uris[i]);
#endif
#if !VFO_RADIO_SETUP
    static const httpd_uri_t bt_uris[] = {
        { .uri = "/api/bt", .method = HTTP_GET,  .handler = bt_get_h },
        { .uri = "/api/bt", .method = HTTP_POST, .handler = bt_post_h },
        { .uri = "/api/bt/update", .method = HTTP_POST, .handler = bt_update_h },
    };
    for (size_t i = 0; i < sizeof bt_uris / sizeof bt_uris[0]; i++)
        httpd_register_uri_handler(s_srv, &bt_uris[i]);
#endif
    for (size_t i = 0; i < n_extra; i++) {
        const httpd_uri_t u = { .uri = extra[i].uri, .method = extra[i].method,
                                .handler = radio_endpoint, .user_ctx = (void *)&extra[i] };
        httpd_register_uri_handler(s_srv, &u);
    }
    /* The hotspot's page's endpoints, which answer only while the hotspot is
     * up; and the WiFi list, behind the page's login like the rest. */
    static const httpd_uri_t portal[] = {
        { .uri = "/api/scan", .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/join", .method = HTTP_POST, .handler = join_post },
        { .uri = "/api/join", .method = HTTP_GET,  .handler = join_get },
        { .uri = "/api/wifi", .method = HTTP_GET,  .handler = wifi_get_h },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_h },
    };
    for (size_t i = 0; i < sizeof portal / sizeof portal[0]; i++)
        httpd_register_uri_handler(s_srv, &portal[i]);
    httpd_register_err_handler(s_srv, HTTPD_404_NOT_FOUND, portal_404);

    ESP_LOGI(TAG, "configuration page on http://<device>/ (port 80)");
    return ESP_OK;
}
