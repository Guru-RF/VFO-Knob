#include "usb_net.h"

#if !CONFIG_VFO_USB_NET
/* Not built in. Stubs keep the call sites free of #ifdef. */
esp_err_t   usb_net_init(void)  { return ESP_ERR_NOT_SUPPORTED; }
bool        usb_net_is_up(void) { return false; }
const char *usb_net_host(void)  { return NULL; }
#else


#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_net.h"

static const char *TAG = "usbnet";

static esp_netif_t *s_netif;
static volatile bool s_host_seen;

/* --- esp_netif <-> tinyusb glue ------------------------------------------ */

static esp_err_t netif_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    /* Synchronous: the caller owns the buffer and esp_netif frees it on
     * return, so an async send would race the free. */
    return tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
}

static void netif_free_rx(void *h, void *buffer)
{
    (void)h; (void)buffer;   /* TinyUSB owns the RX buffer */
}

static esp_err_t usb_recv(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (s_netif) esp_netif_receive(s_netif, buffer, len, NULL);
    return ESP_OK;
}

static void on_dhcp_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == IP_EVENT_ETH_GOT_IP) s_host_seen = true;
}

esp_err_t usb_net_init(void)
{
    /* A locally-administered MAC derived from the chip's own, so two knobs on
     * one host do not collide. */
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_ETH), TAG, "mac");
    mac[0] |= 0x02;
    mac[0] &= 0xFE;

    const tinyusb_config_t usb = { .external_phy = false };
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&usb), TAG, "tinyusb");

    tinyusb_net_config_t ncfg = { .on_recv_callback = usb_recv };
    memcpy(ncfg.mac_addr, mac, 6);
    ESP_RETURN_ON_ERROR(tinyusb_net_init(&ncfg), TAG, "net init");

    /* An Ethernet-like netif that runs a DHCP SERVER: the host is the client
     * here, so we hand it an address rather than asking for one. */
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_desc = "usb";
    base.route_prio = 10;              /* below WiFi's default 50 by design */
    base.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr      = esp_ip4addr_aton(USB_NET_DEVICE_IP);
    ip.gw.addr      = esp_ip4addr_aton(USB_NET_DEVICE_IP);
    ip.netmask.addr = esp_ip4addr_aton(USB_NET_NETMASK);
    base.ip_info    = &ip;

    esp_netif_driver_ifconfig_t drv = {
        .handle                = (void *)1,   /* opaque, must be non-NULL */
        .transmit              = netif_transmit,
        .driver_free_rx_buffer = netif_free_rx,
    };
    esp_netif_config_t cfg = {
        .base   = &base,
        .driver = &drv,
        .stack  = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_netif = esp_netif_new(&cfg);
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "netif");

    esp_netif_action_start(s_netif, NULL, 0, NULL);
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               on_dhcp_event, NULL));

    ESP_LOGI(TAG, "USB network up: device %s, host will be %s",
             USB_NET_DEVICE_IP, USB_NET_HOST_IP);
    return ESP_OK;
}

bool usb_net_is_up(void)
{
    if (!s_netif) return false;
    /* A lease is the only honest signal. The interface comes up as soon as the
     * cable is in, including into a charger with no host behind it. */
    return s_host_seen || esp_netif_is_netif_up(s_netif);
}

const char *usb_net_host(void)
{
    return usb_net_is_up() ? USB_NET_HOST_IP : NULL;
}

#endif /* CONFIG_VFO_USB_NET */
