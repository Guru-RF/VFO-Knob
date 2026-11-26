/* CI-V as a byte stream: what the icombt firmware (VFO_RADIO=icombt) needs
 * of the IC-705's Bluetooth serial port (SPP), where CI-V comes as bytes on
 * an RFCOMM link and not, as on the LAN, a datagram of whole frames. Plain C,
 * so test/host checks it on the PC (test_civ_link); the link itself is
 * bt_link's, the CI-V meaning icom_client's.
 *
 * The IC-705 Basic Manual, SET > Bluetooth Set > Data Device Set >
 * Serialport Function: "CI-V (Echo Back OFF)", the default, "Transmits or
 * receives a CI-V command"; its CI-V Reference Guide the frames:
 * FE FE <to> <from> <cmd> [sub] [data] FD. */
#ifndef CIV_LINK_H
#define CIV_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The Serial Port Profile's class UUID, 0x1101 on the Bluetooth base UUID,
 * most significant byte first -- what bt_link_radio_mode() and the second
 * chip's SDP search take. */
#define CIV_SPP_UUID                                                                                          \
    { 0x00, 0x00, 0x11, 0x01, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB }

/* The longest frame kept. An IC-705 memory channel's answer (1A 00) is the
 * longest it sends of what the knob asks, under 100 bytes; anything longer
 * is not a frame of ours, and dropped. */
#define CIV_FRAME_MAX 128

/* Bytes into frames. A frame starts at FE FE and ends at the next FD; bytes
 * before an FE FE are junk (a link that opened mid-frame), and a collision
 * (FC) or a frame that runs past CIV_FRAME_MAX is dropped whole. */
typedef struct {
    uint8_t  buf[CIV_FRAME_MAX];
    uint16_t len;
    uint32_t frames, dropped, junk;   /* counts, for the log */
} civ_rx_t;

void civ_rx_init(civ_rx_t *r);
/* Takes one byte: a whole frame's length when this byte ends one -- the
 * frame in r->buf until the next call -- else 0. */
size_t civ_rx_byte(civ_rx_t *r, uint8_t b);

/* A frame for the radio at `to` from `from`: FE FE to from body FD in out.
 * Its length, or 0 when it does not fit, the body is empty, or the body
 * holds an FD -- which would end it early. */
size_t civ_frame_build(uint8_t *out, size_t cap, uint8_t to, uint8_t from, const uint8_t *body, size_t n);

/* "aa:bb:cc:dd:ee:ff" (either case, '-' too) into six bytes, most
 * significant first. False, and out untouched, for anything else -- an IP
 * address or a name left from the WiFi firmware among them. */
bool civ_bda_parse(const char *s, uint8_t out[6]);

/* A device a scan found that is an Icom radio's Bluetooth unit: named as
 * Icom names them, "ICOM BT(IC-705)" by default (the Advanced Manual,
 * p. 10-7; an owner may append up to nine characters of their own). */
bool civ_is_icom_bt_name(const char *name);

/* A CI-V level (14 xx: 0-255) from a knob level of 0..full -- VOLUME's
 * 0-100 for the AF gain, MIC GAIN's 0-200 for the MIC gain, whose 100 (the
 * knob's unity) is then the radio's 50 % -- and back, rounded so that a
 * level set reads back as the one set. */
uint8_t civ_level_from(unsigned v, unsigned full);
unsigned civ_level_to(uint8_t level, unsigned full);

/* How long to wait before the next try at the link, after `fails` failures
 * in a row: 1 s, doubling, at most 30 s. */
uint32_t civ_redial_ms(uint32_t fails);

#endif /* CIV_LINK_H */
