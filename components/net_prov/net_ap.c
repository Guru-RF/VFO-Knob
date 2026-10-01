/* The knob's hotspot -- the setup firmware's, and any firmware's with none of
 * its WiFi networks in reach: an open network a phone can join, a DHCP
 * server that names the knob as both the DNS server and the captive portal
 * (option 114), and a DNS server that answers every name with the knob's own
 * address. A phone that joins asks whether it is online (generate_204,
 * hotspot-detect.html), is sent to the knob's page, and opens it by itself.
 *
 * The station side stays up beside it (APSTA): the network being set up is
 * joined while the phone watches, so the portal can say whether it worked.
 */
#include "net_prov.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "net_ap";

static esp_netif_t   *s_ap_netif;
static volatile bool  s_ap;
static TaskHandle_t   s_dns;
static uint32_t       s_ap_ip;          /* network order */

bool net_prov_ap_active(void) { return s_ap; }

/* ------------------------------------------------------------------- DNS */

/* Every A question answered with the knob's address, anything else with
 * no answer -- the whole of a captive portal's DNS. */
static void dns_task(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(53),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s < 0 || bind(s, (struct sockaddr *)&me, sizeof me) != 0) {
        ESP_LOGE(TAG, "no DNS socket");
        if (s >= 0) close(s);
        s_dns = NULL;
        vTaskDelete(NULL);
    }
    static uint8_t q[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const int n = recvfrom(s, q, sizeof q - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || !s_ap) continue;
        /* A standard query, one question. */
        if ((q[2] & 0xF8) != 0 || q[4] != 0 || q[5] != 1) continue;
        int p = 12;
        while (p < n && q[p]) p += q[p] + 1;        /* the name's labels */
        if (p + 5 > n) continue;
        const uint16_t qtype = (uint16_t)(q[p + 1] << 8 | q[p + 2]);
        p += 5;                                     /* the 0, type, class */
        q[2] = 0x84;                                /* a response, authoritative */
        q[3] = 0x00;                                /* no error */
        q[6] = q[7] = q[8] = q[9] = q[10] = q[11] = 0;
        int len = p;
        if (qtype == 1) {                           /* A: the knob */
            q[7] = 1;
            static const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
            memcpy(q + len, ans, sizeof ans);
            len += sizeof ans;
            memcpy(q + len, &s_ap_ip, 4);
            len += 4;
        }
        sendto(s, q, len, 0, (struct sockaddr *)&from, fl);
    }
}

/* --------------------------------------------------------------- hotspot */

esp_err_t net_prov_ap_start(const char *ssid)
{
    if (s_ap) return ESP_OK;
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_ap_netif, ESP_ERR_NO_MEM, TAG, "netif");

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.ap.ssid, ssid, sizeof wc.ap.ssid);
    wc.ap.ssid_len       = (uint8_t)strlen(ssid);
    wc.ap.channel        = 6;
    wc.ap.max_connection = 4;
    wc.ap.authmode       = WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wc), TAG, "ap cfg");

    /* The DHCP server names the knob as DNS, and as the captive portal. */
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap_netif, &ip);
    s_ap_ip = ip.ip.addr;
    char uri[32];
    snprintf(uri, sizeof uri, "http://" IPSTR, IP2STR(&ip.ip));
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_dns_info_t dns = { .ip.u_addr.ip4.addr = ip.ip.addr, .ip.type = ESP_IPADDR_TYPE_V4 };
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offer = 0x02;                          /* OFFER_DNS */
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer, sizeof offer);
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           uri, strlen(uri));
    esp_netif_dhcps_start(s_ap_netif);

    s_ap = true;
    if (!s_dns) xTaskCreatePinnedToCore(dns_task, "dns", 3072, NULL, 3, &s_dns, 0);
    ESP_LOGW(TAG, "hotspot \"%s\" up at %s", ssid, uri);
    return ESP_OK;
}

void net_prov_ap_stop(void)
{
    if (!s_ap) return;
    s_ap = false;                                  /* the DNS task goes quiet */
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "hotspot down");
}

/* ------------------------------------------------------------------ scan */

int net_prov_scan(net_prov_net_t *out, int max)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        /* The station in the middle of trying a known network, as a phone
         * comes onto the hotspot: stop it, and look again. */
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
        if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (!n) return 0;
    wifi_ap_record_t *r = heap_caps_calloc(n, sizeof *r, MALLOC_CAP_SPIRAM);
    if (!r) { esp_wifi_clear_ap_list(); return 0; }
    esp_wifi_scan_get_ap_records(&n, r);
    int k = 0;
    /* Strongest first, each name once. */
    for (int round = 0; round < n && k < max; round++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (!r[i].ssid[0]) continue;
            if (best < 0 || r[i].rssi > r[best].rssi) best = i;
        }
        if (best < 0) break;
        bool dup = false;
        for (int j = 0; j < k; j++)
            if (strcmp(out[j].ssid, (char *)r[best].ssid) == 0) dup = true;
        if (!dup) {
            strlcpy(out[k].ssid, (char *)r[best].ssid, sizeof out[k].ssid);
            out[k].rssi = r[best].rssi;
            out[k].open = r[best].authmode == WIFI_AUTH_OPEN;
            k++;
        }
        r[best].ssid[0] = 0;
    }
    free(r);
    return k;
}
