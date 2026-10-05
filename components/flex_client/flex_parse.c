/* The FlexRadio API's text, parsed: see flex_parse.h. */
#include "flex_parse.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* strlcpy, here so the host tests need nothing of newlib's. */
static void cpy(char *out, const char *in, size_t cap)
{
    if (!cap) return;
    size_t n = strlen(in);
    if (n >= cap) n = cap - 1;
    memcpy(out, in, n);
    out[n] = 0;
}

bool flex_kv(const char *line, const char *key, char *out, size_t cap)
{
    const size_t kl = strlen(key);
    for (const char *p = line; (p = strstr(p, key)) != NULL; p += kl) {
        if ((p == line || p[-1] == ' ') && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t n = strcspn(v, " ");
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return true;
        }
    }
    return false;
}

bool flex_kv_mhz(const char *line, const char *key, int64_t *hz)
{
    char v[24];
    if (!flex_kv(line, key, v, sizeof v)) return false;
    const bool neg = v[0] == '-';
    int64_t mhz = strtoll(neg ? v + 1 : v, NULL, 10), frac = 0;
    const char *d = strchr(v, '.');
    int n = 0;
    if (d)
        for (d++; *d >= '0' && *d <= '9' && n < 6; d++, n++) frac = frac * 10 + (*d - '0');
    for (; n < 6; n++) frac *= 10;
    *hz = (neg ? -1 : 1) * (mhz * 1000000 + frac);
    return true;
}

/* A value as the API sends it, its spaces back: 0x7F is a space, and a
 * control character is nothing. Trailing spaces go. */
static void unspace(const char *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && in[i] && o + 1 < cap; i++) {
        const unsigned char c = (unsigned char)in[i];
        if (c == 0x7F)     out[o++] = ' ';
        else if (c >= 0x20) out[o++] = (char)c;
    }
    while (o && out[o - 1] == ' ') o--;
    if (cap) out[o] = 0;
}

/* --- discovery --------------------------------------------------------------- */

/* A dotted quad and nothing else. */
static bool is_ipv4(const char *s)
{
    int parts = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        long v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s++ - '0');
            if (++digits > 3) return false;
        }
        if (v > 255) return false;
        parts++;
        if (*s == '.') {
            s++;
            if (!*s) return false;
        } else if (*s) {
            return false;
        }
    }
    return parts == 4;
}

bool flex_disc_parse(const uint8_t *b, size_t n, flex_disc_t *out)
{
    memset(out, 0, sizeof *out);
    out->port = 4992;
    size_t at = 0, end = n;
    /* VITA-49: the header word -- type in the top four bits, then the class
     * id's flag, the trailer's, the timestamps' kinds, the size in words --
     * the stream id, the class id (OUI, information and packet class), the
     * timestamps. Extension data with a stream id is type 3. */
    if (n >= 16 && (b[0] >> 4) == 3 && (b[0] & 0x08)) {
        const uint32_t oui = (uint32_t)b[9] << 16 | (uint32_t)b[10] << 8 | b[11];
        const uint16_t pcc = (uint16_t)(b[14] << 8 | b[15]);
        if (oui != 0x001C2D || pcc != 0xFFFF) return false;
        size_t hdr = 16;
        if ((b[1] >> 6) & 3) hdr += 4;              /* integer timestamp */
        if ((b[1] >> 4) & 3) hdr += 8;              /* fractional */
        /* To the datagram's end, not the size in words: the text is padded
         * with NULs to a whole word, which part ways as spaces do. */
        if ((b[0] & 0x04) && end >= hdr + 4) end -= 4;   /* the trailer */
        if (hdr >= end) return false;
        at = hdr;
    }
    char who_st[32] = "", who_host[32] = "", who_ip[32] = "";
    while (at < end) {
        /* A word: everything up to a space, a NUL or a line's end. */
        while (at < end && b[at] <= 0x20) at++;
        const size_t w0 = at;
        while (at < end && b[at] > 0x20) at++;
        const char *w = (const char *)b + w0;
        const size_t wl = at - w0;
        const char *eq = memchr(w, '=', wl);
        if (!eq) continue;
        const size_t kl = (size_t)(eq - w), vl = wl - kl - 1;
        const char *v = eq + 1;
#define KEY(k) (kl == sizeof(k) - 1 && !memcmp(w, k, kl))
        if (KEY("serial"))                   unspace(v, vl, out->serial, sizeof out->serial);
        else if (KEY("model"))               unspace(v, vl, out->model, sizeof out->model);
        else if (KEY("nickname"))            unspace(v, vl, out->nickname, sizeof out->nickname);
        else if (KEY("callsign"))            unspace(v, vl, out->callsign, sizeof out->callsign);
        else if (KEY("version"))             unspace(v, vl, out->version, sizeof out->version);
        else if (KEY("status"))              unspace(v, vl, out->status, sizeof out->status);
        else if (KEY("ip"))                  unspace(v, vl, out->ip, sizeof out->ip);
        else if (KEY("gui_client_stations")) unspace(v, vl, who_st, sizeof who_st);
        else if (KEY("inuse_host"))          unspace(v, vl, who_host, sizeof who_host);
        else if (KEY("inuse_ip"))            unspace(v, vl, who_ip, sizeof who_ip);
        else if (KEY("port")) {
            char p[8];
            unspace(v, vl, p, sizeof p);
            const long port = strtol(p, NULL, 10);
            if (port > 0 && port < 65536) out->port = (uint16_t)port;
        }
#undef KEY
    }
    if (!is_ipv4(out->ip)) out->ip[0] = 0;
    /* Who is on it: the stations by name, where it lists them, else the
     * address or the host that has it in use. */
    cpy(out->who, who_st[0] ? who_st : who_host[0] ? who_host : who_ip, sizeof out->who);
    return out->serial[0] != 0;
}

/* --- memories ---------------------------------------------------------------- */

int flex_mem_index(const char *body)
{
    if (strncmp(body, "memory ", 7) != 0) return -1;
    const char *p = body + 7;
    if (*p < '0' || *p > '9') return -1;
    char *e;
    const long v = strtol(p, &e, 10);
    if (v < 0 || v > 65535 || (*e != ' ' && *e != 0)) return -1;
    return (int)v;
}

/* A bare word in a status: "removed". */
static bool has_word(const char *body, const char *word)
{
    const size_t wl = strlen(word);
    for (const char *p = body; (p = strstr(p, word)) != NULL; p += wl)
        if ((p == body || p[-1] == ' ') && (p[wl] == ' ' || p[wl] == 0)) return true;
    return false;
}

void flex_mem_parse(const char *body, flex_mem_t *m, bool *removed)
{
    *removed = false;
    const int idx = flex_mem_index(body);
    if (idx < 0) return;
    m->idx = (uint16_t)idx;
    char v[40];
    if (has_word(body, "removed") || (flex_kv(body, "in_use", v, sizeof v) && !strcmp(v, "0"))) {
        *removed = true;
        return;
    }
    int64_t hz;
    if (flex_kv_mhz(body, "freq", &hz) && hz >= 0) m->hz = hz;
    if (flex_kv(body, "name", v, sizeof v)) unspace(v, strlen(v), m->name, sizeof m->name);
    if (flex_kv(body, "mode", v, sizeof v)) {
        size_t i = 0;
        for (; v[i] && i < sizeof m->mode - 1; i++) m->mode[i] = (char)(v[i] | 0x20);
        m->mode[i] = 0;
    }
    if (flex_kv(body, "repeater", v, sizeof v))
        m->duplex = !strcasecmp(v, "UP") ? 1 : !strcasecmp(v, "DOWN") ? -1 : 0;
    if (flex_kv_mhz(body, "repeater_offset", &hz)) m->offset_hz = (int32_t)(hz < 0 ? -hz : hz);
    if (flex_kv(body, "tone_mode", v, sizeof v)) m->tone_on = strcasecmp(v, "OFF") != 0;
    if (flex_kv(body, "tone_value", v, sizeof v)) {
        /* "79.7" in 0.1 Hz, without float. */
        long t = strtol(v, NULL, 10) * 10;
        const char *d = strchr(v, '.');
        if (d && d[1] >= '0' && d[1] <= '9') t += d[1] - '0';
        m->tone_dhz = t > 0 && t < 65536 ? (uint16_t)t : 0;
    }
    if (flex_kv(body, "rx_filter_low", v, sizeof v))  m->lo = (int32_t)strtol(v, NULL, 10);
    if (flex_kv(body, "rx_filter_high", v, sizeof v)) m->hi = (int32_t)strtol(v, NULL, 10);
}

int flex_mem_find(const flex_mem_t *list, int n, uint16_t idx)
{
    for (int i = 0; i < n; i++)
        if (list[i].idx == idx) return i;
    return -1;
}

bool flex_mem_drop(flex_mem_t *list, int *n, uint16_t idx)
{
    const int at = flex_mem_find(list, *n, idx);
    if (at < 0) return false;
    memmove(&list[at], &list[at + 1], (size_t)(*n - at - 1) * sizeof *list);
    (*n)--;
    return true;
}

static bool before(const flex_mem_t *a, const flex_mem_t *b)
{
    return a->hz < b->hz || (a->hz == b->hz && a->idx < b->idx);
}

int flex_mem_put(flex_mem_t *list, int *n, int cap, const flex_mem_t *m)
{
    const flex_mem_t keep = *m;                  /* m may be one of list's own */
    flex_mem_drop(list, n, keep.idx);
    if (*n >= cap) return -1;
    int at = 0;
    while (at < *n && before(&list[at], &keep)) at++;
    memmove(&list[at + 1], &list[at], (size_t)(*n - at) * sizeof *list);
    list[at] = keep;
    (*n)++;
    return at;
}

int flex_mem_nearest(const flex_mem_t *list, int n, int64_t hz)
{
    int best = -1;
    int64_t bd = 0;
    for (int i = 0; i < n; i++) {
        const int64_t d = list[i].hz > hz ? list[i].hz - hz : hz - list[i].hz;
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    return best;
}

int flex_mem_on(const flex_mem_t *list, int n, int64_t hz, int32_t tol)
{
    const int i = flex_mem_nearest(list, n, hz);
    if (i < 0) return -1;
    const int64_t d = list[i].hz > hz ? list[i].hz - hz : hz - list[i].hz;
    return d <= tol ? i : -1;
}

/* --- lists of names ---------------------------------------------------------- */

int flex_list_count(const char *list)
{
    int n = 0;
    for (const char *p = list; p && *p; ) {
        const size_t l = strcspn(p, ",");
        if (l) n++;
        p += l;
        if (*p) p++;
    }
    return n;
}

int flex_list_find(const char *list, const char *name)
{
    const size_t nl = strlen(name);
    int i = 0;
    for (const char *p = list; nl && p && *p; ) {
        const size_t l = strcspn(p, ",");
        if (l) {
            if (l == nl && !memcmp(p, name, nl)) return i;
            i++;
        }
        p += l;
        if (*p) p++;
    }
    return -1;
}
