/* The tailchaser firmware: the knob watching and steering TailChaser
 * repeaters (tailchaser.app) through their API. See tailchaser_client.c. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* What the page shows: the node in view, Tailscale, the link. No secrets. */
void tc_state_json(char *out, size_t cap);

/* The settings changed on the page: read them again. */
void tc_reload(void);

/* Tailscale left off this boot: it started twice without lasting a minute
 * (a crash). Saving its settings on the page tries it again. */
bool tc_ts_held(void);

/* The page's block buttons (it asks first): rx (which 0) or tx (1). */
void tc_request_block(int which, bool on);

/* The face's speaker: the repeaters' audio muted, every one of them, or not.
 * Kept in the settings. */
void tc_set_audio_mute(bool on);
