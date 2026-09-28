#include "usb_net.h"

#if !CONFIG_VFO_USB_NET
/* Not built in. Stubs keep the call sites free of #ifdef. */
esp_err_t   usb_net_init(void)  { return ESP_ERR_NOT_SUPPORTED; }
bool        usb_net_is_up(void) { return false; }
bool        usb_net_probe_host(uint32_t ms) { (void)ms; return false; }
const char *usb_net_host(void)  { return NULL; }
#else


#include <stdio.h>
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
#include "soc/usb_serial_jtag_reg.h"
#include "soc/soc.h"

static const char *TAG = "usbnet";

static esp_netif_t *s_netif;
static volatile bool s_host_seen;

/* What the computer calls us. The defaults said "Espressif Systems / Espressif
 * Device" with serial "123456" -- the same serial on every unit, so a host
 * could not tell two knobs apart, and Windows files per-device settings under
 * it. The IDs stay Espressif's (303A:4000); only the names are ours.
 *
 * Same slots as esp_tinyusb's own table for an NCM-only build, because
 * tinyusb_net_init() writes the MAC into slot 5 at runtime. Enabling another
 * class (CDC, MSC, vendor) shifts those slots, and this table with them. */
#if CFG_TUD_CDC || CFG_TUD_MSC || CFG_TUD_VENDOR || !CFG_TUD_NCM
/* So does the NCM interface number, which the MS OS 2.0 function subset below
 * names as 0 -- Windows would then load its NCM driver on the wrong function. */
#error "usb_net descriptors assume NCM is the only USB class (ITF 0, strings 4 and 5)"
#endif
static char s_serial[13];
static const char *s_usb_strings[] = {
    (const char[]){ 0x09, 0x04 },   /* 0: English (0x0409)                   */
    "RF.Guru",                      /* 1: manufacturer                       */
    "VFO-Knob",                     /* 2: product                            */
    s_serial,                       /* 3: serial -- the chip's own MAC       */
    "VFO-Knob network",             /* 4: the NCM interface                  */
    "",                             /* 5: MAC, filled by tinyusb_net_init()  */
};

/* --- what Windows needs ---------------------------------------------------
 *
 * Linux binds cdc_ncm to the interface class and is happy with anything here.
 * Windows is not, in two separate ways.
 *
 * The device class. esp_tinyusb only announces an interface association
 * (EF/02/01) when CDC-ACM is enabled, so an NCM-only build said 00/00/00 while
 * its configuration carries an IAD. Windows 11 binds UsbNcm by class code, but
 * since the September 2026 update (KB5124008) it no longer groups an IAD on a
 * class-00 device: the two NCM interfaces land in separate device nodes and
 * UsbNcm fails with Code 10. Espressif fixed this upstream (esp-usb #591) in
 * no release this project can use, so the descriptor is ours.
 *
 * The driver match. Windows 10 (1903 and later) has UsbNcm but does not pick
 * it by class code; without help the knob is an unknown device (Code 28) until
 * someone chooses the driver by hand. A Microsoft OS 2.0 descriptor naming the
 * compatible ID "WINNCM" on the NCM function makes it load by itself. Windows
 * only asks for one when bcdUSB is 2.01 or higher, through the BOS descriptor.
 *
 * bcdDevice moves to 1.01 so Windows files this as a new device revision rather
 * than reusing what it cached for the old descriptors. */
static const tusb_desc_device_t s_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,
    .bDeviceClass       = TUSB_CLASS_MISC,         /* 0xEF */
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,    /* 0x02 */
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,       /* 0x01 */
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,                  /* unchanged: Espressif */
    .idProduct          = 0x4000,                  /* unchanged: esp_tinyusb */
    .bcdDevice          = 0x0101,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

#define MS_OS_20_VENDOR_CODE 0x01     /* any non-zero byte; echoed in the BOS */
#define MS_OS_20_SET_LEN     (0x0A + 0x08 + 0x08 + 0x14)

/* Set header, configuration subset, function subset starting at interface 0
 * (the NCM control interface), and the compatible ID. */
static const uint8_t s_ms_os_20[] = {
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_SET_LEN),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(MS_OS_20_SET_LEN - 0x0A),
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    0, 0, U16_TO_U8S_LE(MS_OS_20_SET_LEN - 0x0A - 0x08),
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'N', 'C', 'M', 0, 0,       /* compatible ID     */
    0, 0, 0, 0, 0, 0, 0, 0,                   /* sub-compatible ID */
};
TU_VERIFY_STATIC(sizeof s_ms_os_20 == MS_OS_20_SET_LEN, "MS OS 2.0 set length");

#define BOS_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
static const uint8_t s_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_SET_LEN, MS_OS_20_VENDOR_CODE),
};
TU_VERIFY_STATIC(sizeof s_bos == BOS_LEN, "BOS length");

/* Both override weak stubs in TinyUSB's usbd.c. They have to exist together:
 * with bcdUSB 2.10 and the stub, the host's BOS request would stall. */
uint8_t const *tud_descriptor_bos_cb(void) { return s_bos; }

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *req)
{
    if (stage != CONTROL_STAGE_SETUP) return true;
    /* IN only. An OUT request with the same code would have TinyUSB copy the
     * host's data stage into s_ms_os_20 -- which is const, in flash-mapped
     * memory, and the write is a cache-error panic. */
    if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        req->bmRequestType_bit.direction == TUSB_DIR_IN &&
        req->bRequest == MS_OS_20_VENDOR_CODE &&
        req->wIndex == 7)                      /* MS_OS_20_DESCRIPTOR_INDEX */
        return tud_control_xfer(rhport, req, (void *)(uintptr_t)s_ms_os_20,
                                sizeof s_ms_os_20);
    return false;                              /* stall anything else */
}

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
    /* The chip's own address, unmodified, is the knob's end of the link. It
     * was left unset, so every ARP reply went out from 00:00:00:00:00:00. */
    uint8_t own[6];
    memcpy(own, mac, sizeof own);
    mac[0] |= 0x02;
    mac[0] &= 0xFE;

    uint8_t chip[6];
    ESP_RETURN_ON_ERROR(esp_efuse_mac_get_default(chip), TAG, "chip mac");
    snprintf(s_serial, sizeof s_serial, "%02X%02X%02X%02X%02X%02X",
             chip[0], chip[1], chip[2], chip[3], chip[4], chip[5]);

    const tinyusb_config_t usb = {
        .external_phy            = false,
        .device_descriptor       = &s_device,
        /* configuration_descriptor stays NULL: esp_tinyusb's NCM one is right. */
        .string_descriptor       = s_usb_strings,
        .string_descriptor_count = sizeof s_usb_strings / sizeof s_usb_strings[0],
    };
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

    /* No gateway. The DHCP server offers a router only when the interface has
     * one, and it used to be our own address -- which told the computer to
     * send its internet traffic down the cable. A 12 Mbit/s link can outrank
     * slow Wi-Fi in Windows' metrics, and the page's own firmware download
     * then went nowhere. Nothing on this link lies beyond it. */
    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr      = esp_ip4addr_aton(USB_NET_DEVICE_IP);
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
    /* Only this reaches lwIP's hardware address. base.mac in the config above
     * stops at esp_netif's own copy, for a driver like this one. */
    ESP_RETURN_ON_ERROR(esp_netif_set_mac(s_netif, own), TAG, "netif mac");

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

bool usb_net_host_present(void)
{
    if (!s_netif) return false;
    /* USB enumeration is the honest test, and TinyUSB already knows the
     * answer: a host CONFIGURES a device, a charger only powers it.
     *
     * The DHCP lease table looked like the obvious source and is not. The
     * host keeps its address across a reboot of this device and never
     * re-requests it, so after the knob restarts the table is empty while a
     * computer is plainly sitting on the other end of the cable -- which sent
     * the knob onto WiFi, where the driver's memory left too little for the
     * WebSocket client to start at all. */
    return tud_mounted();
}

bool usb_net_probe_host(uint32_t ms)
{
    /* Read-only, so it disturbs nothing the ROM's serial port is doing. */
    const uint32_t first = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) &
                           USB_SERIAL_JTAG_SOF_FRAME_INDEX;
    for (uint32_t t = 0; t < ms; t += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if ((REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) &
             USB_SERIAL_JTAG_SOF_FRAME_INDEX) != first)
            return true;
    }
    return false;
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
