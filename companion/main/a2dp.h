/* A speaker: classic Bluetooth's A2DP, this chip as its source -- a phone
 * playing music to it. The knob's audio, mono at its own rate, converted to
 * 44.1 kHz (hfp.c's downlink) and sent as the stack's SBC, the one channel
 * in both; and its volume, the knob's, over AVRCP where it takes it. hfp.c
 * owns the device and decides; this is the profile. */
#ifndef A2DP_H
#define A2DP_H

#include <stdbool.h>
#include <stdint.h>

/* The two requests of a stream, for hfp_av_media()'s answers. */
enum { A2DP_START = 1, A2DP_SUSPEND };

/* From hfp_init(), after esp_hf_ag_init(). */
void a2dp_init(void);
void a2dp_connect(const uint8_t bda[6]);
void a2dp_disconnect(const uint8_t bda[6]);
/* Its stream: asked, answered in hfp_av_media(). Start checks first that
 * the link can stream at all (CHECK_SRC_RDY), then starts. */
void a2dp_start(void);
void a2dp_suspend(void);

/* Its volume: AVRCP's absolute volume, the knob's VOLUME as the speaker's
 * own (bt_link_proto.h's BTL_AV_*). Only the speaker's own remote control is
 * set or heard: `spk`, in each call that takes it, is the address of the
 * speaker whose A2DP is up -- NULL, none -- and a remote control of any
 * other device (one at a time, the stack's rule) counts for nothing.
 *
 * The knob's VOLUME, 0-100 (the knob's command, on link_rx): kept, and set
 * on the speaker now, or after the set under way -- one at a time, the
 * latest wins -- or as soon as one that takes it comes. The same again is
 * no news. */
void    a2dp_volume(uint8_t pct, const uint8_t *spk);
/* BTL_AV_* as they stand; its volume as it last said (0-127, 0xFF none) and
 * its own changes so far, for the state. */
uint8_t a2dp_volume_state(const uint8_t *spk, uint8_t *vol, uint8_t *turns);
/* Its volume is the knob's: the knob's AUDIO_DN_FULL plays (link_rx). */
bool    a2dp_volume_full(void);
/* Its stream may start: its volume is the knob's, or will not be -- it
 * takes none, refused it, or the knob sets none -- not still to come. */
bool    a2dp_volume_settled(const uint8_t *spk);
/* The knob started afresh: its VOLUME may be another now -- no longer set
 * until it says it again. And what its hello says: whether it sets a
 * speaker's volume at all (BTL_HELLO_AV_VOLUME), in each. */
void    a2dp_knob_started(void);
void    a2dp_knob_av(bool av);
/* The main loop's, every tick: sets unanswered, its lines (the speaker's
 * name, "" none known; NULL: the device is not a speaker, no lines), and
 * the state when it changed. */
void    a2dp_tick(const char *speaker, const uint8_t *spk);

/* The stream's own numbers, since a2dp_counts_reset() (at each open). */
typedef struct {
    uint32_t made;                      /* SBC frames made of the knob's audio */
    uint32_t sent;                      /* ...that the stack took from its queue for the air */
    uint32_t dropped;                   /* ...that its queue dropped, memory running short */
    uint32_t waiting;                   /* ...in its queue now: made, neither taken nor dropped */
    uint32_t waiting_max;               /* ...the most seen */
    uint32_t spf;                       /* stereo samples an SBC frame: 128, 16 blocks of 8 subbands */
    uint32_t btc_stack_free;            /* the stack's BTC task: its stack never used, 0 unknown */
} a2dp_counts_t;
void a2dp_counts(a2dp_counts_t *out);
/* At each open, on the BTC task. */
void a2dp_counts_reset(void);

#endif /* A2DP_H */
