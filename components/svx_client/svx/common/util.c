/* SPDX-License-Identifier: MIT
 * SVXConnect-CLI — Copyright (c) 2026 Joeri Van Dooren
 *
 * The knob's side of util.h, log.h and the talkgroup-list half of config.h:
 * only what the shared modules call. The string and list functions are the
 * CLI's own; the clock and the logger are the ESP32's.
 */
#include "util.h"
#include "log.h"
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "svx";

/* ------------------------------------------------------------------ clock */

uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* ---------------------------------------------------------------- logging */

#define LOG_BODY(level)                                      \
    do {                                                     \
        char    line[160];                                   \
        va_list ap;                                          \
        va_start(ap, fmt);                                   \
        vsnprintf(line, sizeof line, fmt, ap);               \
        va_end(ap);                                          \
        ESP_LOG_LEVEL_LOCAL(level, TAG, "%s", line);         \
    } while (0)

void log_err (const char *fmt, ...) { LOG_BODY(ESP_LOG_ERROR); }
void log_warn(const char *fmt, ...) { LOG_BODY(ESP_LOG_WARN); }
void log_info(const char *fmt, ...) { LOG_BODY(ESP_LOG_INFO); }
void log_dbg (const char *fmt, ...) { LOG_BODY(ESP_LOG_DEBUG); }

/* ---------------------------------------------------------------- strings */

char *str_trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

void str_upper(char *s) {
    if (!s) return;
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

int str_ieq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    }
    return *a == *b;
}

void call_strip_ssid(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; src[i] && src[i] != '-' && i + 1 < cap; i++) {
            dst[i] = (char)toupper((unsigned char)src[i]);
        }
    }
    dst[i] = '\0';
}

void maidenhead(char *dst, size_t cap, double lat, double lon) {
    if (cap == 0) return;
    dst[0] = '\0';
    /* Exactly (0,0) means "no position configured", not the Gulf of Guinea. */
    if (lat == 0.0 && lon == 0.0) return;
    if (cap < 7) return;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return;

    double la = lat + 90.0;    /* 0 .. 180 */
    double lo = lon + 180.0;   /* 0 .. 360 */

    int f1 = (int)(lo / 20.0);
    int f2 = (int)(la / 10.0);
    lo -= f1 * 20.0;
    la -= f2 * 10.0;

    int s1 = (int)(lo / 2.0);
    int s2 = (int)(la / 1.0);
    lo -= s1 * 2.0;
    la -= s2 * 1.0;

    int t1 = (int)(lo / (2.0 / 24.0));
    int t2 = (int)(la / (1.0 / 24.0));

    /* Clamp: floating point at an exact pole/meridian can push an index out. */
    f1 = CLAMP(f1, 0, 17); f2 = CLAMP(f2, 0, 17);
    s1 = CLAMP(s1, 0, 9);  s2 = CLAMP(s2, 0, 9);
    t1 = CLAMP(t1, 0, 23); t2 = CLAMP(t2, 0, 23);

    dst[0] = (char)('A' + f1);
    dst[1] = (char)('A' + f2);
    dst[2] = (char)('0' + s1);
    dst[3] = (char)('0' + s2);
    dst[4] = (char)('a' + t1);
    dst[5] = (char)('a' + t2);
    dst[6] = '\0';
}

/* ------------------------------------------------------------ talkgroups */

int tglist_parse(const char *s, svx_tg_entry *out, int max) {
    int n = 0;
    if (!s) return 0;

    const char *p = s;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;

        if (!isdigit((unsigned char)*p)) {
            log_warn("config: talkgroup list: unexpected '%c' in \"%s\"", *p, s);
            return -1;
        }
        char *end = NULL;
        unsigned long id = strtoul(p, &end, 10);
        if (id > 0xFFFFFFFFUL) {
            log_warn("config: talkgroup %lu out of range", id);
            return -1;
        }
        p = end;

        int prio = 0;
        while (*p == '+') { prio++; p++; }
        if (prio > SVX_MAX_PRIO) {
            log_warn("config: talkgroup %lu: %d '+' is more than the %d supported",
                     id, prio, SVX_MAX_PRIO);
            prio = SVX_MAX_PRIO;
        }

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p && *p != ',') {
            log_warn("config: talkgroup list: unexpected '%c' after %lu", *p, id);
            return -1;
        }

        /* Duplicate ids collapse, keeping the highest priority seen. */
        int dup = -1;
        for (int i = 0; i < n; i++) if (out[i].id == (uint32_t)id) { dup = i; break; }
        if (dup >= 0) {
            if (prio > out[dup].priority) out[dup].priority = prio;
            continue;
        }
        if (n >= max) {
            log_warn("config: more than %d talkgroups in a list, ignoring the rest", max);
            break;
        }
        out[n].id       = (uint32_t)id;
        out[n].priority = prio;
        n++;
    }
    return n;
}

void tglist_format(char *dst, size_t cap, const svx_tg_entry *v, int n) {
    if (cap == 0) return;
    dst[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        char item[32];
        char plus[SVX_MAX_PRIO + 1];
        int  k = 0;
        for (; k < v[i].priority && k < SVX_MAX_PRIO; k++) plus[k] = '+';
        plus[k] = '\0';
        int m = snprintf(item, sizeof(item), "%s%lu%s",
                         i ? ", " : "", (unsigned long)v[i].id, plus);
        if (m < 0 || used + (size_t)m >= cap) break;
        memcpy(dst + used, item, (size_t)m + 1);
        used += (size_t)m;
    }
}

int config_tg_priority(const svx_config *cfg, uint32_t id) {
    for (int i = 0; i < cfg->n_monitored; i++)
        if (cfg->monitored[i].id == id) return cfg->monitored[i].priority;
    for (int i = 0; i < cfg->n_switchable; i++)
        if (cfg->switchable[i].id == id) return cfg->switchable[i].priority;
    return -1;
}

int config_tg_is_watched(const svx_config *cfg, uint32_t id) {
    return config_tg_priority(cfg, id) >= 0;
}
