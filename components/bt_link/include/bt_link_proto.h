/* The link between the knob's two chips: the ESP32-S3, which runs the knob,
 * and the ESP32 beside it (ESP32-U4WDH), which has classic Bluetooth and runs
 * the companion firmware (companion/) -- a Bluetooth headset's audio gateway,
 * or a Bluetooth speaker's music source.
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
 * mSBC, 8 kHz with CVSD), and its clock -- or to a speaker's, 44.1 kHz SBC,
 * the one channel in both of its. Blocks are whatever the sender has; the
 * receiver buffers. What goes to the device is at the jack's loudness, the
 * knob's VOLUME applied (AUDIO_DN) -- but a speaker whose own volume the
 * knob sets gets it before the VOLUME (AUDIO_DN_FULL), its own amplifier
 * turning it down. */
#ifndef BT_LINK_PROTO_H
#define BT_LINK_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define BTL_SYNC0       0xA5
#define BTL_SYNC1       0x5A
#define BTL_BAUD        2000000
#define BTL_MAX_PAYLOAD 1024
/* The version of this file's meaning: 2 brought the second chip's own
 * firmware over the link (BTL_UPD_*); 3 brought speakers -- a device's kind,
 * appended to btl_state_t and btl_found_t, the A2DP stream's audio, CMD_KIND
 * and HELLO's SPEAKERS; 4 a speaker's own volume -- AVRCP's absolute volume:
 * CMD_AV_VOLUME, AUDIO_DN_FULL, HELLO's AV_VOLUME, and the state's av
 * fields, appended; 5 the device's battery, as it reports it: the state's
 * batt fields, appended. Each side only logs the other's: neither ever
 * refuses a peer for it, and each ignores the types and the flag bits it
 * does not know, and the bytes past the end of a structure it knows. */
#define BTL_PROTO       5

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
    BTL_CMD_KIND     = 0x18,   /* bda[6], u8 BTL_KIND_*: the page's choice for that device */
    BTL_CMD_AV_VOLUME = 0x19,  /* u8 0-100: the knob's VOLUME, for a speaker that takes it as its
                                  own (btl_av_from_knob) -- to a companion that says AV_VOLUME:
                                  after each HELLO, as it changes (a few times a second at the
                                  most), and every 5 s while a speaker takes it. The companion
                                  keeps it, and sets each speaker that takes it as it comes; the
                                  same value again is no news -- only a reminder, for one lost */

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
    BTL_AUDIO_DN_FULL = 0x32,  /* knob -> a speaker whose own volume is the knob's (BTL_AV_SET):
                                  as AUDIO_DN, but before the knob's VOLUME. The companion plays
                                  it to that speaker only, and drops it for anything else -- a
                                  headset, a speaker it no longer sets -- rather than play it
                                  at full level there */

    /* The second chip's own firmware (companion/main/upd.c). Frozen once a
     * knob ships: only new types, new flag bits, or fields appended at the end
     * that a receiver may find missing (len says). Every knob firmware keeps
     * speaking every dialect a shipped chip has. */
    /* knob -> companion */
    BTL_UPD_BEGIN    = 0x40,   /* btl_upd_begin_t: an image follows */
    BTL_UPD_DATA     = 0x41,   /* u32 offset LE, then 1..BTL_UPD_CHUNK bytes of it */
    BTL_UPD_END      = 0x42,   /* u32 size LE: all sent -- check it, switch, restart */
    BTL_UPD_ABORT    = 0x43,   /* u8 BTL_UPD_WHY_*: stop, keep nothing */
    BTL_UPD_KEEP     = 0x44,   /* the knob hears you and reads your INFO: keep the firmware on trial */
    BTL_UPD_ASK      = 0x45,   /* say UPD_INFO now */
    /* companion -> knob */
    BTL_UPD_STATUS   = 0x48,   /* btl_upd_status_t: the answer to BEGIN, DATA, END, ABORT */
    BTL_UPD_INFO     = 0x49,   /* btl_upd_info_t: after each HELLO either way, on ASK, on KEEP, after keeping */
};

/* HELLO's flags. A side sends HELLO with ASK when it starts, until it hears
 * the other; a HELLO with ASK is answered with one without. Either side can
 * restart without the other, and neither ever answers an answer.
 *
 * UPDATE, the companion's only: it takes its own firmware over this link --
 * its bootloader can go back to the firmware before (its version is 2 or
 * more), it runs from ota_0 or ota_1, and it has the receiver.
 *
 * SPEAKERS, either way. The companion's: it plays to speakers (A2DP). The
 * knob's: it keeps its own microphone while a speaker plays -- a knob that
 * does not say so is never given a speaker: to it every device is a
 * headset, as before. Both say it in every HELLO, asking or answering.
 *
 * AV_VOLUME, either way, likewise. The companion's: it sets a speaker's own
 * volume when the knob says (CMD_AV_VOLUME), and takes AUDIO_DN_FULL. The
 * knob's: it sends both to a speaker that takes them. Without it on either
 * side, a speaker's own controls set its volume and the knob scales what it
 * sends, as before: a companion never sets a speaker's volume unasked. */
enum { BTL_HELLO_ASK = 1, BTL_HELLO_UPDATE = 2, BTL_HELLO_SPEAKERS = 4, BTL_HELLO_AV_VOLUME = 8 };

/* What the device's link is doing: a headset's hands-free link, a speaker's
 * A2DP. */
enum { BTL_LINK_IDLE = 0, BTL_LINK_CONNECTING, BTL_LINK_CONNECTED };
enum { BTL_AUDIO_NONE = 0, BTL_AUDIO_CVSD_8K, BTL_AUDIO_MSBC_16K, BTL_AUDIO_SBC_44K };

/* What a device is to the knob. A headset: its ear, its microphone and its
 * PTT (the hands-free profile). A speaker: an ear only (A2DP) -- the knob
 * keys its own microphone, and the speaker's buttons key nothing. 0 is a
 * headset: a companion before protocol 3 says nothing else. */
enum { BTL_KIND_HEADSET = 0, BTL_KIND_SPEAKER };
/* How the companion came to it. */
enum {
    BTL_KWHY_NONE = 0,         /* nothing known of it: a headset until it shows otherwise */
    BTL_KWHY_CLASS,            /* its class of device, and the services its scan answer lists */
    BTL_KWHY_DROPS,            /* it hung up a call's audio at once, mSBC and CVSD alike: a speaker */
    BTL_KWHY_NO_A2DP,          /* ...and then had no A2DP to play to: a headset after all */
    BTL_KWHY_USER,             /* the configuration page's choice */
};
/* A device's services, as the companion knows them: from the list its scan
 * answer (EIR) carries -- KNOWN when that list was the complete one. */
enum { BTL_SVC_HFP = 1, BTL_SVC_HSP = 2, BTL_SVC_A2DP = 4, BTL_SVC_AVRCP = 8, BTL_SVC_KNOWN = 0x80 };

/* A speaker's own volume -- AVRCP's absolute volume (1.4 on), its source
 * setting the speaker's own amplifier -- in the state's av. */
enum {
    BTL_AV_TAKES = 1,          /* it takes its volume from its source: it lists volume changes among
                                  its events. The knob sends it its VOLUME (CMD_AV_VOLUME) */
    BTL_AV_SET   = 2,          /* ...and it has: a volume of the knob's set on it since the knob
                                  last started. The knob sends it AUDIO_DN_FULL: its own amplifier
                                  is the VOLUME now */
    BTL_AV_REFUSED = 4,        /* ...or it has not: its first answer was louder than asked, or it
                                  answered none of three sets in a row. The knob scales what it
                                  sends, as for a speaker that keeps its own; the knob's next
                                  VOLUME tries again */
};

/* The knob's VOLUME, 0-100, as a speaker's own (0-127, AVRCP's scale), and
 * back; each the nearest. A VOLUME taken from the speaker and sent to it
 * again is the one it said: 0, 1, 2, 3 ... 99, 100 go to 0, 1, 3, 4 ... 126,
 * 127, and come back as they went. The other way, the speaker's 127 steps
 * fold into the knob's 101: its 2 is the knob's 2, sent back as its 3. */
static inline uint8_t btl_av_from_knob(uint8_t pct)
{
    return pct >= 100 ? 127 : (uint8_t)((pct * 127u + 50u) / 100u);
}
static inline uint8_t btl_av_to_knob(uint8_t av)
{
    return av >= 127 ? 100 : (uint8_t)((av * 100u + 63u) / 127u);
}

/* How the device reports its battery, over its hands-free link -- a
 * headset's, or a speaker's own -- in the state's batt. Unknown until it
 * does; the companion forgets it when the device goes. */
enum {
    BTL_BATT_NONE = 0,         /* nothing reported since it connected: unknown */
    BTL_BATT_APPLE,            /* Apple's AT+IPHONEACCEV, in tenths: 10-100 % */
    BTL_BATT_HFP,              /* the hands-free profile's own battery indicator (AT+BIEV), 0-100 % */
};

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
    uint8_t  bda[6];           /* the device -- a headset or a speaker -- or zeros */
    int8_t   rssi;
    uint8_t  spk, mic;         /* volumes, 0-15 */
    char     name[32];         /* its name, NUL-terminated */
    uint8_t  remembered;       /* a device it reconnects to: bda and name are it */
    uint8_t  scanning;
    /* Protocol 3 on; zeros from a companion before it (a headset). */
    uint8_t  kind;             /* BTL_KIND_* of the device above, as this knob is to treat it */
    uint8_t  kind_why;         /* BTL_KWHY_* */
    uint8_t  svc;              /* BTL_SVC_* */
    uint16_t delay_ms;         /* a speaker's audio open: how far behind the knob it plays; else 0 */
    /* Protocol 4 on; zeros from a companion before it (no volume of its own set). */
    uint8_t  av;               /* BTL_AV_*: a speaker's own volume -- 0 for a headset */
    uint8_t  av_volume;        /* ...with TAKES: its volume as it last said, 0-127; 0xFF none yet */
    uint8_t  av_turns;         /* ...with SET: its own changes -- its buttons, its knob -- counted
                                  since the companion started. One more: av_volume is the speaker's
                                  own doing, for the knob's VOLUME to follow */
    /* Protocol 5 on; zeros from a companion before it (no battery known). */
    uint8_t  batt;             /* BTL_BATT_*: how the device above reports its battery -- 0 not yet */
    uint8_t  batt_pct;         /* ...with one: its charge as it last said, 0-100 % */
} btl_state_t;                 /* 55 */

/* EVT_FOUND's payload: a device that answered the scan. */
typedef struct __attribute__((packed)) {
    uint8_t  bda[6];
    uint32_t cod;              /* class of device: 0x0400 in its major class is audio */
    int8_t   rssi;
    char     name[32];
    /* Protocol 3 on; zeros from a companion before it. */
    uint8_t  kind;             /* BTL_KIND_*: what the companion would make of it */
    uint8_t  kind_why;         /* BTL_KWHY_* */
    uint8_t  svc;              /* BTL_SVC_* */
} btl_found_t;                 /* 46 */

/* ---- the second chip's own firmware ------------------------------------
 *
 * The knob sends BEGIN; the chip answers READY (or REFUSED, why). DATA
 * follows from offset 0, at most BTL_UPD_WINDOW frames unanswered; each one
 * written is answered ACK{next}, and anything out of order ACK{next} again,
 * from where the knob goes on. END: the chip checks the bytes' SHA-256, the
 * image and its signature, switches to it and answers DONE, then restarts
 * into it, on trial. It keeps it once the knob has heard its INFO saying so
 * and sent KEEP -- or goes back to the firmware before, by itself. A
 * repeated BEGIN or END is answered again: either side may lose a frame. */
#define BTL_UPD_CHUNK  1020    /* 4 + 1020 = BTL_MAX_PAYLOAD */
#define BTL_UPD_WINDOW 4       /* DATA unanswered, at most: ~4.1 kB, half the chip's 8 kB RX ring */

typedef struct __attribute__((packed)) {
    uint32_t size;             /* whole 4 kB sectors (signed) */
    uint8_t  sha256[32];       /* of all `size` bytes */
    uint8_t  app_sha[8];       /* its app_elf_sha256 (offset 0xB0), first 8 bytes: its identity */
    char     version[16];      /* offset 0x30, NUL-padded */
} btl_upd_begin_t;             /* 60 */

typedef struct __attribute__((packed)) {
    uint8_t  state;            /* BTL_UPD_READY.. */
    uint8_t  why;              /* BTL_UPD_WHY_* */
    uint32_t next;             /* bytes written: the offset wanted next */
    int32_t  err;              /* esp_err_t or 0 */
} btl_upd_status_t;            /* 10 */

typedef struct __attribute__((packed)) {
    uint8_t  boot_ver;         /* esp_bootloader_desc_t.version: 2+ goes back */
    uint8_t  slot;             /* 0 ota_0, 1 ota_1, 0xFF neither */
    uint8_t  state;            /* BTL_RUN_* */
    uint8_t  flags;            /* BTL_INFO_RELEASE */
    uint8_t  app_sha[8];       /* the running firmware */
    uint8_t  back;             /* BTL_BACK_*: why the last new firmware went back, while its slot holds it */
    uint8_t  back_sha[8];
    char     back_ver[16];
} btl_upd_info_t;              /* 37 */

/* UPD_STATUS's state. */
enum {
    BTL_UPD_READY = 1,         /* BEGIN taken: DATA from 0 */
    BTL_UPD_ACK,               /* written up to next: DATA from there */
    BTL_UPD_DONE,              /* checked and switched to: restarting into it (next is its size) */
    BTL_UPD_REFUSED,           /* BEGIN not taken: why */
    BTL_UPD_FAILED,            /* the image or the flash failed: why, err */
    BTL_UPD_STOPPED,           /* given up, nothing kept: why */
};
/* Why: in UPD_STATUS, and the knob's ABORT. */
enum {
    BTL_UPD_WHY_NONE = 0,
    BTL_UPD_WHY_HEADSET,       /* a headset connecting, connected, in a call, or a scan */
    BTL_UPD_WHY_TRIAL,         /* the firmware running is on trial: none until it is kept */
    BTL_UPD_WHY_BUSY,          /* another image is coming in; from the knob, an over or a call */
    BTL_UPD_WHY_SIZE,          /* not whole sectors, more than a slot, or not all of it came */
    BTL_UPD_WHY_BEFORE,        /* this image went back here before, for a real failure */
    BTL_UPD_WHY_BOOTLOADER,    /* this chip's bootloader cannot go back: no updates until the bench */
    BTL_UPD_WHY_FLASH,         /* the flash said no: err */
    BTL_UPD_WHY_SHA,           /* the bytes are not BEGIN's SHA-256 */
    BTL_UPD_WHY_IMAGE,         /* the image did not verify (its form, hash, chip, signature): err */
    BTL_UPD_WHY_PROJECT,       /* a good image, but not of this chip's firmware */
    BTL_UPD_WHY_QUIET,         /* 15 s without DATA or END */
    BTL_UPD_WHY_KNOB,          /* the knob's ABORT */
    BTL_UPD_WHY_MEMORY,        /* no room for the transfer */
    BTL_UPD_WHY_NO_SESSION,    /* DATA, END or ABORT with no update going */
    BTL_UPD_WHY_RESTARTED,     /* the other side restarted mid-transfer */
};
/* INFO's state: the firmware running. */
enum {
    BTL_RUN_OTHER = 0,         /* otadata says neither: no entry, or one a firmware without rollback wrote */
    BTL_RUN_VALID,             /* kept */
    BTL_RUN_TRIAL,             /* on trial: KEEP it, or it goes back by itself */
};
enum { BTL_INFO_RELEASE = 1 }; /* a release (tools/release.sh): the knob keeps it up to date by itself */
/* INFO's back: why the last new firmware went back to the one before. */
enum {
    BTL_BACK_NONE = 0,
    BTL_BACK_POWER,            /* the power (or the EN pin) went during its trial or its start: it may come again */
    BTL_BACK_QUIET,            /* no knob kept it within 2 minutes: it may come again, a try */
    BTL_BACK_CRASHED,          /* it crashed on trial -- real from here on: never taken again */
    BTL_BACK_HUNG,             /* it hung on trial (the RTC watchdog) */
    BTL_BACK_EARLY,            /* it crashed or hung before its trial could begin, or would not load at all */
    BTL_BACK_GUARD,            /* kept, then it crashed 3 times in a row */
};
_Static_assert(sizeof(btl_upd_begin_t) == 60 && sizeof(btl_upd_status_t) == 10 &&
               sizeof(btl_upd_info_t) == 37, "frozen");
_Static_assert(4 + BTL_UPD_CHUNK == BTL_MAX_PAYLOAD, "DATA fills a frame");
/* Grown only at the end: a side before protocol 3 reads the first 45 and 43
 * bytes, one before 4 the state's first 50, one before 5 its first 53, and
 * one from it on finds zeros past what an older side sent. */
_Static_assert(sizeof(btl_state_t) == 55 && sizeof(btl_found_t) == 46, "appended only");

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
