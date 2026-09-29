/* SPDX-License-Identifier: MIT
 * SVXConnect-CLI — Copyright (c) 2026 Joeri Van Dooren
 *
 * Configuration: the part of the CLI's svx_config the knob uses, with the same
 * field names so the talkgroup manager and the node info need no changes.
 * The knob has no config file; svx_client.c fills this in from its settings
 * (the "svx" NVS namespace, edited on the web page).
 */
#ifndef SVX_CONFIG_H
#define SVX_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define SVX_MAX_TG      64
#define SVX_MAX_PRIO     3    /* "8+++" — three '+' is as deep as it goes */

/* A talkgroup as declared in the config: an id plus a priority given by the
 * number of trailing '+' characters.  8 => prio 0,  8+ => 1,  8++ => 2. */
typedef struct {
    uint32_t id;
    int      priority;
} svx_tg_entry;

typedef struct {
    /* ---- identity ---- */
    char     callsign[32];
    char     email[128];
    char     reflector[64];
    int      port;
    double   latitude;
    double   longitude;
    char     location[64];

    /* ---- talkgroups ---- */
    svx_tg_entry switchable[SVX_MAX_TG];
    int          n_switchable;
    svx_tg_entry monitored[SVX_MAX_TG];
    int          n_monitored;
    int      default_tg;
    int      lock_on_start;
    int      linger_seconds;
    int      idle_seconds;

    /* ---- audio ---- */
    int      mic_agc;
    int      mic_agc_target_pct;
    int      tail_trim_ms;
    int      roger_beep;
    int      roger_beep_min_sec;

    /* ---- ptt ---- */
    int      tx_timeout_sec;      /* hard un-key after this long; 0 disables */
} svx_config;

/* ---- talkgroup list helpers ---- */

/* Parse "8++, 1745+, 8000" into `out`. Returns the count, or -1 on a
 * malformed entry. Duplicate ids keep the highest priority seen. */
int  tglist_parse(const char *s, svx_tg_entry *out, int max);

/* Format back to "8++, 1745+, 8000". */
void tglist_format(char *dst, size_t cap, const svx_tg_entry *v, int n);

/* Priority of `id`: the monitored list wins, then switchable, else 0.
 * Returns -1 if `id` is in neither list (i.e. not watched at all). */
int  config_tg_priority(const svx_config *cfg, uint32_t id);

/* Is `id` in either list? */
int  config_tg_is_watched(const svx_config *cfg, uint32_t id);

#endif
