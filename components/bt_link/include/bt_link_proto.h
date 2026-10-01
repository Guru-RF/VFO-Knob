/* The link between the knob's two chips: the ESP32-S3, which runs the knob,
 * and the ESP32 beside it (ESP32-U4WDH), which has classic Bluetooth and runs
 * the companion firmware (companion/) -- a Bluetooth headset's audio gateway.
 *
 * One UART between them, 2 Mbit/s, 8N1, no flow control:
 *
 *   S3 GPIO38 (net ESP32S3_TX)  ->  ESP32 IO18
 *   S3 GPIO48 (net ESP32S3_RX)  <-  ESP32 IO23
 *
 * Shared by both firmwares: this header is the protocol, and both build it
 * unchanged.
 *
 * A frame:
 *
 *   0xA5 0x5A | type u8 | len u16 LE | payload[len] | crc16 LE
 *
 * The CRC is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type, len and
 * payload. A frame that fails it is dropped and the reader hunts for the next
 * 0xA5 0x5A: a byte lost on the wire costs one frame, never the link.
 *
 * Audio is 16-bit little-endian mono PCM at the knob's rates, which CMD_AUDIO
 * names: the companion converts to and from the headset's own (16 kHz with
 * mSBC, 8 kHz with CVSD), and its clock. Blocks are whatever the sender has;
 * the receiver buffers. */
#ifndef BT_LINK_PROTO_H
#define BT_LINK_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define BTL_SYNC0       0xA5
#define BTL_SYNC1       0x5A
#define BTL_BAUD        2000000
#define BTL_MAX_PAYLOAD 1024
#define BTL_PROTO       1          /* the version of this file's meaning */

enum {
    /* either way */
    BTL_HELLO        = 0x01,   /* u8 proto, u8 BTL_HELLO_*, the firmware's version as text */
    BTL_PING         = 0x02,   /* u32 seq */
    BTL_PONG         = 0x03,   /* u32 seq, as pinged */

    /* knob -> companion */
    BTL_CMD_SCAN     = 0x10,   /* u8 seconds: look for headsets */
    BTL_CMD_CONNECT  = 0x11,   /* bda[6]: pair if need be, and connect */
    BTL_CMD_DISCONNECT = 0x12,
    BTL_CMD_FORGET   = 0x13,   /* bda[6]: unpair it */
    BTL_CMD_AUDIO    = 0x14,   /* u8 on, u32 knob->headset rate, u32 headset->knob rate:
                                  the headset's audio open whenever it is connected */
    BTL_CMD_VOLUME   = 0x15,   /* u8 speaker 0-15, u8 mic 0-15 */
    BTL_CMD_STATE    = 0x16,   /* say EVT_STATE now */
    BTL_CMD_MIC      = 0x17,   /* u8 on: send the microphone (AUDIO_UP) -- only while keyed */

    /* companion -> knob */
    BTL_EVT_FOUND    = 0x20,   /* btl_found_t: a device the scan found */
    BTL_EVT_SCAN_DONE = 0x21,
    BTL_EVT_STATE    = 0x22,   /* see btl_state_t */
    BTL_EVT_BUTTON   = 0x23,   /* u8 BTL_BTN_*: a headset's button, as it says it */
    BTL_EVT_VOLUME   = 0x24,   /* u8 speaker, u8 mic: the headset's own change */
    BTL_EVT_LOG      = 0x25,   /* text: the companion's log, for the knob's */

    /* audio */
    BTL_AUDIO_DN     = 0x30,   /* knob -> headset's ear */
    BTL_AUDIO_UP     = 0x31,   /* headset's microphone -> knob */
};

/* HELLO's flags. A side sends HELLO with ASK when it starts, until it hears
 * the other; a HELLO with ASK is answered with one without. Either side can
 * restart without the other, and neither ever answers an answer. */
enum { BTL_HELLO_ASK = 1 };

/* What the headset's link is doing. */
enum { BTL_LINK_IDLE = 0, BTL_LINK_CONNECTING, BTL_LINK_CONNECTED };
enum { BTL_AUDIO_NONE = 0, BTL_AUDIO_CVSD_8K, BTL_AUDIO_MSBC_16K };

/* A headset's buttons, as the hands-free profile carries them. */
enum {
    BTL_BTN_ANSWER = 1,        /* ATA */
    BTL_BTN_HANGUP,            /* AT+CHUP */
    BTL_BTN_REDIAL,            /* AT+BLDN */
    BTL_BTN_VOICE_ON,          /* AT+BVRA=1 */
    BTL_BTN_VOICE_OFF,         /* AT+BVRA=0 */
};

typedef struct __attribute__((packed)) {
    uint8_t  link;             /* BTL_LINK_* */
    uint8_t  audio;            /* BTL_AUDIO_* */
    uint8_t  bda[6];           /* the headset, or zeros */
    int8_t   rssi;
    uint8_t  spk, mic;         /* volumes, 0-15 */
    char     name[32];         /* the headset's name, NUL-terminated */
    uint8_t  remembered;       /* a headset it reconnects to: bda and name are it */
    uint8_t  scanning;
} btl_state_t;

/* EVT_FOUND's payload: a device that answered the scan. */
typedef struct __attribute__((packed)) {
    uint8_t  bda[6];
    uint32_t cod;              /* class of device: 0x0400 in its major class is audio */
    int8_t   rssi;
    char     name[32];
} btl_found_t;

/* CRC-16/CCITT-FALSE. */
static inline uint16_t btl_crc16(uint16_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* A frame into `out` (len + 7 bytes); returns its length. */
static inline size_t btl_frame(uint8_t *out, uint8_t type, const void *payload, uint16_t len)
{
    out[0] = BTL_SYNC0;
    out[1] = BTL_SYNC1;
    out[2] = type;
    out[3] = (uint8_t)len;
    out[4] = (uint8_t)(len >> 8);
    for (uint16_t i = 0; i < len; i++) out[5 + i] = ((const uint8_t *)payload)[i];
    const uint16_t crc = btl_crc16(0xFFFF, out + 2, (size_t)len + 3);
    out[5 + len] = (uint8_t)crc;
    out[6 + len] = (uint8_t)(crc >> 8);
    return (size_t)len + 7;
}

/* A reader, fed a byte at a time: btl_rx_put() returns 1 when a whole frame
 * has arrived, in r->type, r->len and r->buf. */
typedef struct {
    uint8_t  state;
    uint8_t  type;
    uint16_t len, got;
    uint8_t  crc_lo;
    uint8_t  buf[BTL_MAX_PAYLOAD];
    uint32_t bad;              /* frames dropped: their CRC, or too long */
} btl_rx_t;

static inline int btl_rx_put(btl_rx_t *r, uint8_t b)
{
    switch (r->state) {
    case 0: r->state = b == BTL_SYNC0 ? 1 : 0; return 0;
    case 1: r->state = b == BTL_SYNC1 ? 2 : b == BTL_SYNC0 ? 1 : 0; return 0;
    case 2: r->type = b; r->state = 3; return 0;
    case 3: r->len = b; r->state = 4; return 0;
    case 4:
        r->len |= (uint16_t)b << 8;
        r->got = 0;
        if (r->len > BTL_MAX_PAYLOAD) { r->bad++; r->state = 0; return 0; }
        r->state = r->len ? 5 : 6;
        return 0;
    case 5:
        r->buf[r->got++] = b;
        if (r->got == r->len) r->state = 6;
        return 0;
    case 6: r->crc_lo = b; r->state = 7; return 0;
    case 7: {
        r->state = 0;
        uint8_t h[3] = { r->type, (uint8_t)r->len, (uint8_t)(r->len >> 8) };
        uint16_t crc = btl_crc16(0xFFFF, h, 3);
        crc = btl_crc16(crc, r->buf, r->len);
        if (crc != (uint16_t)(r->crc_lo | (uint16_t)b << 8)) { r->bad++; return 0; }
        return 1;
    }
    }
    r->state = 0;
    return 0;
}

#endif /* BT_LINK_PROTO_H */
