/* The firmware images on the knob's microSD card.
 *
 * A copy of each firmware the knob has installed -- and, from the setup
 * firmware, of every one published -- so that switching firmware, or going
 * back to the setup firmware, takes seconds and needs no network, and an
 * image already fetched is not fetched again. The card is otherwise left
 * alone: everything of ours is in one folder, one image and its manifest per
 * radio, in 8.3 names (the knob's FAT has no long ones):
 *
 *   VFO-KNOB/INDEX.JSN    the release server's index.json, as last read
 *   VFO-KNOB/ICOM.BIN     the image -- vfo-knob-icom-1.15.0.bin
 *   VFO-KNOB/ICOM.JSN     its manifest, as published: version, file, sha256
 *   VFO-KNOB/COMPANIO.BIN the second chip's firmware, and COMPANIO.JSN its
 *                         manifest: the knob sends it to that chip, and
 *                         never installs it on its own (components/ota)
 *   VFO-CFG/              every firmware's settings (components/kvstore),
 *                         written there by kvstore alone
 *
 * An image is only used against its manifest's sha256, and only ever
 * installed through the update path's signature check: a card can hold
 * nothing the knob would run unsigned.
 *
 * Mounted while in use (the driver takes some 1.5 kB of internal RAM),
 * counted, so nested users share one mount -- and kept mounted by the
 * settings (kvstore) for as long as they are on the card. Its transfers from
 * PSRAM go through one fixed 512 B bounce, made once. Not for a task whose
 * stack is in PSRAM that also writes flash. */
#ifndef SD_CACHE_H
#define SD_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

/* True with a card mounted; every true needs its sdc_unmount(). False: no
 * card, or none that mounts -- said once in the log. */
bool sdc_mount(void);
void sdc_unmount(void);

/* A radio's manifest from the card, NUL-terminated. ESP_ERR_NOT_FOUND. */
esp_err_t sdc_manifest(const char *radio, char *buf, size_t cap);

/* Its image, open for reading, and its size; NULL without one. */
FILE *sdc_image_open(const char *radio, size_t *size);

/* Whether the image's sha256 is the one given (hex). Reads it all: about a
 * second and a half for a firmware. */
bool sdc_image_ok(const char *radio, const char *sha256_hex);

/* A new image: written to a temporary file, then put in place with its
 * manifest by sdc_image_commit(), or thrown away by sdc_image_discard(). */
FILE     *sdc_image_create(const char *radio);
esp_err_t sdc_image_commit(const char *radio, const char *manifest);
void      sdc_image_discard(const char *radio);

/* Room for an image of this size beside the rest -- this radio's old image
 * given up for it, if need be. */
bool sdc_room(const char *radio, size_t bytes);

/* The release server's index, as last read. */
esp_err_t sdc_index_save(const char *json);
esp_err_t sdc_index_load(char *buf, size_t cap);

/* Provisioning only: the whole card emptied, a new FAT on it, our folder
 * made. Not for anything a knob does by itself. Refused (INVALID_STATE)
 * while anyone else holds the card mounted: the settings let go first. */
esp_err_t sdc_format(void);

/* Why the last mount failed: ESP_OK after one that worked; ESP_FAIL, a card
 * that answers but holds no FAT the knob reads (exFAT, blank); another
 * error, no card that answers. */
esp_err_t sdc_last_error(void);

/* The mounted card's FatFs drive ("0:"), for FatFs called directly; NULL
 * while none is mounted. */
const char *sdc_drive(void);

/* The mounted card's identity: a hash of its CID (maker, OEM, name, serial),
 * never 0; 0 while none is mounted. */
uint32_t sdc_cid(void);

/* A card that answers but does not mount, given a new FAT and mounted: the
 * owner's "Prepare this card" (the settings). Counted as sdc_mount() is. */
esp_err_t sdc_prepare(void);

/* Up to four flushers run, in order, at every restart -- one shutdown
 * handler for the card, registered at the first call, ESP-IDF having few. */
esp_err_t sdc_on_restart(void (*fn)(void));

/* How many images of ours, and the room on the card, in the log. */
void sdc_log_state(void);

#endif /* SD_CACHE_H */
