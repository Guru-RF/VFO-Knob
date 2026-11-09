/* WiFi station bring-up and the stored radio endpoint.
 *
 * v1 seeds NVS from Kconfig on first boot so bench work can start immediately.
 * The SoftAP captive portal and the on-screen host editor come in v1.1; the
 * storage format is already what they will write to, so adding them does not
 * disturb anything above.
 */
#ifndef NET_PROV_H
#define NET_PROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

typedef struct {
    char     ssid[33];         /* the WiFi network joined last: the list's first */
    char     pass[65];
    char     radio_host[64];   /* IP, or a name -- ".local" resolves via mDNS */
    uint16_t radio_port;
    /* For radios that log in (the IC-705's network user); empty otherwise. */
    char     radio_user[33];
    char     radio_pass[33];
} vfo_cfg_t;

/* Audio levels persist across reboots. Written debounced, because NVS wear is
 * real and the knob can produce a lot of intermediate values in a second. */
uint8_t net_prov_volume(void);
uint8_t net_prov_mic_gain(void);
/* A Bluetooth headset's microphone has its own gain: it comes levelled, the
 * knob's PDM element is quiet, and one setting never suited both. */
uint8_t net_prov_mic_gain_headset(void);
void    net_prov_save_audio(uint8_t volume, uint8_t mic_gain, uint8_t mic_gain_headset);
/* The same in RAM only, for net_prov_flush_audio() to write when it can: a
 * flash write holds every interrupt off for up to ~100 ms, a hole in what
 * is being heard or sent. */
void    net_prov_set_audio(uint8_t volume, uint8_t mic_gain, uint8_t mic_gain_headset);
void    net_prov_flush_audio(void);

esp_err_t net_prov_init(void);

/* The radios the knob knows -- up to NET_PROV_RADIOS, each firmware its own
 * list -- and the one in use, whose endpoint is the configuration's: the
 * client starts with it at boot. Choosing another (a swipe up, or the page)
 * takes effect on the next boot; the caller restarts the knob for that. */
#define NET_PROV_RADIOS 4
typedef struct {
    char     name[24];         /* on the dial; "" = its host */
    char     host[64];         /* an UberSDR's with its scheme: "http://host" */
    uint16_t port;
    char     user[33];
    char     pass[33];
} net_radio_t;
/* A radio's host as the dial and the page name it: without the "https://" or
 * "http://" an UberSDR's keeps. */
static inline const char *net_prov_host_shown(const char *host)
{
    const char *s = strstr(host, "://");
    return s ? s + 3 : host;
}
int       net_prov_radio_count(void);
int       net_prov_radio_active(void);
bool      net_prov_radio_get(int i, net_radio_t *out);
/* Whether a radio at this address -- its host as given -- is in the list. */
bool      net_prov_radio_known(const char *host);
esp_err_t net_prov_radios_save(const net_radio_t *list, int n, int active);
esp_err_t net_prov_radio_activate(int i);
/* Moves on with each list saved (net_prov_radios_save), never with the one
 * in use alone: a client that switches live (the OpenWebRX firmware's)
 * follows a list saved under it. */
uint32_t  net_prov_radios_gen(void);

/* Written by the HTTP configuration page. Takes effect on the next boot: the
 * transport is chosen once at startup and the TCI client has no restart path.
 * Its ssid and pass, when they differ from the network joined last, are added
 * to the networks (below), as a configuration page from before the list
 * gives them. */
esp_err_t net_prov_save_cfg(const vfo_cfg_t *cfg);

/* The WiFi networks the knob knows -- up to NET_PROV_WIFIS: home and a
 * phone's hotspot, say -- the one joined last first. The station tries them
 * in turn until one answers, and whichever does goes to the front. The first
 * is also kept where every firmware before the list kept its one network, so
 * that one still finds it. */
#define NET_PROV_WIFIS 4
typedef struct { char ssid[33]; char pass[65]; } net_wifi_t;
int       net_prov_wifi_count(void);
/* The network the station is on, or "". */
const char *net_prov_wifi_now(void);
bool      net_prov_wifi_get(int i, net_wifi_t *out);
/* The whole list, as the configuration page gives it. */
esp_err_t net_prov_wifis_save(const net_wifi_t *list, int n);
/* One more, or a new password for one known: to the front. */
esp_err_t net_prov_wifi_add(const char *ssid, const char *pass);
/* Now and then, from a task with room on its stack: writes the list when the
 * network joined has moved to its front. */
void      net_prov_tick(void);

/* A one-time flag left in NVS ("vfo" namespace, u8 1) by something outside
 * the firmware -- tools/install-setup.sh's "sdwipe": true once, and gone. */
bool      net_prov_take_once(const char *key);
/* The same flag read, and left there. */
bool      net_prov_peek(const char *key);
/* Everything but the WiFi and the boot counter, from the settings (kvstore):
 * after kv_init. */
void      net_prov_load_settings(void);

/* Credentials for the HTTP configuration page, default admin/admin; "default"
 * is the shipped password, whatever the name. */
const char *net_prov_web_user(void);
const char *net_prov_web_pass(void);
bool        net_prov_web_is_default(void);
void        net_prov_save_web(const char *user, const char *pass);

/* How often to check for firmware updates, in hours; 0 disables. */
uint16_t net_prov_ota_hours(void);
void     net_prov_save_ota_hours(uint16_t hours);

/* The screen while nobody uses the knob (no knob, no touch, no transmit):
 * after `after` minutes the screensaver, `dim` minutes later it dims, `dark`
 * minutes after that the screen goes dark. Each counts from the stage before
 * it, 0 = that stage never comes. NET_SAVER_NONE keeps the dial up, dimmed
 * and darkened at the same moments. */
enum { NET_SAVER_NONE, NET_SAVER_MATRIX, NET_SAVER_CLOCK, NET_SAVER_DRIFT, NET_SAVER_DIGITAL,
       NET_SAVER_N };
typedef struct {
    uint8_t  saver;
    uint16_t after, dim, dark;
} net_screen_t;
#define NET_SCREEN_MAX_MIN 1440
void net_prov_screen(net_screen_t *out);
void net_prov_save_screen(const net_screen_t *s);

/* The owner's time zone: the IANA name the page shows and the POSIX rule the
 * clock runs on (wallclock_set_zone), Europe/Brussels until set; and the
 * hours as 24 (true until set) or 12. */
#define NET_TZ_NAME_LEN 40
#define NET_TZ_RULE_LEN 64
const char *net_prov_tz_name(void);
const char *net_prov_tz_rule(void);
bool        net_prov_clock24(void);
void        net_prov_save_time(const char *tz_name, const char *tz_rule, bool h24);

/* The clock's face -- the Clock firmware's, and the clock screensaver's on
 * every firmware: one of components/ui clockface.h's CF_* (0, the shack
 * clock, until set), and its second hand (on until set). */
#define NET_CLOCK_FACES 4
uint8_t     net_prov_clock_face(void);
bool        net_prov_clock_seconds(void);
void        net_prov_save_clock(uint8_t face, bool seconds);

/* The screen's brightness while it is lit, percent: NET_PROV_BRIGHT_MIN to
 * 100, 80 until one is set (ui_set_brightness). Kept on every firmware. */
#define NET_PROV_BRIGHT_MIN 10
uint8_t  net_prov_brightness(void);
void     net_prov_save_brightness(uint8_t percent);

/* The knob mounted with its USB cable at the bottom (GitHub #15): the screen
 * and its touch turned 180 degrees from the way it comes (ui_set_turned).
 * Kept on every firmware, as the mount is the same whichever runs. */
bool     net_prov_turned(void);
void     net_prov_save_turned(bool turned);

/* The EpiBeam firmware's array: its directions, 4 (a 4-square's N, E, S, W)
 * or 8, 4 until set; and the one the beam was left on, kept for the next
 * start -- 0 is N, on round clockwise (components/vfo_core beam.h). Fewer
 * directions put a beam past them back on N. */
uint8_t  net_prov_beam_dirs(void);
void     net_prov_save_beam_dirs(uint8_t n);
uint8_t  net_prov_beam_dir(void);
void     net_prov_save_beam_dir(uint8_t dir);

/* The ADS-B Radar firmware's radar (components/adsb_client): its middle,
 * the feed, the units, the range on the dial, what it shows, and the types
 * it calls out. Copied out and in whole, under a lock: the page writes it
 * while the feed's task and the face read it. Only that firmware has it. */
#if VFO_RADIO_ADSB
#define NET_ADSB_NO_POS INT32_MIN
/* The middle until one is set, and when the page empties it: Brussels
 * Airport, so a new knob shows aircraft as soon as it is on WiFi. */
#define NET_ADSB_HOME        "EBBR"
#define NET_ADSB_HOME_LAT_E6 50901400
#define NET_ADSB_HOME_LON_E6 4484400
enum { NET_ADSB_AIRPORTS = 1, NET_ADSB_COUNTRIES = 2, NET_ADSB_SWEEP = 4, NET_ADSB_GROUND = 8 };
#define NET_ADSB_OPTS_DEFAULT (NET_ADSB_AIRPORTS | NET_ADSB_COUNTRIES | NET_ADSB_SWEEP)
#define NET_ADSB_REFRESH_MIN  5
#define NET_ADSB_REFRESH_MAX  60
typedef struct {
    int32_t lat_e6, lon_e6;     /* the middle; NET_ADSB_HOME's until set */
    char    centre[5];          /* the airport it was set from, "" for a position */
    uint8_t source;             /* ADSB_SRC_* (components/vfo_core adsb.h) */
    char    url[128];           /* a receiver of one's own: its aircraft.json */
    uint8_t units;              /* ADSB_NM, ADSB_KM, ADSB_MI */
    uint8_t range;              /* the dial's, 0 to ADSB_RANGES - 1 */
    uint8_t show;               /* ADSB_SHOW_* */
    uint8_t opts;               /* NET_ADSB_* */
    uint8_t refresh_s;          /* the feed asked every so many seconds */
    char    types[64];          /* the types called out: "C17, A400, B74*" */
} net_adsb_t;
void     net_prov_adsb(net_adsb_t *out);
/* What differs from what is kept is written; anything out of bounds is not. */
void     net_prov_save_adsb(const net_adsb_t *a);
#endif

/* The Weather firmware's place and units (components/weather_client):
 * the forecast's place, as the page found it; the units shown; how often
 * Open-Meteo is asked; how often the faces go on to the next by themselves;
 * and the face the dial was left on. Copied out and in
 * whole, under a lock: the page writes it while the client's task and the
 * face read it. Only that firmware has it. */
#if VFO_RADIO_WEATHER
#define NET_WX_HOME          "Brussels, Belgium"
#define NET_WX_HOME_LAT_E6   50850300
#define NET_WX_HOME_LON_E6   4351700
#define NET_WX_REFRESH_MIN   10
#define NET_WX_REFRESH_MAX   60
#define NET_WX_FACES         3          /* now, the next hours, the days */
#define NET_WX_CYCLE_MAX     60         /* seconds a face, at the most */
typedef struct {
    int32_t lat_e6, lon_e6;
    char    place[48];          /* "Brussels, Belgium" */
    uint8_t temp_unit;          /* WX_C, WX_F (components/vfo_core weather.h) */
    uint8_t wind_unit;          /* WX_KMH, WX_MS, WX_KN, WX_MPH, WX_BFT */
    uint8_t refresh_min;        /* NET_WX_REFRESH_MIN to _MAX, 15 until set */
    uint8_t cycle_s;            /* 0 never, 1 to NET_WX_CYCLE_MAX seconds, 5 until set */
    uint8_t face;               /* the dial's, 0 to NET_WX_FACES - 1 */
} net_weather_t;
void     net_prov_weather(net_weather_t *out);
/* What differs from what is kept is written; anything out of bounds is not. */
void     net_prov_save_weather(const net_weather_t *w);
#endif

/* The Clock firmware's alarms (components/vfo_core alarm.h): ten, all off
 * at 07:00 until set; how long a snooze is, and how long one rings before it
 * stops by itself, minutes; what rings -- the motor, the sound, or both --
 * and how loud the sound is: on a Bluetooth headset or speaker while one is
 * connected, on the jack while none is.
 * Copied out and in whole, under a lock: the page and the dial both set
 * them. Only that firmware has them. */
#if VFO_RADIO_CLOCK
#include "alarm.h"
#define NET_ALARM_MIN_MAX 30            /* the longest snooze, the longest ring */
enum { NET_ALARM_BOTH, NET_ALARM_MOTOR, NET_ALARM_SOUND, NET_ALARM_BYS };
typedef struct {
    alarm_t a[ALARM_N];
    uint8_t snooze_min;         /* 1 to NET_ALARM_MIN_MAX, 9 until set */
    uint8_t ring_min;           /* 1 to NET_ALARM_MIN_MAX, 10 until set */
    uint8_t volume;             /* 0 to 100, 60 until set */
    uint8_t by;                 /* NET_ALARM_*: what rings, both until set */
} net_alarms_t;
void     net_prov_alarms(net_alarms_t *out);
/* Kept as it is, when every alarm is in bounds and the minutes are too. */
void     net_prov_save_alarms(const net_alarms_t *a);
#endif

/* The jack's level above the VOLUME, dB: 0 to NET_PROV_JACK_DB_MAX in steps
 * of 3 (audio_out_set_jack_level); anything else is not saved. */
#define NET_PROV_JACK_DB_MAX 12
uint8_t  net_prov_jack_db(void);
void     net_prov_save_jack_db(uint8_t db);

/* What the AetherSDR and icom firmwares transmit with (GitHub #19, #23):
 * NET_TX_KNOB, the knob's own microphone or a headset's, sent over TCI or
 * the Icom's network audio, until set; or NET_TX_RADIO, the radio's own
 * microphone input -- AetherSDR's, the computer's; an Icom's, as it is set --
 * the knob keying it as a foot switch would. Read at each key (tci_client,
 * icom_client). */
enum { NET_TX_KNOB, NET_TX_RADIO };
uint8_t  net_prov_tx_mic(void);
void     net_prov_save_tx_mic(uint8_t src);

/* The IC-9700 in satellite mode (GitHub #18): what a turn of the knob does to
 * the uplink, the SUB band. The radio's tracking follows its own dial only,
 * and no CI-V command reads how it is set, so this says it again: NET_SAT_OFF,
 * the downlink alone, until set; NET_SAT_NORMAL, the same way; NET_SAT_REVERSE,
 * the other way, an inverting transponder's. Read at each turn (icom_client). */
enum { NET_SAT_OFF, NET_SAT_NORMAL, NET_SAT_REVERSE };
uint8_t  net_prov_sat_track(void);
void     net_prov_save_sat_track(uint8_t track);

/* Boot-loop guard. net_prov_boot_count() is incremented on every boot and
 * cleared once the device has been up long enough to be considered healthy;
 * call net_prov_boot_ok() from a timer for that. Three rapid boots in a row
 * put the firmware into a reduced mode so it stays reachable and flashable
 * instead of disappearing from USB in a panic loop. */
uint8_t net_prov_boot_count(void);
void    net_prov_boot_ok(void);          /* NVS + seed from Kconfig if empty */
const vfo_cfg_t *net_prov_cfg(void);

/* Start the station and keep it connected. Non-blocking. */
esp_err_t net_prov_wifi_start(void);

/* Stops and deinitialises the radio, freeing its internal RAM. Used when the
 * USB cable wins the transport choice and WiFi is dead weight. */
esp_err_t net_prov_wifi_stop(void);

bool net_prov_is_connected(void);

/* Resolve the configured host. mDNS for *.local, getaddrinfo otherwise.
 * Writes a dotted-quad into `out`. On the openx6x00 firmware, x6100.local
 * not answering, x6200.local is asked too, and the other way round. */
esp_err_t net_prov_resolve(char *out, size_t out_len);
/* The name that answered in the configured one's place, at the last
 * resolve ("x6200.local"); "" when the configured one did. */
const char *net_prov_host_answered(void);

/* --- the knob's own hotspot, for WiFi setup (net_ap.c) ----------------------
 * A hotspot a phone can join, which sends it to the knob's page by itself (a
 * captive portal: DNS answers every name with the knob, DHCP names it as the
 * portal). The station stays up beside it. Needs net_prov_wifi_start() first. */
esp_err_t net_prov_ap_start(const char *ssid);
void      net_prov_ap_stop(void);
bool      net_prov_ap_active(void);

/* The networks in reach, strongest first, each once: at most `max`. Blocks
 * for the scan, a few seconds. */
typedef struct { char ssid[33]; int8_t rssi; bool open; } net_prov_net_t;
int net_prov_scan(net_prov_net_t *out, int max);

/* Join a network now, as given on the portal. It is tried a few times;
 * net_prov_join_state() says how it went -- and why not, when it did not --
 * and net_prov_join_keep() stores it once it worked. */
typedef enum { NET_JOIN_IDLE = 0, NET_JOIN_TRYING, NET_JOIN_OK, NET_JOIN_FAILED } net_join_t;
esp_err_t  net_prov_join(const char *ssid, const char *pass);
net_join_t net_prov_join_state(char *ssid, size_t sn, char *why, size_t wn);
esp_err_t  net_prov_join_keep(void);

/* While the hotspot is up, stop chasing the stored network: scanning the
 * channels for it takes the hotspot off the air, and the phone with it. Even
 * without this, the station stands still while a phone is on the hotspot,
 * and takes up the known networks again when the last one leaves. */
void net_prov_hold_station(bool hold);

#endif /* NET_PROV_H */
