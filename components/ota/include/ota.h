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
 * The one exception, on purpose: the second chip's firmware (below), which
 * the knob fetches and sends without asking. It never transmits, it goes
 * only while no headset is connected, the chip checks its signature, and
 * the chip goes back to the firmware before by itself if it does not settle
 * (bt_link.h).
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
 * checking instead. 0 also stops the look for the second chip's firmware
 * that rides on every check (below). */
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

/* The knob's microSD card (components/sd_cache). Every install takes the
 * image from it when it holds the one wanted, its sha256 checked, and
 * otherwise downloads it straight in and then puts a copy on the card -- the
 * two never side by side, which TLS's hardware AES did not survive; a switch
 * with no update server in reach takes the card's own. */

/* The latest of a radio's firmware onto the card, unless it is there: the
 * setup firmware fills the card with every one. Into PSRAM first, then onto
 * the card. TLS on the caller's stack, which wants 8 kB; `stop` set ends a
 * download early. */
esp_err_t ota_cache(const char *radio, volatile bool *stop);

/* The index, as the card last kept it: for a setup firmware whose update
 * server is out of reach. */
esp_err_t ota_card_index(char *buf, size_t cap);

/* That radio's firmware is on the card -- switching to it needs no network
 * -- and its version, into `ver` when given. */
bool ota_card_has(const char *radio, char *ver, size_t cap);

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

/* Whether version `candidate` is newer than `running` ("v1.18.0" or
 * "1.18.0"; the "v" ignored). One that does not parse never is. */
bool ota_is_newer(const char *candidate, const char *running);

/* The second chip's firmware (companion/, bt_link.h): never installed on
 * this chip. The most an image of it can be: its app slot. */
#define OTA_COMPANION_SLOT 0x1E0000

/* An image of the second chip's firmware, by its form: whole 4 kB sectors,
 * no more than its slot, an ESP32's (not this chip's), the companion's
 * project, and signed -- the signature's sector last. Its version into
 * `ver`, its identity (app_elf_sha256's first 8 bytes) into `app_sha8`. The
 * signature itself the second chip checks, against the key of the firmware
 * it runs. */
bool ota_companion_image_ok(const uint8_t *img, size_t n, char *ver, size_t cap, uint8_t app_sha8[8]);

/* Where the second chip's firmware is to be had: firmware/companion/ on the
 * update server -- never in index.json's "firmwares" -- and the SD card's
 * VFO-KNOB/COMPANIO.BIN. Its manifest names the release: */
typedef struct {
    char     version[32];   /* "1.18.0" */
    char     file[64];      /* in the channel: vfo-knob-companion-1.18.0.bin */
    char     sha256[72];    /* of the image, hex */
    uint8_t  app_sha[8];    /* its identity: app_elf_sha256's first 8 bytes */
    uint32_t size;          /* 0: not said */
    bool     known;         /* a release is known: the fields above */
    bool     from_card;     /* ...from the SD card's manifest, not the server's */
    bool     route;         /* the knob's last check reached the update server */
    bool     looked;        /* ...and there was one, since the knob started */
} ota_comp_offer_t;
void ota_companion_offer(ota_comp_offer_t *out);

/* The look rides on the knob's own check (ota_start_check(false)): after
 * this firmware's manifest, on its connection, firmware/companion/
 * manifest.json -- while the automatic check is on (ota_set_interval: 0
 * stops this look too). Without the server, the SD card's manifest is the
 * offer: this reads it, no network, the card mounted meanwhile. It never
 * replaces one the server gave. */
esp_err_t ota_companion_card_look(void);

/* The offer's image into PSRAM, on the check's worker: from the SD card when
 * it has that one, else -- `network` -- downloaded. `in_session`: a radio's
 * client runs beside it, so the download stops when internal RAM runs short
 * (12 kB free, a 4 kB block), and nothing is written to the card; without,
 * the boot window, a download also leaves a copy on the card. Checked: its
 * sha256 against the manifest's, its form (ota_companion_image_ok), its
 * version and identity against the manifest's. Started, not done:
 * ota_companion_busy() says when, ota_companion_take() has the image.
 * ESP_ERR_INVALID_STATE while one is fetched or waits to be taken, or this
 * chip's flash is written; ESP_ERR_NOT_FOUND with no offer. */
esp_err_t ota_companion_fetch(bool network, bool in_session);
bool      ota_companion_busy(void);
/* A fetch going stops at its next read (8 s at most), for `why`, which its
 * log line gives ("its time was up", "an over"). False: none was going. */
bool      ota_companion_stop(const char *why);
/* The image fetched, and its SHA-256: the caller's to free, or to hand to
 * bt_link_update_start(). NULL when there is none. */
uint8_t  *ota_companion_take(size_t *len, uint8_t sha256[32]);

/* An install or an upload is writing this chip's flash, or about to. */
bool ota_writing(void);

/* The task watchdog given half a minute while this chip's flash is written
 * (true), its usual timeout back (false): an install's or an upload's erase
 * stalls both cores for seconds. */
void ota_watchdog_relax(bool relaxed);

/* Called just before this chip's flash is written -- an install, a switch,
 * an upload -- from the task about to write it: the second chip's update
 * steps aside (bt_link_update_stop). A fetch of its image stops too, and
 * the install waits for it, 10 s at most. */
void ota_set_flash_hook(void (*before)(void));

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
