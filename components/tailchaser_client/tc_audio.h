/* TailChaser's audio: what the repeater in focus transmits, from its
 * ws://node:8080/monitor/tx (TailChaser's src/monitor.h), played on the
 * knob's jack and -- through audio_out's tap -- a Bluetooth headset or
 * speaker; and its level, for the face's thin line.
 *
 * One task, its stack in PSRAM: it never touches flash. The node sends only
 * while it transmits -- 100 ms of 16-bit mono PCM at 16 kHz a message, the
 * tail of an over shorter -- and an empty ping every 5 s otherwise. Its
 * listening is off unless the node's [status] monitor = true: then the
 * status says "monitor": false, and the socket answers 404. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tc_proto.h"

typedef enum {
    TCA_OFF = 0,        /* nothing to play: no repeater, no network yet      */
    TCA_CONNECTING,
    TCA_LISTENING,      /* connected: what it transmits is played            */
    TCA_NO_MONITOR,     /* its listening is off ([status] monitor)           */
    TCA_BUSY,           /* four listeners there already                      */
    TCA_FAILED,         /* no answer, refused, cut: tried again shortly      */
} tca_state_t;

bool tc_audio_start(void);

/* The repeater to play, by its address; id changes whenever it is another
 * one, or the list was set anew. monitor: its status says its listening is
 * on (false: not asked, TCA_NO_MONITOR). NULL: none, the stream closed. */
void tc_audio_focus(const tc_addr_t *a, uint32_t id, bool monitor);

/* Muted, for every repeater: nothing played; the stream goes on, for the
 * level on the face. */
void tc_audio_set_mute(bool on);
bool tc_audio_muted(void);

/* The voice's peak in the last tenth of a second, dBFS; -120 when nothing
 * came for a while. */
float tc_audio_level(void);
tca_state_t tc_audio_state(void);
const char *tc_audio_state_word(tca_state_t s);
/* Messages of audio taken since the start, for the page and the tests. */
uint32_t tc_audio_chunks(void);
