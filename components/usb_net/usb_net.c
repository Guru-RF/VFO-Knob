#include "usb_net.h"

#if !CONFIG_VFO_USB_NET
/* Not built in. Stubs keep the call sites free of #ifdef. */
esp_err_t   usb_net_init(void)  { return ESP_ERR_NOT_SUPPORTED; }
bool        usb_net_is_up(void) { return false; }
const char *usb_net_host(void)  { return NULL; }
#else


#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_net.h"
#include "tusb.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

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
    (void)h;
    heap_caps_free(buffer);
}

/* Inbound frame from the host.
 *
 * The copy is not defensive tidiness, it is required. esp_netif hands the
 * pointer to lwIP as a PBUF_REF -- see esp_pbuf_allocate(), which calls
 * pbuf_alloced_custom(..., PBUF_REF, ..., buffer, len) -- so lwIP REFERENCES
 * these bytes rather than copying them, and only releases them later through
 * driver_free_rx_buffer, once TCP has delivered the data to the application.
 * TinyUSB's NTB buffer, by contrast, is valid only for the duration of this
 * callback and is immediately reused for the next packet.
 *
 * Passing it straight through therefore lets the payload mutate underneath
 * lwIP after its checksum has already been verified. The corruption reaches
 * the application as valid-looking data: the symptom was a WebSocket frame
 * parser reading a header at the wrong offset ("Non-zero RSV bits detected"),
 * a few seconds into any sustained stream and within ~40 ms once RX audio was
 * running. WiFi never showed it because that driver allocates a buffer per
 * frame and frees it from this same callback.
 *
 * PSRAM first: internal RAM is the scarce resource on this board, and nothing
 * here is touched by DMA. */
static esp_err_t usb_recv(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!s_netif) return ESP_OK;

    void *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!copy) copy = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!copy) return ESP_ERR_NO_MEM;   /* drop; TCP will retransmit */

    memcpy(copy, buffer, len);
    /* On any failure path esp_netif frees the buffer through netif_free_rx,
     * so ownership transfers unconditionally here. */
    esp_netif_receive(s_netif, copy, len, NULL);
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
    /* This version takes the device index first -- the signature differs from
     * upstream master, so check the installed header, not the docs. */
    ESP_RETURN_ON_ERROR(tinyusb_net_init(TINYUSB_USBDEV_0, &ncfg),
                        TAG, "net init");

    /* Force the host to re-enumerate us.
     *
     * The ROM brings up USB-Serial-JTAG and the host enumerates it seconds
     * before we get here. Handing the PHY to the OTG controller does not drop
     * the D+ pull-up, so the host sees no disconnect and keeps using the
     * descriptors it already cached -- a serial port. Everything on our side
     * reports success (the PHY switches, the driver installs, the device task
     * runs) while the host stays bound to cdc_acm and no network interface
     * ever appears. Pulling the pull-up down long enough to read as a physical
     * unplug is the only way to make it look again. */
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
    tud_connect();

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
    /* Never ESP_ERROR_CHECK during init: an abort here is a reboot loop. */
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                   on_dhcp_event, NULL),
                        TAG, "ip evt");

    ESP_LOGI(TAG, "USB network up: device %s, host will be %s",
             USB_NET_DEVICE_IP, USB_NET_HOST_IP);
    return ESP_OK;
}

void usb_net_release_phy(void)
{
    /* Point the internal USB PHY back at USB-Serial-JTAG.
     *
     * This is what makes the device recoverable. The mux that hands the PHY to
     * the OTG controller lives in the RTC domain, so it SURVIVES A CPU RESET --
     * and usb_del_phy() does not touch it, it only clears the pull overrides.
     * The result, measured: after any reset short of pulling the cable, the
     * ROM's serial port never appears (nine seconds of silence, then NCM), so
     * the flash window is not there and esptool has nothing to open. That is
     * true of a crash or a watchdog reboot just as much as a deliberate one,
     * which is why this runs at startup and not only on the way out.
     *
     * Bit 19 clear = internal FSLS PHY routed to USJ; bit 20 keeps that choice
     * under software control rather than the efuse default. RTC registers are
     * always clocked, so this is safe this early and needs no peripheral setup. */
    REG_SET_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL);
    REG_CLR_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_USB_PHY_SEL);
}

void usb_net_prepare_reboot(void)
{
    if (!s_netif) { usb_net_release_phy(); return; }
    /* Hand the USB PHY back to USB-Serial-JTAG before restarting.
     *
     * The mux that routes the internal PHY to the OTG controller lives in the
     * RTC domain, which survives a CPU reset -- so after a plain esp_restart()
     * the ROM never re-enumerates its serial port and the flash window simply
     * is not there. Measured: a software reboot went dark for nine seconds and
     * came back as NCM, with no serial device in between. Only a power cycle
     * cleared it. tinyusb_driver_uninstall() calls usb_del_phy(), which puts
     * the mux back. */
    esp_err_t err = tinyusb_driver_uninstall();
    usb_net_release_phy();
    ESP_LOGW(TAG, "USB networking torn down before reboot (%s)",
             esp_err_to_name(err));
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
