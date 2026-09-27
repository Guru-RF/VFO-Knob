#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Over-the-air updates from GitHub Releases.
 *
 * The release feed is public and the images are signed, so no account, token
 * or server of our own is involved: the device asks GitHub for the latest
 * release, compares the tag with its own version, and installs the asset if it
 * is newer. Images are RSA-3072 signed and the signature is checked before the
 * update is accepted (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT), which is
 * what makes it safe to point a transceiver's control head at the internet.
 *
 * Nothing is burned into eFuse and Secure Boot is not enabled, so a device can
 * always still be recovered over USB. The signature protects the update path,
 * not the hardware.
 */

typedef enum {
    OTA_IDLE = 0,
    OTA_CHECKING,
    OTA_DOWNLOADING,
    OTA_DONE_REBOOT_NEEDED,
    OTA_UP_TO_DATE,
    OTA_FAILED,
} ota_phase_t;

typedef struct {
    ota_phase_t phase;
    int         percent;        /* while downloading, else 0 */
    char        running[32];    /* version now in flash */
    char        available[32];  /* latest release tag seen, "" if unknown */
    char        message[96];    /* human-readable result or error */
} ota_status_t;

esp_err_t ota_init(void);

/* Check every `hours` and install anything newer; 0 turns it off. Only ever
 * useful on WiFi -- over the USB cable the device has no route out, and the
 * configuration page does the checking instead. Never reboots by itself. */
esp_err_t ota_set_interval(uint32_t hours);

/* Confirms the running image so the bootloader stops treating it as on trial.
 * Call once the device has proved it works -- anything earlier defeats the
 * rollback. */
void ota_mark_valid(void);

/* Kicks off a check (and install, if one is newer) on a worker task. Returns
 * immediately; poll ota_get_status(). */
esp_err_t ota_start_check(bool install);

void ota_get_status(ota_status_t *out);

/* Where images are published. The configuration page needs this so it can do
 * the download itself when the device has no route out. */
const char *ota_base_url(void);

/* Push an image in from the browser instead of pulling it from GitHub.
 *
 * This is the path that always works. Over the USB cable the knob is the DHCP
 * server and the computer is the client, so the knob has no gateway and cannot
 * reach the internet at all -- pulling a release only works when it is on
 * WiFi. Uploading moves the download to the machine that already has a browser
 * open. The image is signature-checked either way, by esp_ota_end(). */
esp_err_t ota_upload_begin(void);
esp_err_t ota_upload_write(const void *data, size_t len);
esp_err_t ota_upload_end(void);
void      ota_upload_abort(void);
