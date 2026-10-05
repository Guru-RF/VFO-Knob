/* A Bluetooth headset or speaker for the knob, through its second chip.
 *
 * The board carries an ESP32 beside the ESP32-S3, with classic Bluetooth --
 * which the S3 has not -- and a UART between the two. The companion firmware
 * on it (companion/) is a headset's audio gateway, as a phone is, or a
 * speaker's music source; this is the knob's end of the link
 * (bt_link_proto.h).
 *
 * While a headset is connected, the knob's audio plays in it as well as on
 * the jack, its microphone is the one keying takes, and its button -- the one
 * that answers and ends a phone call -- is a second PTT, as the glass's is:
 * a press keys, the next unkeys.
 *
 * A speaker plays the knob's audio as the jack does, a fifth of a second or
 * so behind it, and nothing else changes: keying takes the knob's own
 * microphone, and the speaker's buttons key nothing. None of the headset's
 * calls below ever says yes for one. The second chip tells the two apart
 * itself, and the configuration page can say otherwise (bt_link_set_kind).
 * A speaker that takes its volume from its source has the knob's VOLUME for
 * its own (bt_link_set_volume). */
#ifndef BT_LINK_H
#define BT_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bt_link_proto.h"
#include "esp_err.h"

esp_err_t bt_link_init(void);

typedef struct {
    bool        companion;          /* it answers */
    char        version[33];        /* its firmware's */
    uint8_t     proto, flags;       /* from its HELLO: BTL_PROTO, BTL_HELLO_* */
    uint32_t    pings, pongs, bad;  /* the link's health */
    uint32_t    rtt_us;             /* the last ping's round trip */
    btl_state_t hs;                 /* the device, as the companion last said: a headset, or a speaker (kind) */
    uint32_t    presses;            /* the headset's button, all told */
    uint32_t    up_frames;          /* microphone frames in */
} bt_link_status_t;
void bt_link_status(bt_link_status_t *out);

/* A headset's (not a speaker's) audio is open: it hears the knob, and its
 * microphone is what keying sends. (While a headset is connected at all, the
 * knob's own microphone is off.) */
bool bt_link_headset_audio(void);

/* A headset (not a speaker) is connected, its audio open or not. */
bool bt_link_headset_connected(void);

/* Whether the device's audio is to be open while it is connected: always,
 * by default, as a radio's audio never stops. The telephone opens it for its
 * calls only, and closes it after: the headset -- or the speaker -- rests
 * between them. */
void bt_link_want_audio(bool on);

/* A connected headset (not a speaker) has its microphone muted -- it says so
 * as a gain of 0, as the Jabras do. */
bool bt_link_headset_muted(void);

/* The battery of the device connected -- a headset's, or a speaker's -- as
 * it last reported it over its hands-free link (Apple's AT+IPHONEACCEV, in
 * tenths, or the profile's own indicator): 0-100 %. -1 unknown: none
 * connected, nothing reported since it connected, or a second chip whose
 * firmware came before batteries (bt_link_proto.h's protocol 5). */
int bt_link_battery(void);

/* A speaker is connected (bt_link_proto.h's BTL_KIND_SPEAKER). */
bool bt_link_speaker_connected(void);
/* ...and its stream is open. */
bool bt_link_speaker_audio(void);
/* How far behind the jack an open speaker plays, in ms: the companion's
 * figure, 250 without one; 0 with none open. For the telephone's hold. */
uint32_t bt_link_speaker_delay_ms(void);
/* The configuration page's choice for a device: BTL_KIND_HEADSET or
 * BTL_KIND_SPEAKER. The companion keeps it, and calls the device again as
 * that if it is the one connected. Only to a companion that plays to
 * speakers (its HELLO's BTL_HELLO_SPEAKERS): false, and nothing sent,
 * otherwise. */
bool bt_link_set_kind(const uint8_t bda[6], uint8_t kind);

/* A speaker's volume. One that takes its volume from its source -- AVRCP's
 * absolute volume, as the companion finds -- has the knob's VOLUME for its
 * own: it turns itself to it, and is sent the knob's audio at a quarter of
 * full level, 12 dB down (the jack keeps the VOLUME) -- less, in proportion,
 * what it says it plays above the VOLUME, and rising into it over a second
 * or so; its own buttons or knob turn the knob's VOLUME. One that does not
 * keeps its own, and is sent the audio at a quarter of the jack's loudness
 * -- as is one that did not take the knob's (it answered louder than asked,
 * or not at all). */
/* The knob's VOLUME, 0-100, as it stands: from the knob's loop, every pass.
 * Sent on to the companion as it changes, four times a second at the most,
 * which sets it on a speaker that takes it. */
void bt_link_set_volume(uint8_t vol);
/* A turn of the speaker's own, as the knob's VOLUME: true once for each,
 * for the knob's loop to make it its VOLUME. */
bool bt_link_take_volume(uint8_t *vol);
/* What the speaker connected does with the knob's VOLUME. */
enum {
    BT_VOL_NONE = 0,            /* no speaker connected */
    BT_VOL_OWN,                 /* it keeps its own: the knob scales what it sends */
    BT_VOL_ASKING,              /* it takes the knob's, which is on its way to it */
    BT_VOL_KNOB,                /* its volume is the knob's VOLUME */
    BT_VOL_REFUSED,             /* it takes its source's, but did not take the knob's: it keeps its own,
                                   the knob scales what it sends; the knob's next VOLUME tries again */
};
uint8_t bt_link_speaker_volume(void);

/* The boom arm as the PTT, an option on the configuration page: lowering it
 * -- the headset's microphone live -- transmits, raising it -- muted --
 * stops (app_main.c). Kept in NVS. */
bool bt_link_boom_ptt(void);
void bt_link_set_boom_ptt(bool on);

/* What the last scan found (its newest first), and whether one runs. */
int  bt_link_found(btl_found_t *out, int max);

/* The configuration page's buttons. */
void bt_link_scan(uint8_t seconds);
void bt_link_connect(const uint8_t bda[6]);
void bt_link_disconnect(void);
void bt_link_forget(const uint8_t bda[6]);

/* A press of the headset's button since the last call: the PTT's to toggle.
 * Counted only while a headset is connected -- not a speaker: the second
 * chip's firmware changes by itself now, and no firmware of it, faulty or
 * not, keys the radio without a headset there. */
bool bt_link_take_ptt(void);

/* ---- the second chip's own firmware --------------------------------------
 *
 * The knob updates its second chip over the link (bt_link_proto.h's
 * BTL_UPD_*). An image handed over is held in PSRAM until a quiet moment:
 * two minutes with no headset or speaker connected, no over and no call,
 * and the caller saying it is quiet. Then it goes over in a quarter of a
 * minute or so; the chip checks it -- its SHA-256, the image, its signature
 * against the firmware it runs -- restarts into it on trial, and keeps it
 * once the knob has heard it and said KEEP, or goes back by itself. An
 * over, a call, a headset or a speaker, the Bluetooth page's buttons or an
 * install of the knob's own stop a transfer at once; it goes again at the
 * next quiet moment.
 *
 * All of it runs in the link's own task, from PSRAM: no flash, no NVS. */

/* What the update is doing. */
enum {
    BT_UPD_IDLE = 0,            /* no image held */
    BT_UPD_WAITING,             /* one held, for a quiet moment */
    BT_UPD_SENDING,
    BT_UPD_CHECKING,            /* all sent: the chip checks it */
    BT_UPD_RESTARTING,          /* checked and switched to: the chip restarts into it */
    BT_UPD_TRIAL,               /* it runs on trial: the knob says KEEP */
};
/* How the last one ended. */
enum {
    BT_UPD_NONE = 0,
    BT_UPD_KEPT,                /* it runs, and its INFO says VALID */
    BT_UPD_WENT_BACK,           /* why: BTL_BACK_* */
    BT_UPD_STOPPED,             /* why: BTL_UPD_WHY_* or BT_UPD_WHY_*; again at a quiet moment */
    BT_UPD_FAILED,
    BT_UPD_REFUSED,
    BT_UPD_UNKNOWN,             /* no word of it after its restart -- or none that it was kept */
};
/* The knob's own whys, beyond the protocol's BTL_UPD_WHY_*: never on the wire. */
enum {
    BT_UPD_WHY_OVER = 0x80,     /* stopped: an over */
    BT_UPD_WHY_CALL,            /* ...a call */
    BT_UPD_WHY_PAGE,            /* ...the Bluetooth page's buttons */
    BT_UPD_WHY_HELD,            /* ...the knob's supervisor held up (an install, the WiFi setup) */
    BT_UPD_WHY_GONE,            /* ...the chip went quiet */
    BT_UPD_WHY_NO_ANSWER,       /* failed: BEGIN or END never answered */
    BT_UPD_WHY_STALLED,         /* failed: the chip stopped taking the image */
};

/* What the knob remembers of the second chip's updates across its own
 * restarts -- the caller keeps it in NVS (main/app_main.c, btlink/comp) and
 * says it here: one image, by its identity, how many times the chip
 * restarted into it without keeping it, and whether it is never to go
 * again, and why. */
#define BT_UPD_TRIES 3                  /* restarts into one image not kept: then no more, until a newer one */
typedef struct {
    uint8_t        sha8[8];
    uint8_t        tries;       /* the chip's DONEs for it: each a restart into it */
    uint8_t        result, why; /* never again: how it ended (BT_UPD_*) and why; BT_UPD_NONE: it may go */
    char           ver[17];
} bt_link_upd_record_t;

typedef struct {
    uint8_t        phase;       /* BT_UPD_IDLE.. */
    uint8_t        percent;     /* of the image the chip has written */
    char           to[17];      /* the image held, or the last one: its version */
    uint8_t        to_sha[8];   /* ...its identity (app_elf_sha256's first 8 bytes) */
    char           from[17];    /* ...the chip's firmware when it was handed over */
    bool           forced;      /* ...to go whatever the versions */
    uint8_t        result;      /* how the last try ended: BT_UPD_NONE.. */
    uint8_t        why;         /* ...why: BTL_UPD_WHY_*, BT_UPD_WHY_*, or BTL_BACK_* */
    bool           block;       /* ...that image is never to go to the chip again */
    uint8_t        last_sha[8]; /* ...that image */
    char           text[96];    /* ...in words */
    uint32_t       seq;         /* results so far: one more with each */
    uint32_t       dones;       /* the chip's DONEs so far: each a try of the image */
    bool           info;        /* the chip's INFO heard since its last start */
    btl_upd_info_t chip;        /* ...what it said */
    bool           remembered;  /* the caller's record (bt_link_update_record()): */
    bt_link_upd_record_t rec;
} bt_link_upd_t;
void bt_link_update_status(bt_link_upd_t *out);
/* The record, as the caller has it now; NULL: none. */
void bt_link_update_record(const bt_link_upd_record_t *rec);

/* An image for the second chip, taken over: `img` is the caller's PSRAM
 * allocation, its form checked (ota_companion_image_ok) and `sha256` its
 * SHA-256, and from here on this module's to free -- once the chip keeps it,
 * goes back from it, or refuses it for good, or it has failed three tries.
 * Otherwise it is held for the next quiet moment. ESP_ERR_INVALID_STATE, and
 * the caller still owns it, while one is held, or the chip is not there,
 * takes no updates, or has not yet said what it runs (its INFO). `forced`
 * sends it to a development build too; without, a chip that is not a
 * release, or no longer the firmware it was when handed over, has it
 * dropped. The headset's or speaker's part of a quiet moment is this
 * module's. */
esp_err_t bt_link_update_start(uint8_t *img, size_t len, const uint8_t sha256[32], bool forced);
bool      bt_link_update_holding(void);

/* The caller's part of a quiet moment, every pass of its loop: this
 * firmware settled, the radio idle, nothing installing. Also the dead man's
 * switch: unsaid for 10 s, a transfer stops (BT_UPD_WHY_HELD). */
void bt_link_update_allow(bool quiet);
/* An over or a call: asked ten times a second while an image is held, from
 * the link's task -- reads only. */
void bt_link_update_busy_cb(bool (*busy)(void));
/* Stop a transfer now, from any task: why is BTL_UPD_WHY_KNOB before this
 * chip's flash is written. Again at the next quiet moment. */
void bt_link_update_stop(uint8_t why);

/* That image -- its app_elf_sha256's first 8 bytes -- is not to go to the
 * chip again: its INFO says it went back from it for a real failure, its
 * last transfer from here ended in a way no retry mends, or the record says
 * so (never again, or BT_UPD_TRIES restarts into it, none kept). Why, in
 * words, into `why`. */
bool bt_link_update_blocked(const uint8_t app_sha[8], char *why, size_t cap);

#endif /* BT_LINK_H */
