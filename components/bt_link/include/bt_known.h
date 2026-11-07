/* The Bluetooth devices this knob has connected: what the configuration page
 * offers to call again, with no pairing mode. Plain C, with nothing of
 * ESP-IDF's: test/host tests it.
 *
 * The second chip remembers one device, the last connected, and calls that
 * one by itself (companion/main/hfp.c). It keeps the pairing of each device
 * it leaves, though, and its kind, until it is told to forget it: asked to
 * connect to one (BTL_CMD_CONNECT), it goes straight over. So the list is
 * this chip's, on every firmware alike, and needs nothing new of the second
 * chip's firmware: the last BT_KNOWN_DEVICES, newest first, by their
 * Bluetooth address, the oldest falling off the list and staying paired;
 * bt_link keeps the records in NVS (btlink/known). */
#ifndef BT_KNOWN_H
#define BT_KNOWN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_KNOWN_DEVICES 8          /* as many as the second chip keeps kinds for (KIND_MAX) */

/* One device's: written by one firmware and read by the next, so the layout
 * is frozen, and so are the eight at most. */
typedef struct __attribute__((packed)) {
    uint8_t bda[6];
    uint8_t kind;                   /* BTL_KIND_* */
    uint8_t spare;                  /* 0, a later firmware's; kept as it was */
    char    name[32];               /* NUL-terminated; empty: never heard */
} bt_known_rec_t;
_Static_assert(sizeof(bt_known_rec_t) == 40, "frozen");

typedef struct {
    bt_known_rec_t rec[BT_KNOWN_DEVICES];   /* newest first */
    int            n;
} bt_known_t;

/* From what NVS held: `len` bytes of records, newest first. One with no
 * address, or a device already read, is left out. */
void bt_known_load(bt_known_t *t, const void *blob, size_t len);

/* It has connected: to the front, with its name and kind; the oldest goes
 * when all are taken. An empty name leaves the one it had. True when the
 * records changed: false for the one in front as it was, and for no address
 * at all. */
bool bt_known_seen(bt_known_t *t, const uint8_t bda[6], const char *name, uint8_t kind);

/* Its name, as last heard: NULL for a device not on the list, or never heard. */
const char *bt_known_name(const bt_known_t *t, const uint8_t bda[6]);

/* Off the list: true when it was on it. */
bool bt_known_forget(bt_known_t *t, const uint8_t bda[6]);

#endif /* BT_KNOWN_H */
