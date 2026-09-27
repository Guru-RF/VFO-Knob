#pragma once

#include "esp_err.h"

/* Log sink over TCP, for builds that have no serial console.
 *
 * Enabling TinyUSB takes the USB PHY away from USB-Serial-JTAG, so the USB
 * networking build has no console at all -- and the failures worth debugging
 * in that build are exactly the ones that happen before any network is up.
 * A ring of recent lines, dumped to whoever connects, covers both: the boot
 * log survives long enough to be collected after the fact.
 *
 *     nc <device-ip> 3333
 *
 * Connect on any interface the device has (WiFi or USB). Sending the line
 * "reboot" restarts the device -- the only way back into the serial flash
 * window on a build that has no console. Both reading and that command are
 * unauthenticated and unencrypted: this is a LAN debug aid, nothing more.
 */
#define NETLOG_PORT 3333

/* Installs the log hook and allocates the ring. Touches no network state, so
 * this is safe as the very first call in app_main -- which is the point: the
 * lines worth capturing are the ones emitted before anything else is up. */
esp_err_t netlog_init(void);

/* Starts the server task. Must come after esp_netif_init(): lwIP asserts
 * ("Invalid mbox") rather than returning an error if a socket is created
 * before its TCP/IP thread exists, so this cannot be folded into netlog_init()
 * and left to retry. */
esp_err_t netlog_start(void);

/* Runs just before the "reboot" command restarts the device. Used to release
 * the USB PHY, without which the reboot leaves no way back in. */
void netlog_on_reboot(void (*fn)(void));
