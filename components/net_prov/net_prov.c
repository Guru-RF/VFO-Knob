#include "net_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "lwip/netdb.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

/* Each radio's firmware keeps its own endpoint and login, so switching
 * firmware under Firmware on the configuration page never points one radio's
 * client at another radio. The AetherSDR keys are the ones every earlier
 * firmware wrote. */
#if VFO_RADIO_SVXCONNECT
#define KEY_HOST     "svhost"
#define KEY_RLIST    "svlist"     /* the radios, one in use: see below */
#define KEY_RSEL     "svsel"
#define KEY_PORT     "svport"
/* Unused: the station -- callsign, certificate and all -- is the svx
 * client's own ("svx" namespace), and the reflector takes no password. */
#define KEY_USER     "svuser"
#define KEY_PASS     "svpass"
#define DEFAULT_HOST "be.svx.link"   /* its SRV record names the host and port */
#define DEFAULT_PORT 5300            /* SvxLink's reflector port */
#define DEFAULT_USER ""
#define DEFAULT_PASS ""
#elif VFO_RADIO_PHONE
/* Unused: the telephone's SIP account is the phone client's own ("phone"
 * namespace). Keys of its own, so it never writes another firmware's. */
#define KEY_HOST     "phhost"
#define KEY_RLIST    "phlist"
#define KEY_RSEL     "phsel"
#define KEY_PORT     "phport"
#define KEY_USER     "phuser"
#define KEY_PASS     "phpass"
#define DEFAULT_HOST ""
#define DEFAULT_PORT 5060
#define DEFAULT_USER ""
#define DEFAULT_PASS ""
#elif VFO_RADIO_MULTIFLEX
#define KEY_HOST     "fxhost"
#define KEY_RLIST    "fxlist"     /* the radios, one in use: see below */
#define KEY_RSEL     "fxsel"
#define KEY_PORT     "fxport"
#define KEY_USER     "fxuser"
#define KEY_PASS     "fxpass"
#define DEFAULT_HOST ""              /* the radio's IP address, given on the page */
#define DEFAULT_PORT 4992            /* FlexRadio's API */
#define DEFAULT_USER ""
#define DEFAULT_PASS ""
#elif VFO_RADIO_UBERSDR
#define KEY_HOST     "ubhost"
#define KEY_RLIST    "ublist"     /* the receivers, one in use: see below */
#define KEY_RSEL     "ubsel"
#define KEY_PORT     "ubport"
#define KEY_USER     "ubuser"     /* unused: an UberSDR has no users */
#define KEY_PASS     "ubpass"     /* its bypass password, where you have one */
#define DEFAULT_HOST ""              /* https://<name>.tunnel.ubersdr.org, given on the page */
#define DEFAULT_PORT 443             /* its tunnel's https; 8080 on a LAN */
#define DEFAULT_USER ""
#define DEFAULT_PASS ""
#elif VFO_RADIO_XIEGU
#define KEY_HOST     "xhost"
#define KEY_RLIST    "xlist"     /* the radios, one in use: see below */
#define KEY_RSEL     "xsel"
#define KEY_PORT     "xport"
#define KEY_USER     "xuser"
#define KEY_PASS     "xpass"
#define DEFAULT_HOST ""              /* the X6100 and X6200 announce no name */
#define DEFAULT_PORT 50001
#define DEFAULT_USER "user"          /* their wfview server's own login */
#define DEFAULT_PASS "123"
#elif VFO_RADIO_ICOM
#define KEY_HOST     "rhost"
#define KEY_RLIST    "rlist"     /* the radios, one in use: see below */
#define KEY_RSEL     "rsel"
#define KEY_PORT     "rport"
#define KEY_USER     "ruser"
#define KEY_PASS     "rpass"
#define DEFAULT_HOST "IC-705.local"
#define DEFAULT_PORT 50001
#define DEFAULT_USER ""
#define DEFAULT_PASS ""
#else
#define KEY_HOST     "host"
#define KEY_RLIST    "hlist"     /* the radios, one in use: see below */
#define KEY_RSEL     "hsel"
#define KEY_PORT     "port"
#define KEY_USER     "ruser"
#define KEY_PASS     "rpass"
#define DEFAULT_HOST "aethersdr.local"
#define DEFAULT_PORT 50001
#define DEFAULT_USER ""
#define DEFAULT_PASS ""
#endif

static const char *TAG = "net";
static const char *NVS_NS = "vfo";

static uint16_t s_ota_hours = 24;   /* automatic update check; 0 = off */
static uint16_t s_dim_min = 5;      /* idle before the screen dims;  0 = never */
static uint16_t s_blank_min = 10;   /* idle before it goes dark;     0 = never */

/* Credentials for the configuration page. Shipped as admin/admin so a new
 * owner can get in, and the page nags until they are changed -- this device
 * can key a transmitter, so leaving the default in place on someone else's
 * network is not a small thing. */
static char s_web_user[24] = "admin";
static char s_web_pass[33] = "admin";

static vfo_cfg_t          s_cfg;
/* The radios the knob knows, the one in use feeding s_cfg's endpoint. */
EXT_RAM_BSS_ATTR static net_radio_t s_radios[NET_PROV_RADIOS];
static int                s_nradios, s_radio_sel;
static EventGroupHandle_t s_events;
static bool               s_connected;
static int                s_retries;
static uint8_t            s_volume = 40, s_micgain = 100, s_micgain_hs = 100;
static uint8_t            s_boots;

#define BIT_GOT_IP BIT0

/* Joining a network from the setup firmware's portal: tried a few times, then
 * given up with the reason, so the portal can say it and ask again. */
static volatile net_join_t s_join = NET_JOIN_IDLE;
static char     s_join_ssid[33], s_join_pass[65], s_join_why[48];
static uint8_t  s_join_fails;
/* While the hotspot is up the stored network is not chased: a station
 * scanning the channels for it takes the hotspot off the air, and the phone
 * with it. */
static volatile bool s_hold;

/* The WiFi networks the knob knows, the one joined last first; and the one
 * the station is trying, a couple of times before the next. Read on the
 * event loop's task, written from the page's: s_wlock for the copies. */
EXT_RAM_BSS_ATTR static net_wifi_t s_wifis[NET_PROV_WIFIS];
static int          s_nwifis, s_try;
static uint8_t      s_try_fails;
static portMUX_TYPE s_wlock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_wmx;             /* the NVS write of them */
static volatile bool s_wifis_dirty;         /* reordered on the event task: net_prov_tick() writes */
static volatile int s_ap_clients;           /* phones on the hotspot */
static char s_now[33];                      /* the network joined, while it is */

static const char *reason_text(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_NO_AP_FOUND:              return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:              return "wrong password?";
    default:                                   return "could not connect";
    }
}

/* ------------------------------------------------------------ the radios
 *
 * Up to NET_PROV_RADIOS, each firmware its own list (as its own endpoint):
 * name \t host \t port \t user \t pass, a line each. The one in use is the
 * configuration's endpoint, which is what the client starts with at boot --
 * a swipe up chooses another and restarts the knob into it. A knob from
 * before the list has its one radio made the first. */
static void radio_to_cfg(const net_radio_t *r)
{
    strlcpy(s_cfg.radio_host, r->host, sizeof s_cfg.radio_host);
    s_cfg.radio_port = r->port;
    strlcpy(s_cfg.radio_user, r->user, sizeof s_cfg.radio_user);
    strlcpy(s_cfg.radio_pass, r->pass, sizeof s_cfg.radio_pass);
}

static void cfg_to_radio(net_radio_t *r)
{
    strlcpy(r->host, s_cfg.radio_host, sizeof r->host);
    r->port = s_cfg.radio_port;
    strlcpy(r->user, s_cfg.radio_user, sizeof r->user);
    strlcpy(r->pass, s_cfg.radio_pass, sizeof r->pass);
}

static void radios_parse(const char *blob)
{
    int n = 0;
    for (const char *p = blob; p && *p && n < NET_PROV_RADIOS; ) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[200];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        char *f[5] = { line, "", "", "", "" };
        int k = 1;
        for (char *q = line; *q && k < 5; q++)
            if (*q == '\t') { *q = 0; f[k++] = q + 1; }
        if (f[1][0]) {
            net_radio_t *r = &s_radios[n++];
            memset(r, 0, sizeof *r);
            strlcpy(r->name, f[0], sizeof r->name);
            strlcpy(r->host, f[1], sizeof r->host);
            r->port = (uint16_t)atoi(f[2]);
            if (!r->port) r->port = DEFAULT_PORT;
            strlcpy(r->user, f[3], sizeof r->user);
            strlcpy(r->pass, f[4], sizeof r->pass);
        }
        p = eol ? eol + 1 : NULL;
    }
    s_nradios = n;
}

static void radios_load(void)
{
    nvs_handle_t h;
    int8_t sel = 0;
    s_nradios = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = 0;
        if (nvs_get_str(h, KEY_RLIST, NULL, &n) == ESP_OK && n > 1) {
            char *blob = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
            if (blob && nvs_get_str(h, KEY_RLIST, blob, &n) == ESP_OK) radios_parse(blob);
            free(blob);
        }
        nvs_get_i8(h, KEY_RSEL, &sel);
        nvs_close(h);
    }
    if (!s_nradios) {                        /* the one radio it always had */
        memset(&s_radios[0], 0, sizeof s_radios[0]);
        cfg_to_radio(&s_radios[0]);
        s_nradios = 1;
        sel = 0;
    }
    s_radio_sel = sel >= 0 && sel < s_nradios ? sel : 0;
    radio_to_cfg(&s_radios[s_radio_sel]);
    if (s_nradios > 1)
        ESP_LOGI(TAG, "%d radios; in use: %s", s_nradios,
                 s_radios[s_radio_sel].name[0] ? s_radios[s_radio_sel].name
                                               : s_radios[s_radio_sel].host);
}

static bool clean(const char *s)
{
    for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') return false;
    return true;
}

/* The list and the one in use, and that one's endpoint as the config's. */
static esp_err_t radios_write(void)
{
    const size_t cap = NET_PROV_RADIOS * 200 + 1;
    char *blob = heap_caps_calloc(1, cap, MALLOC_CAP_SPIRAM);
    if (!blob) return ESP_ERR_NO_MEM;
    size_t o = 0;
    for (int i = 0; i < s_nradios; i++) {
        const net_radio_t *r = &s_radios[i];
        o += snprintf(blob + o, cap - o, "%s\t%s\t%u\t%s\t%s\n",
                      r->name, r->host, (unsigned)r->port, r->user, r->pass);
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        nvs_set_str(h, KEY_RLIST, blob);
        nvs_set_i8(h, KEY_RSEL, (int8_t)s_radio_sel);
        nvs_set_str(h, KEY_HOST, s_cfg.radio_host);
        nvs_set_u16(h, KEY_PORT, s_cfg.radio_port);
        nvs_set_str(h, KEY_USER, s_cfg.radio_user);
        nvs_set_str(h, KEY_PASS, s_cfg.radio_pass);
        e = nvs_commit(h);
        nvs_close(h);
    }
    free(blob);
    return e;
}

int  net_prov_radio_count(void)  { return s_nradios; }
int  net_prov_radio_active(void) { return s_radio_sel; }

bool net_prov_radio_get(int i, net_radio_t *out)
{
    if (i < 0 || i >= s_nradios || !out) return false;
    *out = s_radios[i];
    return true;
}

esp_err_t net_prov_radios_save(const net_radio_t *list, int n, int active)
{
    if (!list || n < 1 || n > NET_PROV_RADIOS || active < 0 || active >= n)
        return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < n; i++)
        if (!list[i].host[0] || !clean(list[i].name) || !clean(list[i].host) ||
            !clean(list[i].user) || !clean(list[i].pass))
            return ESP_ERR_INVALID_ARG;
    memcpy(s_radios, list, n * sizeof list[0]);
    s_nradios   = n;
    s_radio_sel = active;
    radio_to_cfg(&s_radios[active]);
    const esp_err_t e = radios_write();
    ESP_LOGI(TAG, "%d radio%s saved; in use: %s", n, n == 1 ? "" : "s",
             list[active].name[0] ? list[active].name : list[active].host);
    return e;
}

esp_err_t net_prov_radio_activate(int i)
{
    if (i < 0 || i >= s_nradios) return ESP_ERR_INVALID_ARG;
    s_radio_sel = i;
    radio_to_cfg(&s_radios[i]);
    return radios_write();
}

/* --- the WiFi networks ------------------------------------------------- */

/* The list, and its first where every firmware before it kept the one
 * network. Not on the event loop's task: its stack is 2.3 kB. */
static esp_err_t wifis_write(void)
{
    EXT_RAM_BSS_ATTR static net_wifi_t copy[NET_PROV_WIFIS];
    if (s_wmx) xSemaphoreTake(s_wmx, portMAX_DELAY);
    taskENTER_CRITICAL(&s_wlock);
    const int n = s_nwifis;
    memcpy(copy, s_wifis, sizeof copy);
    s_wifis_dirty = false;
    taskEXIT_CRITICAL(&s_wlock);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        if (n) nvs_set_blob(h, "wifis", copy, (size_t)n * sizeof copy[0]);
        else   nvs_erase_key(h, "wifis");
        nvs_set_str(h, "ssid", n ? copy[0].ssid : "");
        nvs_set_str(h, "pass", n ? copy[0].pass : "");
        err = nvs_commit(h);
        nvs_close(h);
    }
    strlcpy(s_cfg.ssid, n ? copy[0].ssid : "", sizeof s_cfg.ssid);
    strlcpy(s_cfg.pass, n ? copy[0].pass : "", sizeof s_cfg.pass);
    if (s_wmx) xSemaphoreGive(s_wmx);
    return err;
}

/* The list -- and, leading it, the network under "ssid": the one joined or
 * given last, a firmware from before the list included. */
static void wifis_load(void)
{
    nvs_handle_t h;
    s_nwifis = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof s_wifis;
        if (nvs_get_blob(h, "wifis", s_wifis, &len) == ESP_OK && len % sizeof s_wifis[0] == 0)
            s_nwifis = (int)(len / sizeof s_wifis[0]);
        nvs_close(h);
    }
    for (int i = 0; i < s_nwifis; i++) {
        s_wifis[i].ssid[sizeof s_wifis[i].ssid - 1] = 0;
        s_wifis[i].pass[sizeof s_wifis[i].pass - 1] = 0;
    }
    if (s_cfg.ssid[0] && (!s_nwifis || strcmp(s_wifis[0].ssid, s_cfg.ssid) ||
                          strcmp(s_wifis[0].pass, s_cfg.pass))) {
        int at = -1;
        for (int i = 0; i < s_nwifis; i++)
            if (!strcmp(s_wifis[i].ssid, s_cfg.ssid)) at = i;
        if (at < 0) at = s_nwifis < NET_PROV_WIFIS ? s_nwifis++ : NET_PROV_WIFIS - 1;
        memmove(&s_wifis[1], &s_wifis[0], (size_t)at * sizeof s_wifis[0]);
        strlcpy(s_wifis[0].ssid, s_cfg.ssid, sizeof s_wifis[0].ssid);
        strlcpy(s_wifis[0].pass, s_cfg.pass, sizeof s_wifis[0].pass);
    }
    if (s_nwifis) {
        strlcpy(s_cfg.ssid, s_wifis[0].ssid, sizeof s_cfg.ssid);
        strlcpy(s_cfg.pass, s_wifis[0].pass, sizeof s_cfg.pass);
    }
}

int net_prov_wifi_count(void) { return s_nwifis; }
const char *net_prov_wifi_now(void) { return s_connected ? s_now : ""; }

bool net_prov_wifi_get(int i, net_wifi_t *out)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_wlock);
    if (i >= 0 && i < s_nwifis && out) {
        *out = s_wifis[i];
        ok = true;
    }
    taskEXIT_CRITICAL(&s_wlock);
    return ok;
}

esp_err_t net_prov_wifis_save(const net_wifi_t *list, int n)
{
    if (n < 0 || n > NET_PROV_WIFIS || (n && !list)) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_wlock);
    for (int i = 0; i < n; i++) s_wifis[i] = list[i];
    s_nwifis = n;
    s_try = 0;
    taskEXIT_CRITICAL(&s_wlock);
    ESP_LOGI(TAG, "%d WiFi network%s known", n, n == 1 ? "" : "s");
    return wifis_write();
}

esp_err_t net_prov_wifi_add(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_wlock);
    int at = -1;
    for (int i = 0; i < s_nwifis; i++)
        if (!strcmp(s_wifis[i].ssid, ssid)) at = i;
    if (at < 0) at = s_nwifis < NET_PROV_WIFIS ? s_nwifis++ : NET_PROV_WIFIS - 1;
    memmove(&s_wifis[1], &s_wifis[0], (size_t)at * sizeof s_wifis[0]);
    strlcpy(s_wifis[0].ssid, ssid, sizeof s_wifis[0].ssid);
    strlcpy(s_wifis[0].pass, pass ? pass : "", sizeof s_wifis[0].pass);
    s_try = 0;
    taskEXIT_CRITICAL(&s_wlock);
    ESP_LOGI(TAG, "WiFi \"%s\" added: %d known", ssid, s_nwifis);
    return wifis_write();
}

void net_prov_tick(void)
{
    if (s_wifis_dirty) wifis_write();
}

bool net_prov_take_once(const char *key)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    if (nvs_get_u8(h, key, &v) == ESP_OK) {
        nvs_erase_key(h, key);
        nvs_commit(h);
    }
    nvs_close(h);
    return v == 1;
}

static void load_or_seed(void)
{
    nvs_handle_t h;
    size_t len;
    bool have = false;

    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        len = sizeof s_cfg.ssid;
        if (nvs_get_str(h, "ssid", s_cfg.ssid, &len) == ESP_OK && s_cfg.ssid[0]) {
            len = sizeof s_cfg.pass;     nvs_get_str(h, "pass", s_cfg.pass, &len);
            have = true;
        }
        len = sizeof s_cfg.radio_host; nvs_get_str(h, KEY_HOST, s_cfg.radio_host, &len);
        nvs_get_u16(h, KEY_PORT, &s_cfg.radio_port);
        len = sizeof s_cfg.radio_user; nvs_get_str(h, KEY_USER, s_cfg.radio_user, &len);
        len = sizeof s_cfg.radio_pass; nvs_get_str(h, KEY_PASS, s_cfg.radio_pass, &len);
        nvs_close(h);
    }

    /* NO CREDENTIALS ARE COMPILED IN. A unit ships with empty NVS and is
     * configured over the USB cable: it enumerates as a network adapter, hands
     * the host an address, and serves the configuration page -- none of which
     * needs WiFi. Baking a build-time SSID in would put whoever's network was
     * used to build the image into every unit flashed from it. */
    if (!have) ESP_LOGW(TAG, "no WiFi credentials stored -- USB only until the "
                             "configuration page is used");
    if (!s_cfg.radio_host[0])
        strlcpy(s_cfg.radio_host, DEFAULT_HOST, sizeof s_cfg.radio_host);
    if (!s_cfg.radio_port) s_cfg.radio_port = DEFAULT_PORT;
    if (!s_cfg.radio_user[0])
        strlcpy(s_cfg.radio_user, DEFAULT_USER, sizeof s_cfg.radio_user);
    if (!s_cfg.radio_pass[0])
        strlcpy(s_cfg.radio_pass, DEFAULT_PASS, sizeof s_cfg.radio_pass);

    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "vol",   &v) == ESP_OK) s_volume  = v;
        if (nvs_get_u8(h, "mic",   &v) == ESP_OK) s_micgain = v;
        if (nvs_get_u8(h, "mich",  &v) == ESP_OK) s_micgain_hs = v;
        if (nvs_get_u8(h, "boots", &v) == ESP_OK) s_boots   = v;
        nvs_get_u16(h, "otah", &s_ota_hours);
        nvs_get_u16(h, "dim", &s_dim_min);
        nvs_get_u16(h, "blank", &s_blank_min);
        len = sizeof s_web_user; nvs_get_str(h, "wuser", s_web_user, &len);
        len = sizeof s_web_pass; nvs_get_str(h, "wpass", s_web_pass, &len);
        nvs_close(h);
    }
    /* Count this boot straight away. If we never reach net_prov_boot_ok(),
     * the next boot sees a higher count and can back off. */
    if (s_boots < 250) s_boots++;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "boots", s_boots);
        nvs_commit(h);
        nvs_close(h);
    }
    if (s_boots > 1) ESP_LOGW(TAG, "boot #%u since last healthy run", s_boots);

    radios_load();
    wifis_load();

    /* Never log the passphrase, only whether one is present. */
    ESP_LOGI(TAG, "ssid=\"%s\" psk=%s (%d network%s known) host=%s:%u user=%s",
             s_cfg.ssid, s_cfg.pass[0] ? "set" : "EMPTY", s_nwifis, s_nwifis == 1 ? "" : "s",
             s_cfg.radio_host, (unsigned)s_cfg.radio_port,
             s_cfg.radio_user[0] ? s_cfg.radio_user : "-");
}

/* The station at network i of the list, asked to join it. */
static void sta_try(int i)
{
    wifi_config_t wc = { 0 };
    taskENTER_CRITICAL(&s_wlock);
    if (i >= s_nwifis) i = 0;
    s_try = i;
    if (s_nwifis) {
        memcpy(wc.sta.ssid, s_wifis[i].ssid, sizeof s_wifis[i].ssid);
        memcpy(wc.sta.password, s_wifis[i].pass, sizeof wc.sta.password);
    }
    taskEXIT_CRITICAL(&s_wlock);
    if (!wc.sta.ssid[0]) return;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
}

/* Joined: that network to the front, where the next boot tries it first --
 * and where a firmware from before the list finds it. */
static void wifi_joined(void)
{
    net_wifi_t w;
    taskENTER_CRITICAL(&s_wlock);
    const int i = s_try;
    const bool move = i > 0 && i < s_nwifis;
    if (move) {
        w = s_wifis[i];
        memmove(&s_wifis[1], &s_wifis[0], (size_t)i * sizeof w);
        s_wifis[0] = w;
    }
    s_try = 0;
    if (move) s_wifis_dirty = true;
    taskEXIT_CRITICAL(&s_wlock);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_try_fails = 0;
        sta_try(0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
        /* What the hotspot's page has to reach the phone with: the WiFi
         * driver takes each transmit buffer from internal RAM as it sends,
         * and short of it the page never arrives -- the phone shows it blank. */
        ESP_LOGI(TAG, "hotspot: a phone on it; free internal %u, largest %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) s_ap_clients--;
        /* The last phone gone from the hotspot: the known networks again --
         * after a network given on it failed, too. */
        if (!s_ap_clients && !s_connected && !s_hold && s_join != NET_JOIN_TRYING) {
            if (s_join == NET_JOIN_FAILED) s_join = NET_JOIN_IDLE;
            sta_try(s_try);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        const bool was = s_connected;
        s_connected = false;
        s_now[0] = 0;
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        if (s_join == NET_JOIN_TRYING) {
            if (++s_join_fails >= 4) {
                strlcpy(s_join_why, reason_text(d->reason), sizeof s_join_why);
                ESP_LOGW(TAG, "joining \"%s\" failed: %s (reason %u)",
                         s_join_ssid, s_join_why, d->reason);
                s_join = NET_JOIN_FAILED;
                return;                           /* until the portal asks again */
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_wifi_connect();
            return;
        }
        /* A phone on the hotspot: stand still, or the station's scans take
         * the hotspot off the air under it. */
        if (s_hold || s_join == NET_JOIN_FAILED || (net_prov_ap_active() && s_ap_clients > 0))
            return;
        /* Reconnect forever: this is a shack appliance, not a phone. Back off a
         * little so a wrong passphrase does not spin the radio flat out. Each
         * known network gets two tries, the one that was up a few more, then
         * the next: whichever is in reach answers. */
        if (was) s_try_fails = 0;
        int next = s_try;
        if (++s_try_fails >= (was ? 4 : 2) && s_nwifis > 1) {
            next = (s_try + 1) % s_nwifis;
            s_try_fails = 0;
        }
        int delay = s_retries < 5 ? 500 : 5000;
        if (s_retries < 1000) s_retries++;
        ESP_LOGW(TAG, "disconnected from \"%.32s\" (reason %u, attempt %d); %s in %d ms",
                 (const char *)d->ssid, d->reason, s_retries,
                 next != s_try ? "the next network" : "again", delay);
        vTaskDelay(pdMS_TO_TICKS(delay));
        sta_try(next);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_connected = true;
        s_retries   = 0;
        ESP_LOGI(TAG, "got ip " IPSTR " gw " IPSTR,
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            strlcpy(s_now, (const char *)ap.ssid, sizeof s_now);
        xEventGroupSetBits(s_events, BIT_GOT_IP);
        s_try_fails = 0;
        if (s_join == NET_JOIN_TRYING) s_join = NET_JOIN_OK;
        else wifi_joined();
    }
}

esp_err_t net_prov_join(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    strlcpy(s_join_ssid, ssid, sizeof s_join_ssid);
    strlcpy(s_join_pass, pass ? pass : "", sizeof s_join_pass);
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, s_join_ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, s_join_pass, sizeof wc.sta.password);
    s_join_fails = 0;
    s_join_why[0] = 0;
    s_join = NET_JOIN_TRYING;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    ESP_LOGI(TAG, "joining \"%s\"", s_join_ssid);
    return esp_wifi_connect();
}

net_join_t net_prov_join_state(char *ssid, size_t sn, char *why, size_t wn)
{
    if (ssid) strlcpy(ssid, s_join_ssid, sn);
    if (why)  strlcpy(why, s_join_why, wn);
    return s_join;
}

esp_err_t net_prov_join_keep(void)
{
    if (s_join != NET_JOIN_OK) return ESP_ERR_INVALID_STATE;
    s_join = NET_JOIN_IDLE;
    /* Beside the ones known, not instead: home, and a phone's hotspot. */
    return net_prov_wifi_add(s_join_ssid, s_join_pass);
}

void net_prov_hold_station(bool hold)
{
    s_hold = hold;
    if (!hold && !s_connected && s_join != NET_JOIN_TRYING) esp_wifi_connect();
}

esp_err_t net_prov_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");
    s_wmx = xSemaphoreCreateMutex();
    load_or_seed();

    /* The TCP/IP stack and the default event loop are prerequisites for ANY
     * netif -- WiFi, USB-NCM or the log server -- so they belong here, not in
     * whichever transport happens to start first. Having them in
     * net_prov_wifi_start() meant usb_net_init(), which runs earlier, called
     * esp_netif_new() against an uninitialised stack and quietly failed; the
     * knob then fell back to WiFi and looked like USB was simply unsupported.
     * lwIP is unforgiving here -- a socket created before its thread exists
     * asserts ("Invalid mbox") rather than returning an error. */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t lerr = esp_event_loop_create_default();
    if (lerr != ESP_OK && lerr != ESP_ERR_INVALID_STATE)
        ESP_RETURN_ON_ERROR(lerr, TAG, "evt loop");
    return ESP_OK;
}

const vfo_cfg_t *net_prov_cfg(void) { return &s_cfg; }
uint8_t net_prov_volume(void)   { return s_volume; }
uint8_t net_prov_mic_gain(void) { return s_micgain; }
uint8_t net_prov_mic_gain_headset(void) { return s_micgain_hs; }

uint8_t net_prov_boot_count(void) { return s_boots; }

void net_prov_boot_ok(void)
{
    if (!s_boots) return;
    s_boots = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "boots", 0);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "boot considered healthy; loop counter cleared");
}

static bool s_audio_dirty;

void net_prov_set_audio(uint8_t volume, uint8_t mic_gain, uint8_t mic_gain_headset)
{
    if (volume == s_volume && mic_gain == s_micgain && mic_gain_headset == s_micgain_hs) return;
    s_volume = volume;
    s_micgain = mic_gain;
    s_micgain_hs = mic_gain_headset;
    s_audio_dirty = true;
}

void net_prov_flush_audio(void)
{
    if (!s_audio_dirty) return;
    s_audio_dirty = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "vol", s_volume);
    nvs_set_u8(h, "mic", s_micgain);
    nvs_set_u8(h, "mich", s_micgain_hs);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved volume=%u mic=%u headset mic=%u", s_volume, s_micgain, s_micgain_hs);
}

void net_prov_save_audio(uint8_t volume, uint8_t mic_gain, uint8_t mic_gain_headset)
{
    if (volume == s_volume && mic_gain == s_micgain && mic_gain_headset == s_micgain_hs &&
        !s_audio_dirty) return;
    s_audio_dirty = false;
    s_volume = volume;
    s_micgain = mic_gain;
    s_micgain_hs = mic_gain_headset;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "vol", volume);
    nvs_set_u8(h, "mic", mic_gain);
    nvs_set_u8(h, "mich", mic_gain_headset);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved volume=%u mic=%u headset mic=%u", volume, mic_gain, mic_gain_headset);
}
bool net_prov_is_connected(void)    { return s_connected; }

/* Shuts the radio down and hands its internal RAM back.
 *
 * Worth doing, not just tidy: the WiFi driver's static RX descriptors live in
 * internal DMA-capable RAM, which is the one resource this board never has
 * enough of. With WiFi and USB-NCM both up there was not enough left for the
 * TCI client's 4 kB transmit task -- the knob connected, received, and could
 * not send a single command. */
/* Persist the endpoint and credentials written by the configuration page. */
esp_err_t net_prov_save_cfg(const vfo_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    /* A network given the old way, one at a time: to the list. */
    if (cfg->ssid[0] && (strcmp(cfg->ssid, s_cfg.ssid) || strcmp(cfg->pass, s_cfg.pass)))
        net_prov_wifi_add(cfg->ssid, cfg->pass);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, KEY_HOST, cfg->radio_host);
    nvs_set_u16(h, KEY_PORT, cfg->radio_port);
    nvs_set_str(h, KEY_USER, cfg->radio_user);
    nvs_set_str(h, KEY_PASS, cfg->radio_pass);
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        /* The network is the list's to say. */
        char ssid[33], pass[65];
        memcpy(ssid, s_cfg.ssid, sizeof ssid);
        memcpy(pass, s_cfg.pass, sizeof pass);
        s_cfg = *cfg;
        memcpy(s_cfg.ssid, ssid, sizeof ssid);
        memcpy(s_cfg.pass, pass, sizeof pass);
        /* The endpoint given this way is the radio in use: the list follows. */
        if (s_nradios > 0) {
            net_radio_t *r = &s_radios[s_radio_sel];
            if (strcmp(r->host, cfg->radio_host) || r->port != cfg->radio_port ||
                strcmp(r->user, cfg->radio_user) || strcmp(r->pass, cfg->radio_pass)) {
                cfg_to_radio(r);
                err = radios_write();
            }
        }
    }
    return err;
}

uint16_t net_prov_ota_hours(void) { return s_ota_hours; }
uint16_t net_prov_dim_min(void)   { return s_dim_min; }
uint16_t net_prov_blank_min(void) { return s_blank_min; }

void net_prov_save_dim(uint16_t dim_minutes, uint16_t blank_minutes)
{
    /* Blanking before dimming is not a setting, it is a typo. Anything
     * non-zero below the dim time is pulled up to it so the two stages stay
     * in the order the operator meant. */
    if (blank_minutes && dim_minutes && blank_minutes < dim_minutes)
        blank_minutes = dim_minutes;
    if (dim_minutes == s_dim_min && blank_minutes == s_blank_min) return;
    s_dim_min   = dim_minutes;
    s_blank_min = blank_minutes;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "dim", s_dim_min);
    nvs_set_u16(h, "blank", s_blank_min);
    nvs_commit(h);
    nvs_close(h);
}

void net_prov_save_ota_hours(uint16_t hours)
{
    if (hours == s_ota_hours) return;
    s_ota_hours = hours;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "otah", hours);
    nvs_commit(h);
    nvs_close(h);
}

const char *net_prov_web_user(void) { return s_web_user; }
const char *net_prov_web_pass(void) { return s_web_pass; }

bool net_prov_web_is_default(void)
{
    return strcmp(s_web_user, "admin") == 0 && strcmp(s_web_pass, "admin") == 0;
}

void net_prov_save_web(const char *user, const char *pass)
{
    if (user && *user) strlcpy(s_web_user, user, sizeof s_web_user);
    if (pass && *pass) strlcpy(s_web_pass, pass, sizeof s_web_pass);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "wuser", s_web_user);
    nvs_set_str(h, "wpass", s_web_pass);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t net_prov_wifi_stop(void)
{
    s_connected = false;
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_INIT) return ESP_OK;
    if (err == ESP_OK) err = esp_wifi_deinit();
    return err;
}

esp_err_t net_prov_wifi_start(void)
{
    s_events = xEventGroupCreate();
    /* esp_netif and the default event loop are set up in net_prov_init(). */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&ic), TAG, "wifi init");
    /* The driver's own copy of the station's settings stays in RAM: the knob
     * keeps its networks in its own namespace and sets them at every start.
     * In flash, esp_wifi_set_config() writes NVS whenever a setting differs,
     * and on a knob whose NVS had filled up -- switched through several
     * firmwares -- that write failed, and with it the whole WiFi start: no
     * network, and no hotspot to set one up (2026-10-02). */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid,     s_cfg.ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, s_cfg.pass, sizeof wc.sta.password);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "cfg");
    /* Keep the radio off the power-save duty cycle: it adds tens of ms of
     * jitter, and this device keys a transmitter. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    /* Advertise ourselves as a TCI peripheral. AetherSDR's browse side is not
     * implemented yet, so this does nothing today -- but it costs ~20 lines,
     * works the day that lands, and makes the knob visible to avahi-browse
     * while debugging, which is immediately useful. */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("vfo-knob");
        mdns_instance_name_set("VFO-Knob");
        mdns_txt_item_t txt[] = {
            { "txtvers",     "1"             },
            { "model",       "vfo-knob"      },
            { "class",       "controller"    },
            { "tci-version", "1.5"           },
        };
#if VFO_RADIO_AETHERSDR
        mdns_service_add(NULL, "_tci", "_tcp", s_cfg.radio_port, txt,
                         sizeof txt / sizeof txt[0]);
#else
        (void)txt;
#endif
    }
    return ESP_OK;
}

esp_err_t net_prov_resolve(char *out, size_t out_len)
{
    const char *host = s_cfg.radio_host;
    size_t n = strlen(host);

    if (n > 6 && strcmp(host + n - 6, ".local") == 0) {
        char base[64];
        strlcpy(base, host, sizeof base);
        base[n - 6] = '\0';
        esp_ip4_addr_t addr = { 0 };
        if (mdns_query_a(base, 2000, &addr) == ESP_OK) {
            snprintf(out, out_len, IPSTR, IP2STR(&addr));
            return ESP_OK;
        }
        ESP_LOGW(TAG, "mDNS could not resolve %s", host);
        return ESP_FAIL;
    }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return ESP_FAIL;
    struct in_addr a = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    inet_ntoa_r(a, out, out_len);
    freeaddrinfo(res);
    return ESP_OK;
}
