/* USB-C networking: the knob appears to the host as a USB network adapter.
 *
 * Why this rather than a serial protocol: nothing above the IP layer changes.
 * TCI, the WebSocket, tuning, PTT, RX and TX audio all work exactly as they do
 * over WiFi, because as far as the firmware is concerned it is still TCP to
 * AetherSDR. Linux supports CDC-NCM natively, so there is no driver to install
 * and no bridge program to keep running on the PC.
 *
 * The device runs a DHCP server on the link and hands the host a known
 * address, which means the AetherSDR endpoint needs no configuration at all
 * when connected by USB -- we know where the PC is because we assigned it.
 *
 * NOTE: the ESP32-S3's USB-Serial-JTAG and USB-OTG peripherals share the same
 * two pins, so a build with this enabled has NO serial console. That is why it
 * sits behind CONFIG_VFO_USB_NET rather than being always on.
 */
#ifndef USB_NET_H
#define USB_NET_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Link subnet. Deliberately obscure so it cannot collide with a home LAN. */
#define USB_NET_DEVICE_IP "10.55.42.1"
#define USB_NET_HOST_IP   "10.55.42.2"
#define USB_NET_NETMASK   "255.255.255.0"

esp_err_t usb_net_init(void);

/* True once the host has taken a DHCP lease, i.e. the USB link is actually
 * carrying traffic rather than merely being plugged into a charger. */
/* Routes the USB PHY back to USB-Serial-JTAG, so the ROM's serial port is
 * present and the device can be flashed. Call early at startup: the mux is in
 * the RTC domain and survives a reset, so without this a crash leaves the
 * device reachable only by unplugging it. */
void usb_net_release_phy(void);

/* Releases the USB PHY so the ROM's serial port comes back on the next reset.
 * Without this a software reboot leaves the device unflashable until it is
 * physically power-cycled. */
void usb_net_prepare_reboot(void);

/* True once a computer has actually taken a DHCP lease on the cable, as
 * opposed to the cable merely having power on it. */
bool usb_net_host_present(void);

bool usb_net_is_up(void);

/* The host's address, or NULL when the link is down. */
const char *usb_net_host(void);

#endif /* USB_NET_H */
