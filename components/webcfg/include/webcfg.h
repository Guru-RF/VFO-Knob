#pragma once

#include "esp_err.h"

/* Configuration and diagnostics over HTTP, on port 80.
 *
 * Reachable on whichever interface the knob has: http://10.55.42.1/ over the
 * USB cable, or the DHCP address over WiFi. It exists because the device has
 * no keyboard and, in the USB build, no serial console -- so without it the
 * only way to change an SSID or an AetherSDR address is to rebuild and
 * reflash.
 *
 * NOT AUTHENTICATED. Anything that can reach the knob can retune the radio
 * and read the WiFi password back. That is acceptable for a bench tool on a
 * trusted LAN and is not acceptable for a shipped product; see TODO.md.
 */
esp_err_t webcfg_start(void);
