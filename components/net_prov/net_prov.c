#include "net_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "kvstore.h"
#if VFO_RADIO_ADSB
#include "adsb.h"
#endif
#if VFO_RADIO_WEATHER
#include "weather.h"
#endif
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
#elif VFO_RADIO_EPIBEAM
/* EpiCenter's address, for its WebSocket when it comes (components/
 * epi_client). Keys of its own, so it never writes another firmware's. */
#define KEY_HOST     "ephost"
#define KEY_RLIST    "eplist"
#define KEY_RSEL     "epsel"
#define KEY_PORT     "epport"
#define KEY_USER     "epuser"
#define KEY_PASS     "eppass"
#define DEFAULT_HOST ""
#define DEFAULT_PORT 80
#define DEFAULT_USER ""
#define DEFAULT_PASS ""
#elif VFO_RADIO_ADSB
/* No radio: the feed is the radar's own (net_prov_adsb). Keys of its own all
 * the same, so it never writes another firmware's. */
#define KEY_HOST     "abhost"
#define KEY_RLIST    "ablist"
#define KEY_RSEL     "absel"
#define KEY_PORT     "abport"
#define KEY_USER     "abuser"
#define KEY_PASS     "abpass"
#define DEFAULT_HOST ""
#define DEFAULT_PORT 80
#define DEFAULT_USER ""
#define DEFAULT_PASS ""
#elif VFO_RADIO_WEATHER
/* No radio: the forecast's place is the weather's own (net_prov_weather).
 * Keys of its own all the same, so it never writes another firmware's. */
#define KEY_HOST     "wxhost"
#define KEY_RLIST    "wxlist"
#define KEY_RSEL     "wxsel"
#define KEY_PORT     "wxport"
#define KEY_USER     "wxuser"
#define KEY_PASS     "wxpass"
#define DEFAULT_HOST ""
#define DEFAULT_PORT 80
#define DEFAULT_USER ""
#define DEFAULT_PASS ""
#elif VFO_RADIO_CLOCK
/* No radio, nor anything to ask: the time is SNTP's (components/wallclock).
 * Keys of its own all the same, so it never writes another firmware's. */
#define KEY_HOST     "ckhost"
#define KEY_RLIST    "cklist"
#define KEY_RSEL     "cksel"
#define KEY_PORT     "ckport"
#define KEY_USER     "ckuser"
#define KEY_PASS     "ckpass"
#define DEFAULT_HOST ""
#define DEFAULT_PORT 80
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
#define DEFAULT_HOST ""              /* given on the page: https://<name>.tunnel.ubersdr.org,
                                        or http://<host> on a LAN -- the scheme kept */
#define DEFAULT_PORT 443             /* its tunnel's https; 8080 on a LAN */
#define DEFAULT_USER ""
#define DEFAULT_PASS ""
#elif VFO_RADIO_KIWI
/* Unused: its receivers are the web SDRs' list (components/sdr_rx), each
 * with its own address and passwords. Keys of its own, so it never writes
 * another firmware's. */
#define KEY_HOST     "kwhost"
#define KEY_RLIST    "kwrlist"
#define KEY_RSEL     "kwrsel"
#define KEY_PORT     "kwport"
#define KEY_USER     "kwuser"
#define KEY_PASS     "kwpass"
#define DEFAULT_HOST ""
#define DEFAULT_PORT 8073            /* most KiwiSDRs' */
#define DEFAULT_USER ""
#define DEFAULT_PASS ""
#elif VFO_RADIO_OWRX
/* Its receivers, each by its address whole -- "https://fms.komkon.org/OWRX/",
 * "http://sdr.on4pra.be/", "host:8073" -- scheme, port and path in the host,
 * as the page takes it: the client reads each (owrx_url). No user, no
 * password: OpenWebRX asks a listener for neither. */
#define KEY_HOST     "owhost"
#define KEY_RLIST    "owlist"
#define KEY_RSEL     "owsel"
#define KEY_PORT     "owport"
#define KEY_USER     "owuser"
#define KEY_PASS     "owpass"
#define DEFAULT_HOST ""
#define DEFAULT_PORT 8073            /* OpenWebRX's own */
#define DEFAULT_USER ""
#define DEFAULT_PASS ""
#elif VFO_RADIO_WEBSDR
/* Its receivers, each by its address whole -- "http://websdr.ewi.utwente.nl:
 * 8901/", "http://sdr.websdrmaasbree.nl:8901/" -- as the OpenWebRX firmware
 * keeps its own (owrx_url reads both). No user, no password: a WebSDR asks a
 * listener for neither. */
#define KEY_HOST     "wshost"
#define KEY_RLIST    "wslist"
#define KEY_RSEL     "wssel"
#define KEY_PORT     "wsport"
#define KEY_USER     "wsuser"
#define KEY_PASS     "wspass"
#define DEFAULT_HOST ""
#define DEFAULT_PORT 8901            /* most WebSDRs' */
#define DEFAULT_USER ""
#define DEFAULT_PASS ""
#elif VFO_RADIO_OPENX6X00
#define KEY_HOST     "oxhost"
#define KEY_RLIST    "oxlist"    /* the radios, one in use: see below */
#define KEY_RSEL     "oxsel"
#define KEY_PORT     "oxport"
#define KEY_USER     "oxuser"    /* unused: its TCI server has no login */
#define KEY_PASS     "oxpass"
#define DEFAULT_HOST "x6100.local"   /* OpenX6X00 announces it over Avahi (rootfs/etc/avahi) */
#define DEFAULT_PORT 50001           /* OpenX6X00's TCI server (src/tci_server.h) */
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
static kv_handle_t s_kv;            /* the same namespace's settings, on the SD card (kvstore) */

/* NULL until kvstore is up: its calls then do nothing. */
static kv_handle_t kv(void)
{
    if (!s_kv) kv_open(NVS_NS, &s_kv);
    return s_kv;
}

/* A page's save: ESP_OK once written -- or still on its way (the timeout),
 * which is as good as saved; ESP_FAIL when no medium took it. */
static esp_err_t kv_saved_or(esp_err_t e, const char *what)
{
    if (e == ESP_ERR_TIMEOUT) return ESP_OK;
    if (e != ESP_OK) ESP_LOGE(TAG, "%s not saved: %s", what, esp_err_to_name(e));
    return e;
}

static uint16_t s_ota_hours = 24;   /* automatic update check; 0 = off */
/* The screensaver after a minute, dimmed a minute later, dark a minute after
 * that. The Clock and the Weather are screens to look at: theirs stays up. */
#if VFO_RADIO_CLOCK || VFO_RADIO_WEATHER
static net_screen_t s_screen = { NET_SAVER_NONE, 0, 0, 0 };
#else
static net_screen_t s_screen = { NET_SAVER_MATRIX, 1, 1, 1 };
#endif
static char     s_tz_name[NET_TZ_NAME_LEN] = "Europe/Brussels";
static char     s_tz_rule[NET_TZ_RULE_LEN] = "CET-1CEST,M3.5.0,M10.5.0/3";
static bool     s_h24 = true;
static uint8_t  s_ck_face;          /* the clock's face, CF_* */
static bool     s_ck_sec = true;    /* ...with a second hand */
static void     screen_load(kv_handle_t h);
static uint8_t  s_bright = 80;      /* the screen lit, percent: 80 is the 200 of 255 it always was */
static bool     s_turned;           /* the USB cable at the bottom: the screen turned 180 degrees */
static uint8_t  s_beam_n = 4;       /* EpiBeam: the array's directions, 4 or 8 */
static uint8_t  s_beam_dir;         /* ...and the one the beam was left on, 0 = N */
static uint8_t  s_jack_db;          /* the jack's level above the VOLUME, dB */
static uint8_t  s_tx_mic;           /* NET_TX_*: what AetherSDR transmits with */
#if VFO_RADIO_ADSB
/* ADS-B Radar's radar: in PSRAM, as large as it is beside the rest. */
EXT_RAM_BSS_ATTR static net_adsb_t s_adsb;
static portMUX_TYPE s_adsb_lock = portMUX_INITIALIZER_UNLOCKED;
static void adsb_load(kv_handle_t h);
#endif
#if VFO_RADIO_CLOCK
static net_alarms_t s_alarms = { .snooze_min = 9, .ring_min = 10, .volume = 60 };
static portMUX_TYPE s_alarms_lock = portMUX_INITIALIZER_UNLOCKED;
static void alarms_load(kv_handle_t h);
#endif
#if VFO_RADIO_WEATHER
EXT_RAM_BSS_ATTR static net_weather_t s_wx;
static portMUX_TYPE s_wx_lock = portMUX_INITIALIZER_UNLOCKED;
static void weather_load(kv_handle_t h);
#endif

/* Credentials for the configuration page. Shipped as admin/admin so a new
 * owner can get in; over the network the knob then does nothing more until
 * the password is one of their own (webcfg's pw_forced) -- this device can
 * key a transmitter, so leaving the default in place on someone else's
 * network is not a small thing. */
static char s_web_user[24] = "admin";
static char s_web_pass[33] = "admin";

static vfo_cfg_t          s_cfg;
/* The radios the knob knows, the one in use feeding s_cfg's endpoint. */
EXT_RAM_BSS_ATTR static net_radio_t s_radios[NET_PROV_RADIOS];
static int                s_nradios, s_radio_sel;
static volatile uint32_t  s_radios_gen;      /* each list saved */
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
    int8_t sel = 0;
    s_nradios = 0;
    size_t n = 0;
    char *blob = kv_dup(kv(), KEY_RLIST, &n);
    if (blob && n) radios_parse(blob);
    free(blob);
    kv_get_i8(kv(), KEY_RSEL, &sel);
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
static esp_err_t radios_write(bool wait)
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
    /* Every write's word: one refused is the list not saved, said as such --
     * never an OK the next boot finds untrue. The six reach the medium together. */
    kv_handle_t h = kv();
    kv_edit_begin(h);
    esp_err_t e = kv_set_str(h, KEY_RLIST, blob);
    if (e == ESP_OK) e = kv_set_i8(h, KEY_RSEL, (int8_t)s_radio_sel);
    if (e == ESP_OK) e = kv_set_str(h, KEY_HOST, s_cfg.radio_host);
    if (e == ESP_OK) e = kv_set_u16(h, KEY_PORT, s_cfg.radio_port);
    if (e == ESP_OK) e = kv_set_str(h, KEY_USER, s_cfg.radio_user);
    if (e == ESP_OK) e = kv_set_str(h, KEY_PASS, s_cfg.radio_pass);
    kv_edit_end(h);
    free(blob);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "the radios not saved: %s", esp_err_to_name(e));
        return e;
    }
    /* The one in use changed from a timer, or before a restart: written at
     * once, not waited for (a restart writes what waits first). */
    if (!wait) return kv_commit_now(h);
    return kv_saved_or(kv_commit_wait(h, 3000), "the radios");
}

int  net_prov_radio_count(void)  { return s_nradios; }
int  net_prov_radio_active(void) { return s_radio_sel; }
uint32_t net_prov_radios_gen(void) { return s_radios_gen; }

bool net_prov_radio_known(const char *host)
{
    for (int i = 0; host && host[0] && i < s_nradios; i++)
        if (!strcasecmp(s_radios[i].host, host)) return true;
    return false;
}

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
    s_radios_gen++;
    radio_to_cfg(&s_radios[active]);
    const esp_err_t e = radios_write(true);
    if (e == ESP_OK)
        ESP_LOGI(TAG, "%d radio%s saved; in use: %s", n, n == 1 ? "" : "s",
                 list[active].name[0] ? list[active].name : list[active].host);
    return e;
}

esp_err_t net_prov_radio_activate(int i)
{
    if (i < 0 || i >= s_nradios) return ESP_ERR_INVALID_ARG;
    s_radio_sel = i;
    radio_to_cfg(&s_radios[i]);
    return radios_write(false);
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

bool net_prov_peek(const char *key)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_u8(h, key, &v);
    nvs_close(h);
    return v == 1;
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

/* What stays in NVS: the WiFi, and the boot counter. */
static void load_nvs(void)
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
        uint8_t v;
        if (nvs_get_u8(h, "boots", &v) == ESP_OK) s_boots = v;
        nvs_close(h);
    }

    /* NO CREDENTIALS ARE COMPILED IN. A unit ships with empty NVS and is
     * configured over the USB cable: it enumerates as a network adapter, hands
     * the host an address, and serves the configuration page -- none of which
     * needs WiFi. Baking a build-time SSID in would put whoever's network was
     * used to build the image into every unit flashed from it. */
    if (!have) ESP_LOGW(TAG, "no WiFi credentials stored -- USB only until the "
                             "configuration page is used");
    /* Count this boot straight away. If we never reach net_prov_boot_ok(),
     * the next boot sees a higher count and can back off. Not a start that a
     * computer on the cable asked for, by opening the S3's USB port --
     * esptool, the website's bench and WiFi pages: no crash ever resets it
     * that way, and three of them inside 20 s each took a knob being
     * provisioned on the bench into safe mode. */
    const bool by_usb = esp_reset_reason() == ESP_RST_USB;
    if (!by_usb && s_boots < 250) s_boots++;
    if (!by_usb && nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "boots", s_boots);
        nvs_commit(h);
        nvs_close(h);
    }
    if (s_boots > 1) ESP_LOGW(TAG, "boot #%u since last healthy run%s", s_boots,
                              by_usb ? " (this one, by the USB cable, not counted)" : "");
    wifis_load();
}

void net_prov_load_settings(void)
{
    kv_handle_t h = kv();
    size_t len;
    len = sizeof s_cfg.radio_host; kv_get_str(h, KEY_HOST, s_cfg.radio_host, &len);
    kv_get_u16(h, KEY_PORT, &s_cfg.radio_port);
    len = sizeof s_cfg.radio_user; kv_get_str(h, KEY_USER, s_cfg.radio_user, &len);
    len = sizeof s_cfg.radio_pass; kv_get_str(h, KEY_PASS, s_cfg.radio_pass, &len);
    if (!s_cfg.radio_host[0])
        strlcpy(s_cfg.radio_host, DEFAULT_HOST, sizeof s_cfg.radio_host);
    if (!s_cfg.radio_port) s_cfg.radio_port = DEFAULT_PORT;
    if (!s_cfg.radio_user[0])
        strlcpy(s_cfg.radio_user, DEFAULT_USER, sizeof s_cfg.radio_user);
    if (!s_cfg.radio_pass[0])
        strlcpy(s_cfg.radio_pass, DEFAULT_PASS, sizeof s_cfg.radio_pass);

    uint8_t v;
    if (kv_get_u8(h, "vol",  &v) == ESP_OK) s_volume  = v;
    if (kv_get_u8(h, "mic",  &v) == ESP_OK) s_micgain = v;
    if (kv_get_u8(h, "mich", &v) == ESP_OK) s_micgain_hs = v;
    kv_get_u16(h, "otah", &s_ota_hours);
    screen_load(h);
    len = sizeof s_tz_name; kv_get_str(h, "tz", s_tz_name, &len);
    len = sizeof s_tz_rule; kv_get_str(h, "tzrule", s_tz_rule, &len);
    if (kv_get_u8(h, "h24", &v) == ESP_OK) s_h24 = v != 0;
    if (kv_get_u8(h, "ckface", &v) == ESP_OK && v < NET_CLOCK_FACES) s_ck_face = v;
    if (kv_get_u8(h, "cksec", &v) == ESP_OK) s_ck_sec = v != 0;
    if (kv_get_u8(h, "bright", &v) == ESP_OK && v >= NET_PROV_BRIGHT_MIN && v <= 100) s_bright = v;
    if (kv_get_u8(h, "turn", &v) == ESP_OK && v <= 1) s_turned = v;
    if (kv_get_u8(h, "beamn", &v) == ESP_OK && (v == 4 || v == 8)) s_beam_n = v;
    if (kv_get_u8(h, "beamd", &v) == ESP_OK && v < s_beam_n) s_beam_dir = v;
    if (kv_get_u8(h, "jackdb", &v) == ESP_OK && v <= NET_PROV_JACK_DB_MAX && v % 3 == 0) s_jack_db = v;
    if (kv_get_u8(h, "txmic", &v) == ESP_OK && v <= NET_TX_RADIO) s_tx_mic = v;
#if VFO_RADIO_ADSB
    adsb_load(h);
#endif
#if VFO_RADIO_WEATHER
    weather_load(h);
#endif
#if VFO_RADIO_CLOCK
    alarms_load(h);
#endif
    len = sizeof s_web_user; kv_get_str(h, "wuser", s_web_user, &len);
    len = sizeof s_web_pass; kv_get_str(h, "wpass", s_web_pass, &len);

    radios_load();

    /* Never log the passphrase, only whether one is present. */
    ESP_LOGI(TAG, "ssid=\"%s\" psk=%s (%d network%s known) host=%s:%u user=%s",
             s_cfg.ssid, s_cfg.pass[0] ? "set" : "EMPTY", s_nwifis, s_nwifis == 1 ? "" : "s",
             s_cfg.radio_host, (unsigned)s_cfg.radio_port,
             s_cfg.radio_user[0] ? s_cfg.radio_user : "-");
}

/* A network of several access points under one name -- a mesh, a house
 * with a repeater -- is joined at its strongest. Left at ESP-IDF's default,
 * a fast scan, the station took the first it heard going up the channels:
 * one at -91 dBm on channel 1 over one at -65 on channel 6 (2026-10-21),
 * where nothing got through and the link came and went for minutes. A scan
 * of every channel costs a second or two at each join. */
static void sta_strongest(wifi_config_t *wc)
{
    wc->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
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
    sta_strongest(&wc);
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
    sta_strongest(&wc);
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
    load_nvs();

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
    kv_handle_t h = kv();
    kv_edit_begin(h);
    kv_set_u8(h, "vol", s_volume);
    kv_set_u8(h, "mic", s_micgain);
    kv_set_u8(h, "mich", s_micgain_hs);
    kv_edit_end(h);
    kv_commit(h);
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
    kv_handle_t h = kv();
    kv_edit_begin(h);
    kv_set_u8(h, "vol", volume);
    kv_set_u8(h, "mic", mic_gain);
    kv_set_u8(h, "mich", mic_gain_headset);
    kv_edit_end(h);
    kv_commit_now(h);
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
    kv_handle_t h = kv();
    kv_edit_begin(h);
    esp_err_t err = kv_set_str(h, KEY_HOST, cfg->radio_host);
    if (err == ESP_OK) err = kv_set_u16(h, KEY_PORT, cfg->radio_port);
    if (err == ESP_OK) err = kv_set_str(h, KEY_USER, cfg->radio_user);
    if (err == ESP_OK) err = kv_set_str(h, KEY_PASS, cfg->radio_pass);
    kv_edit_end(h);
    if (err == ESP_OK) err = kv_saved_or(kv_commit_wait(h, 3000), "the radio's address");
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
                err = radios_write(true);
            }
        }
    }
    return err;
}

uint16_t net_prov_ota_hours(void) { return s_ota_hours; }
/* Before the screensaver the page had "Dim after" and "Go dark after", both
 * counted from the last use, stored only once changed. An owner who chose
 * them keeps those moments: the screensaver where the dial dimmed, dimmed a
 * minute later, dark when it went dark. The old keys are left alone. */
static void screen_load(kv_handle_t h)
{
    uint8_t  kind;
    uint16_t m;
    if (kv_get_u8(h, "ss", &kind) == ESP_OK) {
        if (kind < NET_SAVER_N) s_screen.saver = kind;
        if (kv_get_u16(h, "ssa", &m) == ESP_OK) s_screen.after = m;
        if (kv_get_u16(h, "ssd", &m) == ESP_OK) s_screen.dim   = m;
        if (kv_get_u16(h, "ssb", &m) == ESP_OK) s_screen.dark  = m;
        return;
    }
    uint16_t dim = 5, blank = 10;
    const bool had_dim   = kv_get_u16(h, "dim",   &dim)   == ESP_OK;
    const bool had_blank = kv_get_u16(h, "blank", &blank) == ESP_OK;
    if (!had_dim && !had_blank) return;
    s_screen.after = dim;
    s_screen.dim   = dim ? 1 : 0;
    const uint16_t at = dim + s_screen.dim;     /* the moment it dims, or the saver's */
    s_screen.dark  = !blank ? 0 : blank > at ? blank - at : 1;
}

void net_prov_screen(net_screen_t *out) { *out = s_screen; }

void net_prov_save_screen(const net_screen_t *s)
{
    net_screen_t n = *s;
    if (n.saver >= NET_SAVER_N) n.saver = NET_SAVER_MATRIX;
    if (n.after > NET_SCREEN_MAX_MIN) n.after = NET_SCREEN_MAX_MIN;
    if (n.dim   > NET_SCREEN_MAX_MIN) n.dim   = NET_SCREEN_MAX_MIN;
    if (n.dark  > NET_SCREEN_MAX_MIN) n.dark  = NET_SCREEN_MAX_MIN;
    if (!memcmp(&n, &s_screen, sizeof n)) return;
    s_screen = n;
    kv_handle_t h = kv();
    kv_set_u8 (h, "ss",  n.saver);
    kv_set_u16(h, "ssa", n.after);
    kv_set_u16(h, "ssd", n.dim);
    kv_set_u16(h, "ssb", n.dark);
    kv_commit_now(h);
}

const char *net_prov_tz_name(void) { return s_tz_name; }
const char *net_prov_tz_rule(void) { return s_tz_rule; }
bool        net_prov_clock24(void) { return s_h24; }

void net_prov_save_time(const char *tz_name, const char *tz_rule, bool h24)
{
    kv_handle_t h = kv();
    if (tz_name && tz_rule && tz_name[0] && tz_rule[0] &&
        (strcmp(tz_name, s_tz_name) || strcmp(tz_rule, s_tz_rule))) {
        strlcpy(s_tz_name, tz_name, sizeof s_tz_name);
        strlcpy(s_tz_rule, tz_rule, sizeof s_tz_rule);
        kv_set_str(h, "tz", s_tz_name);
        kv_set_str(h, "tzrule", s_tz_rule);
    }
    if (h24 != s_h24) {
        s_h24 = h24;
        kv_set_u8(h, "h24", h24);
    }
    kv_commit_now(h);
}

uint8_t net_prov_clock_face(void)    { return s_ck_face; }
bool    net_prov_clock_seconds(void) { return s_ck_sec; }

void net_prov_save_clock(uint8_t face, bool seconds)
{
    if (face >= NET_CLOCK_FACES || (face == s_ck_face && seconds == s_ck_sec)) return;
    kv_handle_t h = kv();
    if (face != s_ck_face) { s_ck_face = face; kv_set_u8(h, "ckface", face); }
    if (seconds != s_ck_sec) { s_ck_sec = seconds; kv_set_u8(h, "cksec", seconds); }
    kv_commit_now(h);
}

uint8_t net_prov_brightness(void) { return s_bright; }

void net_prov_save_brightness(uint8_t percent)
{
    if (percent < NET_PROV_BRIGHT_MIN) percent = NET_PROV_BRIGHT_MIN;
    if (percent > 100) percent = 100;
    if (percent == s_bright) return;
    s_bright = percent;
    kv_set_u8(kv(), "bright", percent);
    kv_commit_now(kv());
}

bool net_prov_turned(void) { return s_turned; }

void net_prov_save_turned(bool turned)
{
    if (turned == s_turned) return;
    s_turned = turned;
    kv_set_u8(kv(), "turn", turned);
    kv_commit_now(kv());
}

uint8_t net_prov_beam_dirs(void) { return s_beam_n; }
uint8_t net_prov_beam_dir(void)  { return s_beam_dir; }

void net_prov_save_beam_dirs(uint8_t n)
{
    if ((n != 4 && n != 8) || n == s_beam_n) return;
    s_beam_n = n;
    if (s_beam_dir >= n) s_beam_dir = 0;
    kv_set_u8(kv(), "beamn", n);
    kv_commit_now(kv());
}

void net_prov_save_beam_dir(uint8_t dir)
{
    if (dir >= s_beam_n || dir == s_beam_dir) return;
    s_beam_dir = dir;
    kv_set_u8(kv(), "beamd", dir);
    kv_commit_now(kv());
}

#if VFO_RADIO_ADSB
/* Each its own key, so a page that changes one writes one. */
static void adsb_load(kv_handle_t h)
{
    net_adsb_t a = {
        .lat_e6 = NET_ADSB_HOME_LAT_E6, .lon_e6 = NET_ADSB_HOME_LON_E6, .centre = NET_ADSB_HOME,
        .source = ADSB_SRC_ADSB_FI, .range = 4, .opts = NET_ADSB_OPTS_DEFAULT, .refresh_s = 10,
    };
    int32_t la, lo;
    if (kv_get_i32(h, "adlat", &la) == ESP_OK && kv_get_i32(h, "adlon", &lo) == ESP_OK &&
        la >= -90000000 && la <= 90000000 && lo >= -180000000 && lo <= 180000000) {
        a.lat_e6 = la;
        a.lon_e6 = lo;
    }
    size_t len = sizeof a.centre;  kv_get_str(h, "adctr", a.centre, &len);
    len = sizeof a.url;            kv_get_str(h, "adurl", a.url, &len);
    len = sizeof a.types;          kv_get_str(h, "adtypes", a.types, &len);
    uint8_t v;
    if (kv_get_u8(h, "adsrc", &v) == ESP_OK && adsb_src_ok(v))      a.source = v;
    if (kv_get_u8(h, "adunit", &v) == ESP_OK && adsb_units_ok(v))   a.units = v;
    if (kv_get_u8(h, "adrng", &v) == ESP_OK && v < ADSB_RANGES)     a.range = v;
    if (kv_get_u8(h, "adshow", &v) == ESP_OK && adsb_show_ok(v))    a.show = v;
    if (kv_get_u8(h, "adopt", &v) == ESP_OK && v < 16)              a.opts = v;
    if (kv_get_u8(h, "adrefr", &v) == ESP_OK && v >= NET_ADSB_REFRESH_MIN && v <= NET_ADSB_REFRESH_MAX)
        a.refresh_s = v;
    portENTER_CRITICAL(&s_adsb_lock);
    s_adsb = a;
    portEXIT_CRITICAL(&s_adsb_lock);
}

void net_prov_adsb(net_adsb_t *out)
{
    portENTER_CRITICAL(&s_adsb_lock);
    *out = s_adsb;
    portEXIT_CRITICAL(&s_adsb_lock);
}

void net_prov_save_adsb(const net_adsb_t *in)
{
    net_adsb_t was, a = *in;
    net_prov_adsb(&was);
    const bool pos = a.lat_e6 >= -90000000 && a.lat_e6 <= 90000000 &&
                     a.lon_e6 >= -180000000 && a.lon_e6 <= 180000000;
    if (!pos) { a.lat_e6 = was.lat_e6; a.lon_e6 = was.lon_e6; memcpy(a.centre, was.centre, sizeof a.centre); }
    if (!adsb_src_ok(a.source))   a.source = was.source;
    if (!adsb_units_ok(a.units))  a.units = was.units;
    if (a.range >= ADSB_RANGES)   a.range = was.range;
    if (!adsb_show_ok(a.show))    a.show = was.show;
    if (a.opts >= 16)             a.opts = was.opts;
    if (a.refresh_s < NET_ADSB_REFRESH_MIN || a.refresh_s > NET_ADSB_REFRESH_MAX) a.refresh_s = was.refresh_s;
    a.centre[sizeof a.centre - 1] = 0;
    a.url[sizeof a.url - 1] = 0;
    a.types[sizeof a.types - 1] = 0;
    if (!memcmp(&a, &was, sizeof a)) return;
    portENTER_CRITICAL(&s_adsb_lock);
    s_adsb = a;
    portEXIT_CRITICAL(&s_adsb_lock);

    kv_handle_t h = kv();
    if (a.lat_e6 != was.lat_e6 || a.lon_e6 != was.lon_e6) {
        kv_set_i32(h, "adlat", a.lat_e6);
        kv_set_i32(h, "adlon", a.lon_e6);
    }
    if (strcmp(a.centre, was.centre)) kv_set_str(h, "adctr", a.centre);
    if (strcmp(a.url, was.url))       kv_set_str(h, "adurl", a.url);
    if (strcmp(a.types, was.types))   kv_set_str(h, "adtypes", a.types);
    if (a.source != was.source)       kv_set_u8(h, "adsrc", a.source);
    if (a.units != was.units)         kv_set_u8(h, "adunit", a.units);
    if (a.range != was.range)         kv_set_u8(h, "adrng", a.range);
    if (a.show != was.show)           kv_set_u8(h, "adshow", a.show);
    if (a.opts != was.opts)           kv_set_u8(h, "adopt", a.opts);
    if (a.refresh_s != was.refresh_s) kv_set_u8(h, "adrefr", a.refresh_s);
    kv_commit_now(h);
}
#endif

#if VFO_RADIO_WEATHER
static bool wx_pos_ok(int32_t la, int32_t lo)
{
    return la >= -90000000 && la <= 90000000 && lo >= -180000000 && lo <= 180000000;
}

/* Each its own key, so a page that changes one writes one. */
static void weather_load(kv_handle_t h)
{
    net_weather_t w = {
        .lat_e6 = NET_WX_HOME_LAT_E6, .lon_e6 = NET_WX_HOME_LON_E6, .place = NET_WX_HOME,
        .temp_unit = WX_C, .wind_unit = WX_KMH, .refresh_min = 15, .cycle_s = 5,
    };
    int32_t la, lo;
    if (kv_get_i32(h, "wxlat", &la) == ESP_OK && kv_get_i32(h, "wxlon", &lo) == ESP_OK && wx_pos_ok(la, lo)) {
        w.lat_e6 = la;
        w.lon_e6 = lo;
        size_t len = sizeof w.place;
        if (kv_get_str(h, "wxplace", w.place, &len) != ESP_OK) w.place[0] = 0;
    }
    uint8_t v;
    if (kv_get_u8(h, "wxtemp", &v) == ESP_OK && wx_temp_unit_ok(v)) w.temp_unit = v;
    if (kv_get_u8(h, "wxwind", &v) == ESP_OK && wx_wind_unit_ok(v)) w.wind_unit = v;
    if (kv_get_u8(h, "wxrefr", &v) == ESP_OK && v >= NET_WX_REFRESH_MIN && v <= NET_WX_REFRESH_MAX)
        w.refresh_min = v;
    if (kv_get_u8(h, "wxcycle", &v) == ESP_OK && v <= NET_WX_CYCLE_MAX) w.cycle_s = v;
    if (kv_get_u8(h, "wxface", &v) == ESP_OK && v < NET_WX_FACES) w.face = v;
    portENTER_CRITICAL(&s_wx_lock);
    s_wx = w;
    portEXIT_CRITICAL(&s_wx_lock);
}

void net_prov_weather(net_weather_t *out)
{
    portENTER_CRITICAL(&s_wx_lock);
    *out = s_wx;
    portEXIT_CRITICAL(&s_wx_lock);
}

void net_prov_save_weather(const net_weather_t *in)
{
    net_weather_t was, w = *in;
    net_prov_weather(&was);
    if (!wx_pos_ok(w.lat_e6, w.lon_e6)) {
        w.lat_e6 = was.lat_e6;
        w.lon_e6 = was.lon_e6;
        memcpy(w.place, was.place, sizeof w.place);
    }
    if (!wx_temp_unit_ok(w.temp_unit)) w.temp_unit = was.temp_unit;
    if (!wx_wind_unit_ok(w.wind_unit)) w.wind_unit = was.wind_unit;
    if (w.refresh_min < NET_WX_REFRESH_MIN || w.refresh_min > NET_WX_REFRESH_MAX) w.refresh_min = was.refresh_min;
    if (w.cycle_s > NET_WX_CYCLE_MAX) w.cycle_s = was.cycle_s;
    if (w.face >= NET_WX_FACES) w.face = was.face;
    w.place[sizeof w.place - 1] = 0;
    if (!memcmp(&w, &was, sizeof w)) return;
    portENTER_CRITICAL(&s_wx_lock);
    s_wx = w;
    portEXIT_CRITICAL(&s_wx_lock);

    kv_handle_t h = kv();
    /* The place with its position, always: kept without it, it is read
     * back as none. */
    const bool moved = w.lat_e6 != was.lat_e6 || w.lon_e6 != was.lon_e6;
    if (moved) {
        kv_set_i32(h, "wxlat", w.lat_e6);
        kv_set_i32(h, "wxlon", w.lon_e6);
    }
    if (moved || strcmp(w.place, was.place)) kv_set_str(h, "wxplace", w.place);
    if (w.temp_unit != was.temp_unit)     kv_set_u8(h, "wxtemp", w.temp_unit);
    if (w.wind_unit != was.wind_unit)     kv_set_u8(h, "wxwind", w.wind_unit);
    if (w.refresh_min != was.refresh_min) kv_set_u8(h, "wxrefr", w.refresh_min);
    if (w.cycle_s != was.cycle_s)         kv_set_u8(h, "wxcycle", w.cycle_s);
    if (w.face != was.face)               kv_set_u8(h, "wxface", w.face);
    kv_commit_now(h);
}
#endif

#if VFO_RADIO_CLOCK
/* One key for the lot, its first byte the layout's: ALARMS_V, then each
 * alarm's hour, minute, days and on, then the snooze, the ring, the volume
 * and what rings. Version 1, the first test knob's, had no last byte: both. */
#define ALARMS_V   2
#define ALARMS_LEN (1 + ALARM_N * 4 + 4)

static bool alarms_ok(const net_alarms_t *a)
{
    for (int i = 0; i < ALARM_N; i++)
        if (!alarm_ok(&a->a[i])) return false;
    return a->snooze_min >= 1 && a->snooze_min <= NET_ALARM_MIN_MAX &&
           a->ring_min >= 1 && a->ring_min <= NET_ALARM_MIN_MAX && a->volume <= 100 &&
           a->by < NET_ALARM_BYS;
}

static void alarms_load(kv_handle_t h)
{
    /* Until set: each off, at 07:00, a morning a few turns away. */
    portENTER_CRITICAL(&s_alarms_lock);
    for (int i = 0; i < ALARM_N; i++) s_alarms.a[i].hour = 7;
    portEXIT_CRITICAL(&s_alarms_lock);
    uint8_t b[ALARMS_LEN];
    size_t len = sizeof b;
    if (kv_get_blob(h, "alarms", b, &len) != ESP_OK) return;
    if (!(len == sizeof b && b[0] == ALARMS_V) && !(len == sizeof b - 1 && b[0] == 1)) return;
    net_alarms_t a;
    for (int i = 0; i < ALARM_N; i++) {
        const uint8_t *e = b + 1 + i * 4;
        a.a[i] = (alarm_t){ .hour = e[0], .min = e[1], .days = e[2], .on = e[3] != 0 };
    }
    a.snooze_min = b[1 + ALARM_N * 4];
    a.ring_min   = b[2 + ALARM_N * 4];
    a.volume     = b[3 + ALARM_N * 4];
    a.by         = len == sizeof b ? b[4 + ALARM_N * 4] : NET_ALARM_BOTH;
    if (!alarms_ok(&a)) return;
    portENTER_CRITICAL(&s_alarms_lock);
    s_alarms = a;
    portEXIT_CRITICAL(&s_alarms_lock);
}

void net_prov_alarms(net_alarms_t *out)
{
    portENTER_CRITICAL(&s_alarms_lock);
    *out = s_alarms;
    portEXIT_CRITICAL(&s_alarms_lock);
}

void net_prov_save_alarms(const net_alarms_t *in)
{
    if (!alarms_ok(in)) return;
    net_alarms_t was;
    net_prov_alarms(&was);
    bool same = in->snooze_min == was.snooze_min && in->ring_min == was.ring_min &&
                in->volume == was.volume && in->by == was.by;
    for (int i = 0; same && i < ALARM_N; i++)
        same = in->a[i].hour == was.a[i].hour && in->a[i].min == was.a[i].min &&
               in->a[i].days == was.a[i].days && in->a[i].on == was.a[i].on;
    if (same) return;
    portENTER_CRITICAL(&s_alarms_lock);
    s_alarms = *in;
    portEXIT_CRITICAL(&s_alarms_lock);
    uint8_t b[ALARMS_LEN];
    b[0] = ALARMS_V;
    for (int i = 0; i < ALARM_N; i++) {
        uint8_t *e = b + 1 + i * 4;
        e[0] = in->a[i].hour;
        e[1] = in->a[i].min;
        e[2] = in->a[i].days;
        e[3] = in->a[i].on;
    }
    b[1 + ALARM_N * 4] = in->snooze_min;
    b[2 + ALARM_N * 4] = in->ring_min;
    b[3 + ALARM_N * 4] = in->volume;
    b[4 + ALARM_N * 4] = in->by;
    kv_handle_t h = kv();
    kv_set_blob(h, "alarms", b, sizeof b);
    kv_commit_now(h);
}
#endif

uint8_t net_prov_jack_db(void) { return s_jack_db; }

void net_prov_save_jack_db(uint8_t db)
{
    if (db > NET_PROV_JACK_DB_MAX || db % 3 || db == s_jack_db) return;
    s_jack_db = db;
    kv_set_u8(kv(), "jackdb", db);
    kv_commit_now(kv());
}

uint8_t net_prov_tx_mic(void) { return s_tx_mic; }

void net_prov_save_tx_mic(uint8_t src)
{
    if (src > NET_TX_RADIO || src == s_tx_mic) return;
    s_tx_mic = src;
    kv_set_u8(kv(), "txmic", src);
    kv_commit_now(kv());
}

void net_prov_save_ota_hours(uint16_t hours)
{
    if (hours == s_ota_hours) return;
    s_ota_hours = hours;
    kv_set_u16(kv(), "otah", hours);
    kv_commit_now(kv());
}

const char *net_prov_web_user(void) { return s_web_user; }
const char *net_prov_web_pass(void) { return s_web_pass; }

bool net_prov_web_is_default(void)
{
    /* The password alone: another name with "admin" for its password is
     * as easily guessed. */
    return strcmp(s_web_pass, "admin") == 0;
}

void net_prov_save_web(const char *user, const char *pass)
{
    if (user && *user) strlcpy(s_web_user, user, sizeof s_web_user);
    if (pass && *pass) strlcpy(s_web_pass, pass, sizeof s_web_pass);
    kv_handle_t h = kv();
    kv_edit_begin(h);
    kv_set_str(h, "wuser", s_web_user);
    kv_set_str(h, "wpass", s_web_pass);
    kv_edit_end(h);
    kv_saved_or(kv_commit_wait(h, 3000), "the page's login");
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
    sta_strongest(&wc);
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

/* The name that answered in the given one's place ("" = the given one). */
static char s_answered[24];

const char *net_prov_host_answered(void) { return s_answered; }

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
            s_answered[0] = '\0';
            return ESP_OK;
        }
        ESP_LOGW(TAG, "mDNS could not resolve %s", host);
#if VFO_RADIO_OPENX6X00
        /* OpenX6X00 announces an X6100 as x6100.local and an X6200 as
         * x6200.local: a knob set for the one, as it comes, finds the other. */
        const char *other = !strcasecmp(base, "x6100") ? "x6200"
                          : !strcasecmp(base, "x6200") ? "x6100" : NULL;
        if (other && mdns_query_a(other, 2000, &addr) == ESP_OK) {
            snprintf(out, out_len, IPSTR, IP2STR(&addr));
            snprintf(s_answered, sizeof s_answered, "%s.local", other);
            ESP_LOGW(TAG, "%s answered instead", s_answered);
            return ESP_OK;
        }
#endif
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
