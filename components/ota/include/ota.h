#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Over-the-air updates from GitHub Releases.
 *
 * The release feed is public and the images are signed, so no account, token
 * or server of our own is involved: the device asks GitHub for the latest
 * release and compares it with its own version. Whether to install a newer
 * one is asked on the dial (main/app_main.c), never decided here. Images are RSA-3072 signed and the signature is checked before the
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
    bool        newer;          /* `available` is newer than `running` */
    uint32_t    checks;         /* completed checks, so each is acted on once */
} ota_status_t;

esp_err_t ota_init(void);

/* Every `hours` a check falls due; 0 turns it off. The timer only marks it:
 * the caller starts it with ota_start_check(false) when the radio is idle and
 * RAM has room, then ota_clear_due(). Only looks -- whether to install is
 * asked on the dial, not decided here. Only ever useful on WiFi: over the USB
 * cable the device has no route out, and the configuration page does the
 * checking instead. */
esp_err_t ota_set_interval(uint32_t hours);
bool      ota_check_due(void);
void      ota_clear_due(void);

/* Confirms the running image so the bootloader stops treating it as on trial.
 * Call once the device has proved it works -- anything earlier defeats the
 * rollback. */
void ota_mark_valid(void);

/* Kicks off a check (and install, if one is newer) on a worker task. Returns
 * immediately; poll ota_get_status() -- `checks` counts each finished run. A
 * check that only looks runs on a PSRAM stack and keeps its buffers there; an
 * install writes flash and needs an internal stack. */
esp_err_t ota_start_check(bool install);

void ota_get_status(ota_status_t *out);

/* Install the latest release of another radio's firmware: a deliberate switch
 * -- the firmware picker's, and the way back to it. Runs like an install from
 * ota_start_check(true), polled the same way; only that radio's image is
 * accepted, and it is installed whatever its version. The settings stay: they
 * are in NVS, which no image touches. */
esp_err_t ota_start_switch(const char *radio);

/* The version a radio's channel holds ("1.9.0"), fetched now: a few seconds
 * at most, TLS on the caller's stack, which wants 8 kB. ESP_ERR_NOT_FOUND when
 * the channel has nothing in it. */
esp_err_t ota_fetch_version(const char *radio, char *ver, size_t cap);

/* Every firmware published, as the release script lists them in
 * firmware/index.json: {"firmwares":[{"radio":"multiflex","name":"FlexRadio",
 * "version":"1.9.0"},...]}. Fetched now, TLS on the caller's stack, into
 * `buf`, NUL-terminated. So a setup firmware in the field offers the radios
 * published after it was built. */
esp_err_t ota_fetch_index(char *buf, size_t cap);

/* The radio this firmware is for: its image is vfo-knob-<radio>, and it
 * follows the update channel of that name. */
const char *ota_radio(void);

/* Where this firmware's images are published -- ota_root_url() plus the
 * radio. The configuration page needs this so it can do the download itself
 * when the device has no route out. */
const char *ota_base_url(void);

/* Where every radio's channel lives, for the page's switch to another radio's
 * firmware. */
const char *ota_root_url(void);

/* Push an image in from the browser instead of pulling it from GitHub.
 *
 * This is the path that always works. Over the USB cable the knob is the DHCP
 * server and the computer is the client, so the knob has no gateway and cannot
 * reach the internet at all -- pulling a release only works when it is on
 * WiFi. Uploading moves the download to the machine that already has a browser
 * open. The image is signature-checked either way, by esp_ota_end().
 *
 * `radio` is NULL for an update: the image must be this radio's firmware, and
 * ota_upload_end() refuses another's with ESP_ERR_NOT_SUPPORTED. Naming a
 * radio is a deliberate switch to that radio's firmware, and only that one's
 * is accepted.
 *
 * `size` is the image's length when the sender gave one, 0 when not. With it,
 * the slot is erased here, before any data arrives -- which takes seconds. */
bool      ota_busy(void);
esp_err_t ota_upload_begin(const char *radio, size_t size);
esp_err_t ota_upload_write(const void *data, size_t len);
esp_err_t ota_upload_end(void);
void      ota_upload_abort(void);
