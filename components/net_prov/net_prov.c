#include "net_prov.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/netdb.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "net";
static const char *NVS_NS = "vfo";

static vfo_cfg_t          s_cfg;
static EventGroupHandle_t s_events;
static bool               s_connected;
static int                s_retries;
static uint8_t            s_volume = 40, s_micgain = 100;
static uint8_t            s_boots;

#define BIT_GOT_IP BIT0

static void load_or_seed(void)
{
    nvs_handle_t h;
    size_t len;
    bool have = false;

    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        len = sizeof s_cfg.ssid;
        if (nvs_get_str(h, "ssid", s_cfg.ssid, &len) == ESP_OK && s_cfg.ssid[0]) {
            len = sizeof s_cfg.pass;     nvs_get_str(h, "pass", s_cfg.pass, &len);
            len = sizeof s_cfg.tci_host; nvs_get_str(h, "host", s_cfg.tci_host, &len);
            nvs_get_u16(h, "port", &s_cfg.tci_port);
            have = true;
        }
        nvs_close(h);
    }

#if CONFIG_VFO_DEV_CREDENTIALS
    if (!have) {
        ESP_LOGW(TAG, "NVS empty -- seeding from build-time dev credentials");
        strlcpy(s_cfg.ssid,     CONFIG_VFO_DEV_WIFI_SSID, sizeof s_cfg.ssid);
        strlcpy(s_cfg.pass,     CONFIG_VFO_DEV_WIFI_PASS, sizeof s_cfg.pass);
        strlcpy(s_cfg.tci_host, CONFIG_VFO_DEV_TCI_HOST,  sizeof s_cfg.tci_host);
        s_cfg.tci_port = CONFIG_VFO_DEV_TCI_PORT;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "ssid", s_cfg.ssid);
            nvs_set_str(h, "pass", s_cfg.pass);
            nvs_set_str(h, "host", s_cfg.tci_host);
            nvs_set_u16(h, "port", s_cfg.tci_port);
            nvs_commit(h);
            nvs_close(h);
        }
        have = true;
    }
#endif
    if (!have) ESP_LOGE(TAG, "no credentials: provisioning UI is a v1.1 item");
    if (!s_cfg.tci_port) s_cfg.tci_port = 50001;

    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "vol",   &v) == ESP_OK) s_volume  = v;
        if (nvs_get_u8(h, "mic",   &v) == ESP_OK) s_micgain = v;
        if (nvs_get_u8(h, "boots", &v) == ESP_OK) s_boots   = v;
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

    /* Never log the passphrase, only whether one is present. */
    ESP_LOGI(TAG, "ssid=\"%s\" psk=%s host=%s:%u",
             s_cfg.ssid, s_cfg.pass[0] ? "set" : "EMPTY",
             s_cfg.tci_host, (unsigned)s_cfg.tci_port);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupClearBits(s_events, BIT_GOT_IP);
        /* Reconnect forever: this is a shack appliance, not a phone. Back off a
         * little so a wrong passphrase does not spin the radio flat out. */
        int delay = s_retries < 5 ? 500 : 5000;
        if (s_retries < 1000) s_retries++;
        ESP_LOGW(TAG, "disconnected (attempt %d), retrying in %d ms",
                 s_retries, delay);
        vTaskDelay(pdMS_TO_TICKS(delay));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_connected = true;
        s_retries   = 0;
        ESP_LOGI(TAG, "got ip " IPSTR " gw " IPSTR,
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

esp_err_t net_prov_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");
    load_or_seed();
    return ESP_OK;
}

const vfo_cfg_t *net_prov_cfg(void) { return &s_cfg; }
uint8_t net_prov_volume(void)   { return s_volume; }
uint8_t net_prov_mic_gain(void) { return s_micgain; }

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

void net_prov_save_audio(uint8_t volume, uint8_t mic_gain)
{
    if (volume == s_volume && mic_gain == s_micgain) return;
    s_volume = volume;
    s_micgain = mic_gain;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "vol", volume);
    nvs_set_u8(h, "mic", mic_gain);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved volume=%u mic=%u", volume, mic_gain);
}
bool net_prov_is_connected(void)    { return s_connected; }

esp_err_t net_prov_wifi_start(void)
{
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "evt loop");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&ic), TAG, "wifi init");
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
        mdns_service_add(NULL, "_tci", "_tcp", s_cfg.tci_port, txt,
                         sizeof txt / sizeof txt[0]);
    }
    return ESP_OK;
}

esp_err_t net_prov_resolve(char *out, size_t out_len)
{
    const char *host = s_cfg.tci_host;
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
