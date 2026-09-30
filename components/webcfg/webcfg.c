#include "webcfg.h"

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

#include "audio_in.h"
#include "audio_out.h"
#include "net_prov.h"
#include "ota.h"
#include "ptt_fsm.h"
#include "radio.h"
#if VFO_HAS_SDR
#include "sdr_rx.h"
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
    /* A second receiver and the antennas, where the radio has them. */
    char ant[12] = "";
    if (st.n_ant && st.have_ant)
        snprintf(ant, sizeof ant, "ANT%u%s", st.ant + 1u, st.ant_rx ? "+RX" : "");

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
        "\"talking\":%s,\"rx\":\"%s\",\"ant\":\"%s\"}",
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
        st.n_rx > 1 ? (st.rx ? "SUB" : "MAIN") : "", ant);
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
             "\"vol\":%u,\"mic\":%u,"
             "\"user\":\"%s\",\"defaultpw\":%s,\"otah\":%u,\"dim\":%u,\"blank\":%u,"
             "\"radio\":\"%s\",\"fwbase\":\"%s\",\"fwroot\":\"%s\","
             "\"ruser\":\"%s\",\"rpass\":%s,\"link\":\"%s\","
             "\"client\":\"%s\",\"client_usb\":%s}",
             c->radio_host, (unsigned)c->radio_port, c->ssid,
             (unsigned)net_prov_volume(), (unsigned)net_prov_mic_gain(),
             net_prov_web_user(),
             net_prov_web_is_default() ? "true" : "false",
             (unsigned)net_prov_ota_hours(), (unsigned)net_prov_dim_min(),
             (unsigned)net_prov_blank_min(), ota_radio(), ota_base_url(),
             ota_root_url(), c->radio_user, c->radio_pass[0] ? "true" : "false",
             radio_link_name(), client, client_usb ? "true" : "false");
    if (n < 0 || n >= (int)sizeof buf) return httpd_resp_send_500(r);
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

    uint8_t vol = net_prov_volume(), mic = net_prov_mic_gain();
    if (field_num(body, "vol", &v)) vol = (uint8_t)clampl(v, 0, 100);
    /* Up to 200%, as on the dial: the PDM element is quiet. Clamping to 100
     * here used to halve a gain set on the dial whenever the page was saved. */
    if (field_num(body, "mic", &v)) mic = (uint8_t)clampl(v, 0, 200);
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
    audio_out_set_volume(vol);    /* directly too: with no display, no UI task */
    audio_in_set_gain(mic);

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

/* The face as it is drawn, as a BMP -- 24-bit, rows top-down, which every
 * browser shows and nothing needs converting. The USB build has no console
 * to take one over, and this page is always reachable. The pixels and the
 * row being sent are both in PSRAM, as ui_snapshot() is: nothing of a
 * picture is worth internal RAM. */
static esp_err_t screenshot_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    uint32_t w = 0, h = 0;
    uint16_t *px = ui_snapshot(&w, &h);
    const uint32_t row = (w * 3 + 3) & ~3u, img = row * h;
    uint8_t *line = px ? heap_caps_malloc(row, MALLOC_CAP_SPIRAM) : NULL;
    if (!line) {
        heap_caps_free(px);
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no screenshot");
        return ESP_FAIL;
    }
    uint8_t hdr[54] = { 'B', 'M' };
    const uint32_t f[] = { 54 + img, 0, 54, 40, w, (uint32_t)-(int32_t)h };
    memcpy(hdr + 2, &f[0], 4);   memcpy(hdr + 10, &f[2], 4);
    memcpy(hdr + 14, &f[3], 4);  memcpy(hdr + 18, &f[4], 4);
    memcpy(hdr + 22, &f[5], 4);                  /* negative: top-down */
    hdr[26] = 1; hdr[28] = 24;                   /* planes, bits per pixel */
    memcpy(hdr + 34, &img, 4);

    httpd_resp_set_type(r, "image/bmp");
    esp_err_t err = httpd_resp_send_chunk(r, (const char *)hdr, sizeof hdr);
    for (uint32_t y = 0; y < h && err == ESP_OK; y++) {
        memset(line, 0, row);
        for (uint32_t x = 0; x < w; x++) {
            const uint16_t v = px[y * w + x];
            line[x * 3]     = (uint8_t)((v & 0x1F) * 255 / 31);          /* B */
            line[x * 3 + 1] = (uint8_t)((v >> 5 & 0x3F) * 255 / 63);     /* G */
            line[x * 3 + 2] = (uint8_t)((v >> 11 & 0x1F) * 255 / 31);    /* R */
        }
        err = httpd_resp_send_chunk(r, (const char *)line, row);
    }
    heap_caps_free(line);
    heap_caps_free(px);
    httpd_resp_send_chunk(r, NULL, 0);
    return err;
}

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
 * rather than guessed at.
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
    return ok;
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

#if VFO_RADIO_SETUP
#define PORTAL_URIS 3
/* ------------------------------------------------------------ the portal */

/* The setup firmware's WiFi setup, for a phone on the knob's own hotspot:
 * open, since the phone has nothing to log in with yet, and there only while
 * the hotspot is up. On the network the knob then joins, it is gone and the
 * page is the usual one, behind its login. */
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
    snprintf(buf, sizeof buf, "{\"state\":\"%s\",\"ssid\":\"%s\",\"why\":\"%s\"}",
             ST[s], es, ew);
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
#else
#define PORTAL_URIS 0
#endif

#if !VFO_RADIO_SETUP && !VFO_RADIO_SVXCONNECT
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
 * every radio, and how it is reached ("LAN", "SmartLink"). */
static size_t radios_names_json(char *j, size_t cap)
{
    const int nd = net_prov_radio_count(), nf = radio_found_count();
    int o = snprintf(j, cap, ",\"radios\":{\"sel\":%d,\"names\":[", radios_sel());
    for (int i = 0; i < nd + nf && o > 0 && (size_t)o < cap; i++) {
        static net_radio_t r;
        char nm[24] = "", n[68];
        if (i < nd) {
            if (!net_prov_radio_get(i, &r)) break;
            strlcpy(nm, r.name[0] ? r.name : r.host, sizeof nm);
        } else if (!radio_found_get(i - nd, nm, sizeof nm)) {
            break;
        }
        json_esc(nm, n, sizeof n);
        o += snprintf(j + o, cap - o, "%s\"%s\"", i ? "," : "", n);
    }
    if (o > 0 && (size_t)o < cap) o += snprintf(j + o, cap - o, "],\"via\":[");
    for (int i = 0; i < nd + nf && o > 0 && (size_t)o < cap; i++)
        o += snprintf(j + o, cap - o, "%s\"%s\"", i ? "," : "", i < nd ? "LAN" : radio_found_via());
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
        /* "http://host:port/" and "host:port" too, as for the SDRs. */
        const char *s = strstr(v, "://");
        s = s ? s + 3 : v;
        const size_t hl = strcspn(s, ":/ ");
        if (!hl || hl >= sizeof e->host) continue;
        memcpy(e->host, s, hl);
        e->host[hl] = 0;
        long port = 0;
        snprintf(key, sizeof key, "port%d", i);
        if (field_num(body, key, &port) && port > 0 && port < 65536) e->port = (uint16_t)port;
        else if (s[hl] == ':') e->port = (uint16_t)clampl(strtol(s + hl + 1, NULL, 10), 1, 65535);
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
    if (to < nd) strlcpy(name, e.name[0] ? e.name : e.host, sizeof name);
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
#else
#define RADIOS_URIS 0
#endif

#if VFO_HAS_SDR
/* ------------------------------------------------------------- web SDRs
 *
 *   GET  /api/sdr        the receivers (their passwords only as set or not),
 *                        the one listened to and how that goes, the balance
 *   POST /api/sdr        n=, then name0 host0 pass0 ipl0 was0, name1 ... : the
 *                        list, saved. A password left out is the one receiver
 *                        `was` had, at the same address; empty is none.
 *                        sel= (local, or 0-3) and balance= (-100..100) too.
 *   POST /api/sdr/test   host pass ipl was: one receiver tried -- reached,
 *                        what it calls itself, its users, the password
 */
#define SDR_URIS 3

/* ,"sdr":{...} -- for the radio's JSON, and without the key for /api/sdr. */
static size_t sdr_json(char *j, size_t cap)
{
    sdr_status_t s;
    sdr_rx_status(&s);
    char st[48];
    json_esc(s.state, st, sizeof st);
    int o = snprintf(j, cap, ",\"sdr\":{\"sel\":%d,\"state\":\"%s\",\"streaming\":%s,"
                     "\"smeter\":%.1f,\"balance\":%d,\"list\":[",
                     sdr_rx_selected(), st, s.streaming ? "true" : "false",
                     (double)s.smeter_dbm, sdr_rx_balance());
    for (int i = 0; i < sdr_count() && o > 0 && (size_t)o < cap; i++) {
        sdr_cfg_t c;
        if (!sdr_get(i, &c)) break;
        char n[52], h[132];
        json_esc(c.name, n, sizeof n);
        json_esc(c.host, h, sizeof h);
        o += snprintf(j + o, cap - o, "%s{\"name\":\"%s\",\"host\":\"%s\",\"port\":%u,"
                      "\"pass\":%s,\"ipl\":%s}", i ? "," : "", n, h, (unsigned)c.port,
                      c.pass[0] ? "true" : "false", c.ipl[0] ? "true" : "false");
    }
    if (o > 0 && (size_t)o < cap) o += snprintf(j + o, cap - o, "]}");
    return o > 0 && (size_t)o < cap ? (size_t)o : 0;
}

static esp_err_t sdr_get_h(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    EXT_RAM_BSS_ATTR static char j[1400];     /* internal RAM is the scarce one */
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

/* "http://host:port/...", "host:port" or "host": the host, and the port when
 * there is one. */
static bool split_host(const char *in, char *host, size_t cap, uint16_t *port)
{
    const char *s = strstr(in, "://");
    s = s ? s + 3 : in;
    while (*s == ' ') s++;
    const size_t n = strcspn(s, ":/ ");
    if (!n || n >= cap) return false;
    memcpy(host, s, n);
    host[n] = 0;
    if (s[n] == ':') {
        const long p = strtol(s + n + 1, NULL, 10);
        if (p > 0 && p < 65536) *port = (uint16_t)p;
    }
    return true;
}

/* A receiver's fields from a form, their names ending in `sfx` ("0".."3", or
 * "" for the test). A password not in the form is the one it had -- only at
 * the same address, so a receiver moved elsewhere never gets the old one's. */
static bool sdr_from_form(const char *body, const char *sfx, sdr_cfg_t *c, long *was_out)
{
    char key[12], hp[96] = "";
    memset(c, 0, sizeof *c);
    c->port = 8073;
    snprintf(key, sizeof key, "host%s", sfx);
    if (!field(body, key, hp, sizeof hp) || !split_host(hp, c->host, sizeof c->host, &c->port))
        return false;
    snprintf(key, sizeof key, "name%s", sfx);
    field(body, key, c->name, sizeof c->name);
    long was = -1;
    snprintf(key, sizeof key, "was%s", sfx);
    field_num(body, key, &was);
    if (was_out) *was_out = was;
    EXT_RAM_BSS_ATTR static sdr_cfg_t old;       /* this task's stack is tight */
    const bool had = was >= 0 && sdr_get((int)was, &old) &&
                     !strcasecmp(old.host, c->host) && old.port == c->port;
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
    if (field_num(body, "n", &n)) {
        EXT_RAM_BSS_ATTR static sdr_cfg_t list[SDR_MAX];
        const int cur = sdr_rx_selected();
        int k = 0, sel = -1;
        for (int i = 0; i < clampl(n, 0, SDR_MAX); i++) {
            char sfx[4];
            long was;
            snprintf(sfx, sizeof sfx, "%d", i);
            if (!sdr_from_form(body, sfx, &list[k], &was)) continue;
            if (cur >= 0 && was == cur) sel = k;      /* the one listened to, still */
            k++;
        }
        sdr_rx_select(-1);
        if (sdr_save(list, k) != ESP_OK) return httpd_resp_send_500(r);
        sdr_rx_select(sel);
        ESP_LOGI(TAG, "web: %d web SDR%s", k, k == 1 ? "" : "s");
    }
    if (field(body, "sel", v, sizeof v) && v[0]) {
        if (v[0] >= '0' && v[0] <= '9') sdr_rx_select(atoi(v));
        else if (!strcasecmp(v, "local")) sdr_rx_select(-1);
    }
    if (field_num(body, "balance", &n)) sdr_rx_set_balance((int8_t)clampl(n, -100, 100));
    return sdr_get_h(r);
}

/* Blocks this task for the test's few seconds: the page waits for it. */
static esp_err_t sdr_test_h(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    EXT_RAM_BSS_ATTR static char body[512], j[400];
    EXT_RAM_BSS_ATTR static sdr_cfg_t c;
    if (!recv_form(r, body, sizeof body)) return ESP_FAIL;
    if (!sdr_from_form(body, "", &c, NULL)) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "no address");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "web: testing %s:%u", c.host, (unsigned)c.port);
    sdr_test(&c, j, sizeof j);
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
 *   GET /api/radio                          the state, as JSON
 *   GET /api/radio/set?freq=14074000        Hz; 14.074 (a point) is MHz
 *       ...&mode=usb&filter=2&agc=mid&gain=1&rfgain=80&power=50
 *       ...&tuner=on&rx=sub&ant=2&rxant=1&rit=-120&lo=100&hi=2800
 *       ...&sdr=0&balance=-30    a web SDR beside it, "local" for none (see
 *                                above: the Icom, Xiegu and FlexRadio ones)
 *   (POST, with the same fields as a form, does the same.)
 *
 * Behind the page's login like everything here. Nothing that transmits:
 * neither PTT nor a tune cycle, on the page or in the API, and no setting at
 * all while the radio is on the air. Not for the setup firmware, which has
 * no radio, nor svxconnect's reflector. */
#define RADIO_PAGE (!VFO_RADIO_SETUP && !VFO_RADIO_SVXCONNECT)

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

static esp_err_t radio_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    /* Static: this task serves one request at a time, and both are big. */
    static radio_status_t st;
    EXT_RAM_BSS_ATTR static char j[2560];
    radio_get_status(&st);
    static const char *LINK[] = { "DOWN", "CONNECTING", "GREETING", "READY", "DEGRADED" };
    char mode[8], agc[8], model[16], mem[20];
    json_str(mode, sizeof mode, st.mode);
    json_str(agc, sizeof agc, st.agc);
    json_str(model, sizeof model, st.model);
    json_str(mem, sizeof mem, st.mem_name);
    const bool ready = st.link == RADIO_LINK_READY || st.link == RADIO_LINK_DEGRADED;
    snprintf(j, sizeof j,
        "{\"radio\":\"%s\",\"model\":\"%s\",\"link\":\"%s\",\"ready\":%s,"
        "\"freq\":%lld,\"f_max\":%lld,\"mode\":\"%s\",\"tx\":%s,\"smeter\":%.1f,"
        "\"filter\":%u,\"lo\":%ld,\"hi\":%ld,\"agc\":\"%s\",\"rit\":%ld,"
        "\"have_gain\":%s,\"gain\":%d,\"gain_min\":%d,\"gain_max\":%d,\"gain_step\":%d,"
        "\"levels\":%s,\"rfgain\":%u,\"power\":%u,\"max_w\":%u,"
        "\"tuner\":%s,\"tuner_on\":%s,"
        "\"n_rx\":%u,\"rx\":%u,\"n_ant\":%u,\"ant\":%u,\"has_rx_ant\":%s,\"ant_rx\":%s,"
        "\"memories\":%s,\"mem_state\":%u,\"mem_group\":%u,\"mem_ch\":%u,\"mem_name\":\"%s\"",
        ota_radio(), model, st.link <= RADIO_LINK_DEGRADED ? LINK[st.link] : "?",
        ready ? "true" : "false",
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
    if (!radio_is_ready()) {
        httpd_resp_set_status(r, "503 Service Unavailable");
        return httpd_resp_sendstr(r, "the radio is not connected");
    }
    if (radio_on_air()) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "the radio is transmitting");
    }
    char v[32];
    long n;
    if (field(q, "freq", v, sizeof v) && v[0]) {
        /* Hz, or MHz with a point: 14074000 or 14.074. */
        const double f = strchr(v, '.') ? strtod(v, NULL) * 1e6 : strtod(v, NULL);
        if (f >= 10000.0 && f <= 1.3e9) {
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
    if (field(q, "rx", v, sizeof v) && v[0])
        radio_select_rx(strcasecmp(v, "sub") == 0 || strcmp(v, "1") == 0);
    if (field_num(q, "ant", &n) && n >= 1 && n <= 4) {
        long rxant = 0;
        field_num(q, "rxant", &rxant);
        radio_set_antenna((uint8_t)(n - 1), rxant != 0);
    }
    if (field_num(q, "rit", &n)) radio_set_rit((int32_t)clampl(n, -9999, 9999));
#if VFO_HAS_SDR
    /* What is heard: the radio alone ("local"), or a web SDR beside it -- by
     * its place in the configuration page's list, from 0 -- and the mix. */
    if (field(q, "sdr", v, sizeof v) && v[0]) {
        if (v[0] >= '0' && v[0] <= '9') sdr_rx_select(atoi(v));
        else if (!strcasecmp(v, "local")) sdr_rx_select(-1);
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
#if VFO_RADIO_SETUP
    if (net_prov_ap_active()) {
        httpd_resp_set_type(r, "text/html");
        httpd_resp_set_hdr(r, "Cache-Control", "no-store");
        return httpd_resp_send(r, portal_html_start, portal_html_end - portal_html_start - 1);
    }
#endif
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
    c.max_uri_handlers = 11 + n_extra + PORTAL_URIS + RADIO_URIS + SDR_URIS + RADIOS_URIS;
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
        { .uri = "/api/screenshot", .method = HTTP_GET, .handler = screenshot_get },
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
#if VFO_HAS_SDR
    static const httpd_uri_t sdr_uris[] = {
        { .uri = "/api/sdr",      .method = HTTP_GET,  .handler = sdr_get_h },
        { .uri = "/api/sdr",      .method = HTTP_POST, .handler = sdr_post_h },
        { .uri = "/api/sdr/test", .method = HTTP_POST, .handler = sdr_test_h },
    };
    for (size_t i = 0; i < sizeof sdr_uris / sizeof sdr_uris[0]; i++)
        httpd_register_uri_handler(s_srv, &sdr_uris[i]);
#endif
    for (size_t i = 0; i < n_extra; i++) {
        const httpd_uri_t u = { .uri = extra[i].uri, .method = extra[i].method,
                                .handler = radio_endpoint, .user_ctx = (void *)&extra[i] };
        httpd_register_uri_handler(s_srv, &u);
    }
#if VFO_RADIO_SETUP
    static const httpd_uri_t portal[] = {
        { .uri = "/api/scan", .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/join", .method = HTTP_POST, .handler = join_post },
        { .uri = "/api/join", .method = HTTP_GET,  .handler = join_get },
    };
    for (size_t i = 0; i < sizeof portal / sizeof portal[0]; i++)
        httpd_register_uri_handler(s_srv, &portal[i]);
    httpd_register_err_handler(s_srv, HTTPD_404_NOT_FOUND, portal_404);
#endif

    ESP_LOGI(TAG, "configuration page on http://<device>/ (port 80)");
    return ESP_OK;
}
