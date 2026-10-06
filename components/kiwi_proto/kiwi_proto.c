/* KiwiSDR's protocol, the plain-C parts. See kiwi_proto.h.
 *
 * Checked against the servers' sources (jks-prv/KiwiSDR, RaspSDR/server):
 * /status is "key=value" lines; sdr_hw carries an hourglass (U+23F3) when the
 * owner has time limits; date is asctime in UTC; uptime counts seconds since
 * the server started. A KiwiSDR clears its per-address counters when it
 * restarts and once a day (a minute between 01:00 and 06:00 its local time); a
 * Web-888 only when it restarts.
 *
 * A session's MSGs are "key=value" lists; its SND frames carry IMA-ADPCM, two
 * samples a byte, the low nibble first, the decoder's state running on from
 * frame to frame for the whole session, as the server's encoder does. */
#include "kiwi_proto.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define HOURGLASS     "\xE2\x8F\xB3"    /* U+23F3, in UTF-8 */
#define RESTART_SLACK 120u              /* s: two reads of one boot differ by seconds */
#define DAY_S         86400u
#define SANE_UTC      1577836800u       /* 2020-01-01: before that, no clock set yet */

/* `n` bytes of src at most, cut where a character ends: a name cut short in
 * the middle of a UTF-8 sequence ("Brüssel") would be no text at all to a
 * page that shows it. */
static void cpyn(char *dst, size_t cap, const char *src, size_t n)
{
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xC0) == 0x80) n--;     /* a continuation byte: back */
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void cpy(char *dst, size_t cap, const char *src) { cpyn(dst, cap, src, strlen(src)); }

static bool prefix(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

void kiwi_status_parse(char *text, kiwi_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->users = out->users_max = out->ext_api = -1;
    if (!text) return;
    bool up = false;                         /* uptime=0 is said too: it just started */
    for (char *line = text; line && *line;) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = 0;
        const size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            const char *k = line, *v = eq + 1;
            if (!strcmp(k, "sdr_hw")) {
                out->ok      = true;
                out->tlimits = strstr(v, HOURGLASS) != NULL;
            } else if (!strcmp(k, "name"))       cpy(out->name, sizeof out->name, v);
            else if (!strcmp(k, "antenna"))      cpy(out->antenna, sizeof out->antenna, v);
            else if (!strcmp(k, "loc"))          cpy(out->loc, sizeof out->loc, v);
            else if (!strcmp(k, "sw_version")) {
                cpy(out->sw, sizeof out->sw, v);
                out->kind = prefix(v, "KiwiSDR") ? KIWI_KIND_KIWISDR
                          : prefix(v, "Web888") || prefix(v, "Web-888") ? KIWI_KIND_WEB888
                          : KIWI_KIND_UNKNOWN;
            }
            else if (!strcmp(k, "users"))        out->users = atoi(v);
            else if (!strcmp(k, "users_max"))    out->users_max = atoi(v);
            else if (!strcmp(k, "ext_api"))      out->ext_api = atoi(v);
            else if (!strcmp(k, "freq_offset"))  out->offset_khz = strtod(v, NULL);
            else if (!strcmp(k, "offline"))      out->offline = !strcmp(v, "yes");
            else if (!strcmp(k, "status"))       out->offline |= !strcmp(v, "offline");
            else if (!strcmp(k, "uptime"))       { out->uptime_s = (uint32_t)strtoul(v, NULL, 10); up = true; }
            else if (!strcmp(k, "date")) {
                uint32_t t;
                if (kiwi_asctime(v, &t) && t >= SANE_UTC) out->date_utc = t;
            } else if (!strcmp(k, "bands")) {
                /* "0-30000000", maybe more ranges after a comma: the first */
                char *dash;
                const long long lo = strtoll(v, &dash, 10);
                if (*dash == '-') {
                    out->bands_lo = lo;
                    out->bands_hi = strtoll(dash + 1, NULL, 10);
                }
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    out->timed = up && out->date_utc && out->uptime_s <= out->date_utc;
}

/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's). */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

bool kiwi_asctime(const char *s, uint32_t *utc)
{
    static const char MON[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char wd[4], mon[4];
    int d, hh, mm, ss, y;
    if (!s || !utc) return false;
    if (sscanf(s, "%3s %3s %d %d:%d:%d %d", wd, mon, &d, &hh, &mm, &ss, &y) != 7) return false;
    const char *p = strlen(mon) == 3 ? strstr(MON, mon) : NULL;
    if (!p || (p - MON) % 3) return false;
    const unsigned m = (unsigned)(p - MON) / 3 + 1;
    if (y < 1970 || y > 2105 || d < 1 || d > 31 || hh < 0 || hh > 23 || mm < 0 || mm > 59 ||
        ss < 0 || ss > 60)
        return false;
    const int64_t t = days_from_civil(y, m, (unsigned)d) * 86400 + hh * 3600 + mm * 60 + ss;
    if (t < 0 || t > (int64_t)UINT32_MAX) return false;
    *utc = (uint32_t)t;
    return true;
}

uint32_t kiwi_hp(const char *host, uint16_t port)
{
    uint32_t h = 2166136261u;
    for (const char *p = host ? host : ""; *p; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        h = (h ^ (uint8_t)c) * 16777619u;
    }
    char ps[8];
    snprintf(ps, sizeof ps, ":%u", (unsigned)port);
    for (const char *p = ps; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h ? h : 1;
}

/* ------------------------------------------------ addresses, and redirects */

bool kiwi_url(const char *in, uint16_t dflt, char *host, size_t cap, uint16_t *port, bool *tls)
{
    if (!in || !host || !cap) return false;
    while (*in == ' ' || *in == '\t') in++;
    bool s = false;
    uint16_t p = dflt;
    if (!strncasecmp(in, "https://", 8)) {
        s = true;
        p = 443;
        in += 8;
    } else if (!strncasecmp(in, "http://", 7)) {
        p = 80;
        in += 7;
    } else if (!dflt) {
        return false;                               /* a Location: a scheme, or a path alone */
    }
    const size_t n = strcspn(in, ":/?# \t\r\n");
    if (!n || n >= cap) return false;
    if (in[n] == ':') {
        char *e;
        const long v = strtol(in + n + 1, &e, 10);
        if (e == in + n + 1 || v < 1 || v > 65535) return false;
        p = (uint16_t)v;
    }
    memcpy(host, in, n);
    host[n] = 0;
    if (port) *port = p;
    if (tls) *tls = s;
    return true;
}

void kiwi_host_hdr(char *out, size_t cap, const char *host, uint16_t port, bool tls)
{
    if (!out || !cap) return;
    if (port == (tls ? 443 : 80)) snprintf(out, cap, "%s", host ? host : "");
    else                          snprintf(out, cap, "%s:%u", host ? host : "", (unsigned)port);
}

bool kiwi_http_header(const char *head, const char *name, char *v, size_t cap)
{
    if (!head || !name) return false;
    const size_t nl = strlen(name);
    /* Past the status line, a line at a time, to the blank one. */
    for (const char *l = strchr(head, '\n'); l && l[1] && l[1] != '\r' && l[1] != '\n'; l = strchr(l + 1, '\n')) {
        const char *s = l + 1;
        if (strncasecmp(s, name, nl) || s[nl] != ':') continue;
        s += nl + 1;
        while (*s == ' ' || *s == '\t') s++;
        size_t n = strcspn(s, "\r\n");
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
        if (v && cap) {
            if (n >= cap) n = cap - 1;
            memcpy(v, s, n);
            v[n] = 0;
        }
        return true;
    }
    return false;
}

bool kiwi_redirect_code(int status)
{
    return status == 301 || status == 302 || status == 307 || status == 308;
}

bool kiwi_redirect(const char *loc, const char *host, bool tls, uint16_t *port, char *to, size_t cap)
{
    char h[64];
    uint16_t p = 0;
    bool s = false;
    const bool url = kiwi_url(loc, 0, h, sizeof h, &p, &s);
    const bool follow = url && !tls && s && host && !strcasecmp(h, host);
    if (follow && port) *port = p;
    if (!to || !cap) return follow;
    if (url) {
        cpy(to, cap, follow ? "" : h);
        return follow;
    }
    /* No URL kiwi_url takes. None at all: nowhere said. One with a host --
     * after "//", a scheme before it or not -- too long for h, or of another
     * scheme: that host, as much of it as `to` holds. A path alone: the
     * receiver's own host. */
    while (loc && (*loc == ' ' || *loc == '\t')) loc++;
    const char *at = NULL;
    if (loc && !strncmp(loc, "//", 2)) {
        at = loc + 2;
    } else if (loc) {
        const size_t sl = strspn(loc, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-.");
        if (sl && !strncmp(loc + sl, "://", 3)) at = loc + sl + 3;
    }
    if (!loc || !*loc) cpy(to, cap, "");
    else if (at)       cpyn(to, cap, at, strcspn(at, ":/?# \t\r\n"));
    else               cpy(to, cap, host ? host : "");
    return false;
}

static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

void kiwi_unescape(char *s)
{
    if (!s) return;
    char *o = s;
    for (const char *p = s; *p; p++) {
        int a, b;
        if (*p == '%' && (a = hexv(p[1])) >= 0 && (b = hexv(p[2])) >= 0) {
            const char c = (char)(a << 4 | b);
            if (c) *o++ = c;                /* a %00 would end it early */
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}

bool kiwi_mark_cleared(const kiwi_mark_t *m, const kiwi_status_t *st)
{
    if (!m || !st || !st->ok || !st->timed) return false;
    const uint32_t boot = st->date_utc - st->uptime_s;
    /* Started since the mark: it counts from nothing again. */
    if (m->rx_boot && boot > m->rx_boot + RESTART_SLACK) return true;
    /* A KiwiSDR clears the count once a day, so a day after the last refusal
     * one such minute has passed. */
    const uint8_t kind = st->kind ? st->kind : m->kind;
    return kind == KIWI_KIND_KIWISDR && m->mark_utc && st->date_utc >= m->mark_utc + DAY_S;
}

bool kiwi_mark_lifted(const kiwi_mark_t *m, const kiwi_status_t *st)
{
    if (!m || !st || !st->ok) return false;
    /* No time limits now, where it had them: no day limit to refuse the
     * knob for. One that refused it while its /status showed no hourglass --
     * another server's, or a read cut short -- proves nothing by showing
     * none again; a restart, or a KiwiSDR's day, still does. */
    if (!st->tlimits && (m->flags & KIWI_MARK_HG)) return true;
    return kiwi_mark_cleared(m, st);
}

/* ------------------------------------------------------------- modes */

/* Dial-relative, as the Kiwi's own page opens each (openwebrx.js): its CW's
 * 300..700 is a 400 Hz passband around the 500 Hz tone, and -200..200 here. */
static const kiwi_mode_t MODES[] = {
    /* the dial's (ui.c) */
    { "usb",    300,  2700 }, { "lsb", -2700,  -300 }, { "cw",   -200,   200 },
    { "am",   -4900,  4900 }, { "sam", -4900,  4900 }, { "sal",  -4900,    0 },
    { "sau",      0,  4900 }, { "nbfm", -6000, 6000 },
    /* the narrow ones, for the API and for a radio the second receiver follows */
    { "usn",    300,  2400 }, { "lsn", -2400,  -300 }, { "cwn",    -30,    30 },
    { "amn",  -2500,  2500 }, { "nnfm", -3000, 3000 },
};

const kiwi_mode_t *kiwi_mode_find(const char *m)
{
    for (size_t i = 0; m && i < sizeof MODES / sizeof MODES[0]; i++)
        if (!strcasecmp(MODES[i].m, m)) return &MODES[i];
    return NULL;
}

const char *kiwi_mode_of(const char *any)
{
    const kiwi_mode_t *k = kiwi_mode_find(any);
    if (k) return k->m;
    if (!any || !*any) return "usb";
    if (!strncasecmp(any, "cw", 2)) return "cw";                /* cwr, cwu, cwl */
    if (!strcasecmp(any, "digl")) return "lsb";
    if (!strcasecmp(any, "fm") || !strcasecmp(any, "nfm") || !strcasecmp(any, "wfm")) return "nbfm";
    /* The Kiwi's own, which the knob never asks for: the nearest it does. */
    if (!strcasecmp(any, "amw")) return "am";
    if (!strcasecmp(any, "sas") || !strcasecmp(any, "qam")) return "sam";
    return "usb";                                               /* digu, rtty, iq ... */
}

/* IARU Region 1's bands, kHz, 160 m to 6 m. */
static const struct { int32_t lo, hi; } HAM[] = {
    { 1810, 2000 },   { 3500, 3800 },   { 5351, 5367 },   { 7000, 7200 },   { 10100, 10150 },
    { 14000, 14350 }, { 18068, 18168 }, { 21000, 21450 }, { 24890, 24990 }, { 28000, 29700 },
    { 50000, 54000 },
};

const char *kiwi_sideband(int64_t hz)
{
    const int64_t k = hz / 1000;
    for (size_t i = 0; i < sizeof HAM / sizeof HAM[0]; i++)
        if (k >= HAM[i].lo && k <= HAM[i].hi)
            return k < 10000 && !(k >= 5351 && k <= 5367) ? "lsb" : "usb";   /* 60 m: upper */
    return NULL;
}

/* Where it tunes from: its offset, Hz. */
static int64_t base_of(double offset_khz)
{
    return (int64_t)(offset_khz * 1000.0 + (offset_khz < 0 ? -0.5 : 0.5));
}

bool kiwi_tune_reaches(const kiwi_tune_t *t, double offset_khz, int64_t bw_hz)
{
    if (!t) return false;
    const int64_t base = base_of(offset_khz);
    return t->hz >= base && (bw_hz <= 0 || t->hz <= base + bw_hz);
}

int kiwi_tune_cmd(char *out, size_t cap, const kiwi_tune_t *t, double offset_khz, int64_t bw_hz,
                  double rate, int32_t cw)
{
    if (!out || !cap || !t) return -1;
    const char *m = kiwi_mode_of(t->mode);
    const kiwi_mode_t *k = kiwi_mode_find(m);
    int32_t lo = t->lo, hi = t->hi;
    /* A radio's LSB passband, given the way a radio gives it: the Kiwi's is
     * below the carrier. */
    if ((!strcmp(m, "lsb") || !strcmp(m, "lsn")) && lo >= 0 && hi > lo) {
        const int32_t x = lo;
        lo = -hi;
        hi = -x;
    }
    if (lo >= hi) {
        lo = k->lo;
        hi = k->hi;
    }
    int64_t car = t->hz;
    if (!strcmp(m, "cw") || !strcmp(m, "cwn")) {
        /* The Kiwi plays CW at its passband's centre: the carrier that far
         * below the dial puts a station on the dial at that tone -- 500 Hz
         * on a KiwiSDR as it comes. An UberSDR's Kiwi input centres it on
         * the carrier (0): the carrier is the dial, the tone its own. */
        int32_t w = hi - lo;
        if (w < 50) w = 50;
        if (w > 3000) w = 3000;
        car -= cw;
        lo = cw - w / 2;
        hi = cw + w / 2;
    }
    /* Where it tunes: from its offset up its bandwidth. */
    const int64_t base = base_of(offset_khz);
    if (bw_hz > 0 && car > base + bw_hz) car = base + bw_hz;
    if (car < base) car = base;
    /* Within its audio's band, which is all it can hear at once. */
    const int32_t edge = (int32_t)((rate > 0 ? rate : 12000.0) / 2) - 1;
    if (lo < -edge) lo = -edge;
    if (hi > edge) hi = edge;
    if (lo >= hi) {                             /* nothing left of it: the mode's own */
        lo = k->lo > -edge ? k->lo : -edge;
        hi = k->hi < edge ? k->hi : edge;
    }
    const int n = snprintf(out, cap, "SET mod=%s low_cut=%ld high_cut=%ld freq=%.3f", m, (long)lo,
                           (long)hi, (double)car / 1000.0 - offset_khz);
    return n > 0 && (size_t)n < cap ? n : -1;
}

int kiwi_agc_cmd(char *out, size_t cap, uint8_t agc, bool cw)
{
    /* The Kiwi's own default decay is 1000 ms: the middle one. */
    static const int DECAY[] = { 250, 1000, 3000 };
    const int n = snprintf(out, cap, "SET agc=1 hang=0 thresh=%d slope=6 decay=%d manGain=50",
                           cw ? -130 : -100, DECAY[agc <= KIWI_AGC_SLOW ? agc : KIWI_AGC_MED]);
    return n > 0 && (size_t)n < cap ? n : -1;
}

int kiwi_squelch_cmd(char *out, size_t cap, uint8_t pct, bool nbfm)
{
    if (pct > 100) pct = 100;
    /* NBFM's is the Kiwi's 0-99 scale with no tail; the others' dB above
     * the median noise, with half a second's tail. */
    const int n = nbfm ? snprintf(out, cap, "SET squelch=%d param=0.00", (pct * 99 + 50) / 100)
                       : snprintf(out, cap, "SET squelch=%d param=0.50", (pct * 40 + 50) / 100);
    return n > 0 && (size_t)n < cap ? n : -1;
}

bool kiwi_nr_cmd(char *out, size_t cap, uint8_t nr, int i)
{
    /* Each filter's parameters for its two types, as noise_filter.js sets
     * them; then type 0 on and type 1 off. */
    static const char *const P[3][2][4] = {
        { { "64", "16", "0.000080", "0.125" }, { "64", "16", "0.000080", "0.125" } },   /* WDSP */
        { { "1", "0.05", "0.98", "0" },        { "48", "0.125", "0.99915", "0" } },     /* LMS */
        { { "1.0", "0.95", "1000", "0" },      { "1.0", "0.95", "1000", "0" } },        /* SPEC */
    };
    if (!out || !cap || nr > KIWI_NR_SPEC || i < 0) return false;
    int n;
    if (i == 0)                  n = snprintf(out, cap, "SET nr algo=%u", (unsigned)nr);
    else if (nr == KIWI_NR_OFF)  return false;
    else if (i <= 8)             n = snprintf(out, cap, "SET nr type=%d param=%d pval=%s", (i - 1) / 4,
                                              (i - 1) % 4, P[nr - 1][(i - 1) / 4][(i - 1) % 4]);
    else if (i <= 10)            n = snprintf(out, cap, "SET nr type=%d en=%d", i - 9, i == 9);
    else                         return false;
    return n > 0 && (size_t)n < cap;
}

int kiwi_ident_cmd(char *out, size_t cap, const char *who)
{
    static const char HEX[] = "0123456789ABCDEF";
    if (!out || !cap) return -1;
    if (!who || !*who) who = KIWI_IDENT_DEFAULT;
    int o = snprintf(out, cap, "SET ident_user=");
    if (o < 0 || (size_t)o >= cap) return -1;
    /* encodeURIComponent's: letters, digits and -_.!~*'() as they are, every
     * other byte %XX -- a space, and any UTF-8, so the receiver decodes it
     * whole (its rx_cmd.cpp, kiwi_str_decode_inplace). */
    for (const unsigned char *p = (const unsigned char *)who; *p; p++) {
        const bool keep = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                          strchr("-_.!~*'()", *p);
        if ((size_t)o + (keep ? 1 : 3) >= cap) return -1;
        if (keep) {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = HEX[*p >> 4];
            out[o++] = HEX[*p & 15];
        }
    }
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------------- what it says */

bool kiwi_msg_val(const char *msg, const char *key, char *v, size_t cap)
{
    if (!msg || !key || !*key) return false;
    const size_t kl = strlen(key);
    for (const char *p = msg; (p = strstr(p, key)); p += kl) {
        const char c = p[kl];
        if ((p != msg && p[-1] != ' ') || (c != '=' && c != ' ' && c != 0)) continue;
        const char *s = p + kl + (c == '=');
        size_t n = c == '=' ? strcspn(s, " ") : 0;
        if (v && cap) {
            if (n >= cap) n = cap - 1;
            memcpy(v, s, n);
            v[n] = 0;
        }
        return true;
    }
    return false;
}

kiwi_end_t kiwi_http(int status)
{
    if (status == 101) return KIWI_END_NONE;
    if (status <= 0) return KIWI_END_NO_ANSWER;
    if (status == 401 || status == 403) return KIWI_END_REFUSED;
    if (status == 429 || status == 503) return KIWI_END_FULL;
    if (status >= 500) return KIWI_END_DOWN;
    if (kiwi_redirect_code(status) || status == 303) return KIWI_END_MOVED;
    return KIWI_END_NOT_KIWI;
}

void kiwi_said_init(kiwi_said_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->rx_chans = s->chan_no_pwd = s->badp = s->too_busy = s->version_maj = s->version_min = -1;
    s->cw_hz = KIWI_CW_PITCH;
}

kiwi_end_t kiwi_said(kiwi_said_t *s, const char *msg, bool pass_given)
{
    char v[96];
    if (!s || !msg) return KIWI_END_NONE;
    s->said_anything = true;
    /* What it says of itself, in whichever MSG it comes. */
    if (kiwi_msg_val(msg, "rx_chans", v, sizeof v))    s->rx_chans = atoi(v);
    if (kiwi_msg_val(msg, "chan_no_pwd", v, sizeof v)) s->chan_no_pwd = atoi(v);
    if (kiwi_msg_val(msg, "version_maj", v, sizeof v)) s->version_maj = atoi(v);
    if (kiwi_msg_val(msg, "version_min", v, sizeof v)) s->version_min = atoi(v);
    if (kiwi_msg_val(msg, "freq_offset", v, sizeof v)) s->offset_khz = strtod(v, NULL);
    if (kiwi_msg_val(msg, "center_freq", v, sizeof v)) s->center_hz = strtoll(v, NULL, 10);
    if (kiwi_msg_val(msg, "bandwidth", v, sizeof v))   s->bw_hz = strtoll(v, NULL, 10);
    if (kiwi_msg_val(msg, "audio_rate", v, sizeof v))  s->audio_rate = atoi(v);
    if (kiwi_msg_val(msg, "sample_rate", v, sizeof v)) s->rate = strtod(v, NULL);

    /* The day's listening time used up. Said for the login, before it took,
     * it is a refusal the receiver counts, and its fifth bars the address. */
    if (kiwi_msg_val(msg, "ip_limit", v, sizeof v)) {
        s->limit_at_login = !s->logged_in;
        return KIWI_END_DAY_LIMIT;
    }
    if (kiwi_msg_val(msg, "inactivity_timeout", v, sizeof v)) return KIWI_END_IDLE;
    /* KiwiSDR says "n,message", the Web-888 "message", URL-encoded. */
    if (kiwi_msg_val(msg, "kiwi_kick", v, sizeof v)) {
        kiwi_unescape(v);
        const char *t = v;
        while (*t >= '0' && *t <= '9') t++;
        cpy(s->kick, sizeof s->kick, *t == ',' && t > v ? t + 1 : v);
        return KIWI_END_KICKED;
    }
    /* No channel for the knob: none at all before the login; after it, the
     * Web-888's check 10 s in says how many it lets apps have -- none, or
     * fewer than it has, all taken. */
    if (kiwi_msg_val(msg, "too_busy", v, sizeof v)) {
        s->too_busy = atoi(v);
        if (!s->logged_in) return KIWI_END_FULL;
        if (s->too_busy == 0) return KIWI_END_NO_APPS;
        return s->rx_chans > 0 && s->too_busy < s->rx_chans ? KIWI_END_APPS_FULL : KIWI_END_FULL;
    }
    if (kiwi_msg_val(msg, "redirect", NULL, 0) || kiwi_msg_val(msg, "exclusive_use", NULL, 0))
        return KIWI_END_FULL;
    if (kiwi_msg_val(msg, "down", NULL, 0)) return KIWI_END_DOWN;
    if (kiwi_msg_val(msg, "badp", v, sizeof v)) {
        s->badp = atoi(v);
        switch (s->badp) {
        case 0:  s->logged_in = true; return KIWI_END_NONE;
        /* No password given, and channels that need none: all of those
         * are taken, not the password wrong. */
        case 1:  return pass_given || s->chan_no_pwd <= 0 ? KIWI_END_PASSWORD : KIWI_END_PWD_FULL;
        case 3:  return KIWI_END_REFUSED;
        case 5:  return KIWI_END_DUP_IP;
        case 6:  return KIWI_END_UPDATING;
        default: return KIWI_END_TRY_LATER;     /* 2, 4, 7: its address, its admin: not now */
        }
    }
    return KIWI_END_NONE;
}

kiwi_end_t kiwi_quiet(const kiwi_said_t *s, uint8_t kind, int ext_api, bool closed)
{
    if (s && s->logged_in) return closed ? KIWI_END_CLOSED : KIWI_END_QUIET;
    if (closed || (s && s->said_anything) || kind == KIWI_KIND_WEB888) return KIWI_END_NO_ANSWER;
    /* KiwiSDR 1.9 turns an app away by saying nothing and keeping the
     * socket open: its app channels taken, or none allowed. */
    return ext_api == 0 ? KIWI_END_NO_APPS : KIWI_END_SILENT;
}

/* ------------------------------------------------- where it centres CW */

/* kiwi_cw_t's st: the frame's prefix, then the key, then the object -- read
 * to its '}', or to obj's end -- or no load_cfg at all. */
enum { CW_PRE, CW_KEY, CW_OBJ, CW_WHOLE, CW_FULL, CW_NONE };

void kiwi_cw_init(kiwi_cw_t *c)
{
    if (c) memset(c, 0, sizeof *c);
}

/* One character of the JSON, its %XX undone: spaces left out, the key looked
 * for, then the object's text kept up to its '}'. */
static void cw_char(kiwi_cw_t *c, int ch)
{
    static const char KEY[] = "\"cw\":{";
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') return;
    if (c->st == CW_KEY) {
        c->at = ch == KEY[c->at] ? c->at + 1 : ch == KEY[0] ? 1 : 0;
        if (c->at == sizeof KEY - 1) c->st = CW_OBJ;
    } else if (c->st == CW_OBJ) {
        if (ch == '}') {
            c->st = CW_WHOLE;
        } else {
            c->obj[c->n++] = (char)ch;
            if (c->n == sizeof c->obj - 1) c->st = CW_FULL;    /* longer than any it has: what came */
        }
    }
}

void kiwi_cw_feed(kiwi_cw_t *c, const uint8_t *p, size_t n)
{
    static const char PRE[] = "MSG load_cfg=";
    for (size_t i = 0; c && p && i < n && c->st <= CW_OBJ; i++) {
        const int ch = p[i], h = hexv((char)ch);
        if (c->st == CW_PRE) {
            if (ch != PRE[c->at]) {
                c->st = CW_NONE;
            } else if (++c->at == sizeof PRE - 1) {
                c->st = CW_KEY;
                c->at = 0;
            }
        } else if (c->esc == 2 && h >= 0) {             /* %XX whole: the byte it stands for */
            c->esc = 0;
            cw_char(c, hexv(c->dig) << 4 | h);
        } else if (c->esc == 1 && h >= 0) {
            c->esc = 2;
            c->dig = (char)ch;
        } else {
            /* A '%' without two hex digits after it is itself, and so is
             * what came after it. */
            if (c->esc) cw_char(c, '%');
            if (c->esc == 2) cw_char(c, c->dig);
            c->esc = ch == '%';
            if (!c->esc) cw_char(c, ch);
        }
    }
}

int32_t kiwi_cw_centre(const kiwi_cw_t *c, int32_t dflt)
{
    if (!c || (c->st != CW_WHOLE && c->st != CW_FULL)) return dflt;
    char o[sizeof c->obj];
    memcpy(o, c->obj, c->n);
    o[c->n] = 0;
    const char *l = strstr(o, "\"lo\":"), *h = strstr(o, "\"hi\":");
    if (!l || !h) return dflt;
    char *le, *he;
    const long lo = strtol(l + 5, &le, 10), hi = strtol(h + 5, &he, 10);
    /* Numbers, and whole: the one the text kept ends in may have been cut. */
    if (le == l + 5 || he == h + 5 || (c->st == CW_FULL && (!*le || !*he))) return dflt;
    if (lo >= hi || lo < -100000 || hi > 100000) return dflt;
    const long ctr = (lo + hi) / 2;
    return ctr >= -1000 && ctr <= 1500 ? (int32_t)ctr : dflt;
}

/* The face's 28 pt words, all within the warning panel's 264 px. */
static const char *const WORD[] = {
    [KIWI_END_NONE] = "",              [KIWI_END_WANT] = "",
    [KIWI_END_NOT_FOUND] = "NOT FOUND", [KIWI_END_NO_ROUTE] = "NO ROUTE",
    [KIWI_END_NO_SOCKET] = "NO SOCKET", [KIWI_END_NO_ANSWER] = "NO ANSWER",
    [KIWI_END_SILENT] = "APPS FULL",   [KIWI_END_CLOSED] = "NO ANSWER",
    [KIWI_END_QUIET] = "NO ANSWER",    [KIWI_END_PROTOCOL] = "NO ANSWER",
    [KIWI_END_REFUSED] = "REFUSED",    [KIWI_END_NOT_KIWI] = "NOT A KIWI",
    [KIWI_END_PASSWORD] = "PASSWORD?", [KIWI_END_PWD_FULL] = "RECEIVER FULL",
    [KIWI_END_DUP_IP] = "ONE PER IP",  [KIWI_END_UPDATING] = "UPDATING",
    [KIWI_END_TRY_LATER] = "TRY LATER", [KIWI_END_FULL] = "RECEIVER FULL",
    [KIWI_END_APPS_FULL] = "APPS FULL", [KIWI_END_NO_APPS] = "NO APPS",
    [KIWI_END_DAY_LIMIT] = "DAY LIMIT", [KIWI_END_IDLE] = "TIME UP",
    [KIWI_END_KICKED] = "KICKED",      [KIWI_END_DOWN] = "DOWN",
    [KIWI_END_NO_FLASH] = "MEMORY FULL", [KIWI_END_MOVED] = "MOVED",
    [KIWI_END_CERT] = "CERTIFICATE?",
};

/* ...and the second receiver's, beside the radio's S-meter: 11 at most. */
static const char *const NOTE[] = {
    [KIWI_END_NONE] = "",              [KIWI_END_WANT] = "",
    [KIWI_END_NOT_FOUND] = "not found", [KIWI_END_NO_ROUTE] = "no route",
    [KIWI_END_NO_SOCKET] = "no socket", [KIWI_END_NO_ANSWER] = "no answer",
    [KIWI_END_SILENT] = "apps full",   [KIWI_END_CLOSED] = "no answer",
    [KIWI_END_QUIET] = "no answer",    [KIWI_END_PROTOCOL] = "no answer",
    [KIWI_END_REFUSED] = "refused",    [KIWI_END_NOT_KIWI] = "not a kiwi",
    [KIWI_END_PASSWORD] = "password?", [KIWI_END_PWD_FULL] = "full",
    [KIWI_END_DUP_IP] = "one per ip",  [KIWI_END_UPDATING] = "updating",
    [KIWI_END_TRY_LATER] = "try later", [KIWI_END_FULL] = "full",
    [KIWI_END_APPS_FULL] = "apps full", [KIWI_END_NO_APPS] = "no apps",
    [KIWI_END_DAY_LIMIT] = "day limit", [KIWI_END_IDLE] = "time up",
    [KIWI_END_KICKED] = "kicked",      [KIWI_END_DOWN] = "down",
    [KIWI_END_NO_FLASH] = "memory full", [KIWI_END_MOVED] = "moved",
    [KIWI_END_CERT] = "certificate",
};

const char *kiwi_end_word(kiwi_end_t e)
{
    return (unsigned)e < sizeof WORD / sizeof WORD[0] && WORD[e] ? WORD[e] : "NO ANSWER";
}

const char *kiwi_end_note(kiwi_end_t e)
{
    return (unsigned)e < sizeof NOTE / sizeof NOTE[0] && NOTE[e] ? NOTE[e] : "no answer";
}

/* ------------------------------------------------------------- its audio */

bool kiwi_snd(const uint8_t *p, size_t n, kiwi_snd_t *out)
{
    if (!p || !out || n < 10 || memcmp(p, "SND", 3)) return false;
    out->flags  = p[3];
    out->seq    = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
    out->smeter = (uint16_t)(p[8] << 8 | p[9]);
    out->off    = out->flags & KIWI_SND_STEREO ? 20 : 10;
    return n >= out->off;
}

float kiwi_dbm(uint16_t raw, bool web888)
{
    return 0.1f * raw - (web888 ? 140.0f : 127.0f);
}

static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
    60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767 };
static const int8_t IDX[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

/* The filters: windowed sinc, a Kaiser window. With 31 taps, beta 3.6 holds
 * a half-band flat within 0.07 dB to 5 kHz at 12 kHz in, and 42 dB down from
 * 7 kHz -- a Blackman window's wider skirt is 0.8 dB down at 5 kHz and only
 * 21 dB down at 7. */
#define KAISER_BETA 3.6
#define HB_TOP      32000.0             /* half-band stages until the rate is this or more */
#define LP_FROM     26000.0             /* ...or, above this, the low-pass */
#define LP_EDGE     10500.0
#define PI          3.14159265358979323846

static double bessel_i0(double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 40 && t > 1e-12 * s; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}

/* A low-pass of n taps (odd), its edge `fc` a fraction of the rate, unity
 * gain at DC: the taps into h. */
static void lowpass(double *h, int n, double fc)
{
    const int c = n / 2;
    double sum = 0;
    for (int k = 0; k < n; k++) {
        const int m = k - c;
        const double r = (double)m / c;
        const double s = m ? sin(2 * PI * fc * m) / (PI * m) : 2 * fc;
        h[k] = s * bessel_i0(KAISER_BETA * sqrt(1 - r * r)) / bessel_i0(KAISER_BETA);
        sum += h[k];
    }
    for (int k = 0; k < n; k++) h[k] /= sum;
}

void kiwi_dsp_init(kiwi_dsp_t *d)
{
    memset(d, 0, sizeof *d);
    d->trim = 1.0f;
    /* The half-band's odd phase: the taps an odd distance from its centre.
     * They sum to a half, as the even phase's one tap is, so each phase
     * passes DC alike and nothing of it lands at the top of the band. */
    double h[31], odd = 0;
    lowpass(h, 31, 0.25);
    for (int j = 0; j < KIWI_HB_ODD; j++) odd += h[2 * j];
    for (int j = 0; j < KIWI_HB_ODD; j++) d->hb_h[j] = (float)(h[2 * j] * 0.5 / odd);
    kiwi_dsp_reset(d, 12000.0);
}

#define Q32 4294967296.0                /* 1.0 in the resampler's 32.32 */
#define ONE (1ULL << 32)

bool kiwi_dsp_reset(kiwi_dsp_t *d, double rate)
{
    if (!(rate >= 4000.0 && rate <= 50000.0)) return false;      /* a NaN too */
    d->rate = (float)rate;
    d->lp = rate > LP_FROM;
    double at = rate;
    d->hb = 0;
    while (!d->lp && at < HB_TOP && d->hb < KIWI_HB_MAX) {
        at *= 2;
        d->hb++;
    }
    if (d->lp) {
        double h[KIWI_LP_LEN];
        lowpass(h, KIWI_LP_LEN, LP_EDGE / rate);
        for (int k = 0; k < KIWI_LP_LEN; k++) d->lp_h[k] = (float)h[k];
    }
    memset(d->hb_x, 0, sizeof d->hb_x);
    memset(d->hb_at, 0, sizeof d->hb_at);
    memset(d->lp_x, 0, sizeof d->lp_x);
    memset(d->y, 0, sizeof d->y);
    d->lp_at = 0;
    d->step = (uint64_t)(at / KIWI_OUT_HZ * Q32 + 0.5);
    d->pos  = 0;
    return true;
}

int kiwi_adpcm(kiwi_dsp_t *d, const uint8_t *in, size_t n, int16_t *out)
{
    int o = 0;
    for (size_t i = 0; i < n; i++) {
        for (int half = 0; half < 2; half++) {
            const uint8_t code = half ? in[i] >> 4 : in[i] & 0x0F;
            const int step = STEP[d->idx];
            int diff = step >> 3;
            if (code & 4) diff += step;
            if (code & 2) diff += step >> 1;
            if (code & 1) diff += step >> 2;
            d->pred += code & 8 ? -diff : diff;
            if (d->pred > 32767) d->pred = 32767;
            if (d->pred < -32768) d->pred = -32768;
            d->idx += IDX[code];
            if (d->idx < 0) d->idx = 0;
            if (d->idx > 88) d->idx = 88;
            out[o++] = (int16_t)d->pred;
        }
    }
    return o;
}

int kiwi_pcm16(const uint8_t *in, size_t n, bool le, int16_t *out, int cap)
{
    int o = 0;
    for (size_t i = 0; i + 1 < n && o < cap; i += 2)
        out[o++] = (int16_t)(le ? in[i] | in[i + 1] << 8 : in[i] << 8 | in[i + 1]);
    return o;
}

static int16_t sat16(float v)
{
    v = v < 0 ? v - 0.5f : v + 0.5f;
    return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)v;
}

/* One sample into the interpolator, the outputs that fall before it out. It
 * runs a sample behind: it interpolates between y[1] and y[2], with y[0] and
 * y[3] either side. The whole samples above the point, the fraction below
 * it: the step adds up exactly. */
static int interp(kiwi_dsp_t *d, float x, uint64_t step, int16_t *out, int o, int cap)
{
    float *y = d->y;
    y[0] = y[1];
    y[1] = y[2];
    y[2] = y[3];
    y[3] = x;
    while (d->pos < ONE) {
        if (o < cap) {                  /* never more, sized right: the rest let go */
            const float t = (float)(uint32_t)d->pos * (float)(1.0 / Q32);
            out[o++] = sat16(y[1] + 0.5f * t * (y[2] - y[0] + t * (2 * y[0] - 5 * y[1] + 4 * y[2] - y[3] +
                                                                  t * (3 * (y[1] - y[2]) + y[3] - y[0]))));
        }
        d->pos += step;
    }
    d->pos -= ONE;
    return o;
}

/* One sample through half-band stage s and those after it: two out at twice
 * the rate -- the input itself, eight samples late, then the one halfway to
 * the next, from the odd phase. Each history is written twice, n apart, so
 * the newest n are always in a row. */
static int up(kiwi_dsp_t *d, int s, float x, uint64_t step, int16_t *out, int o, int cap)
{
    if (s >= d->hb) return interp(d, x, step, out, o, cap);
    float *r = d->hb_x[s];
    const int at = d->hb_at[s] = (uint8_t)((d->hb_at[s] + 1) % KIWI_HB_ODD);
    r[at] = r[at + KIWI_HB_ODD] = x;
    const float *w = r + at + 1;        /* the last 16, oldest first */
    float mid = 0;
    for (int j = 0; j < KIWI_HB_ODD; j++) mid += d->hb_h[j] * w[j];
    o = up(d, s + 1, w[KIWI_HB_ODD / 2 - 1], step, out, o, cap);
    return up(d, s + 1, 2 * mid, step, out, o, cap);
}

int kiwi_resample(kiwi_dsp_t *d, const int16_t *x, int n, int16_t *out, int cap)
{
    if (n <= 0) return 0;
    const uint64_t step = d->trim == 1.0f ? d->step : (uint64_t)((double)d->step * d->trim + 0.5);
    int o = 0;
    for (int i = 0; i < n; i++) {
        float v = x[i];
        if (d->lp) {
            /* Over 26 kHz: the band above 10.5 kHz away first, as the
             * interpolator takes fewer samples out than come in. */
            const int at = d->lp_at = (uint8_t)((d->lp_at + 1) % KIWI_LP_LEN);
            d->lp_x[at] = d->lp_x[at + KIWI_LP_LEN] = v;
            const float *w = d->lp_x + at + 1;
            v = 0;
            for (int k = 0; k < KIWI_LP_LEN; k++) v += d->lp_h[k] * w[k];
        }
        o = up(d, 0, v, step, out, o, cap);
    }
    return o;
}

void kiwi_dsp_trim(kiwi_dsp_t *d, float ratio)
{
    if (!(ratio >= 0.998f)) ratio = 0.998f;
    if (ratio > 1.002f) ratio = 1.002f;
    d->trim = ratio;
}

size_t kiwi_snd_len(const kiwi_snd_t *f, size_t n)
{
    if (!f || f->off > n || (f->flags & KIWI_SND_STEREO)) return 0;
    return f->flags & KIWI_SND_ADPCM ? 2 * (n - f->off) : (n - f->off) / 2;
}

/* Input per pass of the resampler: at the lowest rate a Kiwi has (4 kHz)
 * that is 6144 samples out, within KIWI_OUT_MAX. */
#define RS_PIECE 1024

int kiwi_snd_audio(kiwi_dsp_t *d, const kiwi_snd_t *f, const uint8_t *p, size_t n, bool play,
                   int16_t *pcm, int16_t *out, kiwi_emit_t emit, void *ctx)
{
    if (!d || !f || !p || !pcm || !out || f->off > n || (f->flags & KIWI_SND_STEREO)) return 0;
    const bool adpcm = f->flags & KIWI_SND_ADPCM, sq = f->flags & KIWI_SND_SQUELCH;
    const uint8_t *in = p + f->off;
    size_t left = n - f->off;
    int total = 0;
    /* As much at a time as `pcm` holds: a frame longer than any Kiwi sends
     * is still decoded whole, never skipped -- that would part the decoder
     * from the encoder for the rest of the session. */
    while (left) {
        const size_t most = adpcm ? KIWI_PCM_MAX / 2 : KIWI_PCM_MAX * 2;
        const size_t take = left < most ? left : most;
        const int m = adpcm ? kiwi_adpcm(d, in, take, pcm)
                            : kiwi_pcm16(in, take, f->flags & KIWI_SND_LE, pcm, KIWI_PCM_MAX);
        in += take;
        left -= take;
        total += m;
        for (int at = 0; play && at < m; at += RS_PIECE) {
            const int k = m - at < RS_PIECE ? m - at : RS_PIECE;
            const int o = kiwi_resample(d, pcm + at, k, out, KIWI_OUT_MAX);
            if (o <= 0) continue;
            /* Squelched: silence in its place, so the ring keeps its time. */
            if (sq) memset(out, 0, (size_t)o * sizeof out[0]);
            if (emit) emit(ctx, out, (size_t)o);
        }
    }
    return total;
}

/* ------------------------------------------------------------- its name */

/* What a receiver says of itself -- its antenna, its name -- as the dial
 * shows it: no spaces at either end, and less the "RF.Guru " in front of the
 * Lombardsijde ones' -- unless that is all there is. Its start, and its
 * length in *n: 0 when it says nothing but spaces. */
static const char *bare(const char *s, size_t *n)
{
    while (*s == ' ') s++;
    if (!strncasecmp(s, "RF.Guru ", 8)) {
        const char *t = s + 8;
        while (*t == ' ') t++;
        if (*t) s = t;
    }
    size_t k = strlen(s);
    while (k && s[k - 1] == ' ') k--;
    *n = k;
    return s;
}

void kiwi_label(char *out, size_t cap, const char *name, const char *antenna, const char *host,
                uint16_t port, bool shared)
{
    if (!out || !cap) return;
    if (name && *name) {
        cpy(out, cap, name);
        return;
    }
    size_t n = 0;
    const char *a = antenna ? bare(antenna, &n) : "";
    if (n) {
        /* The Lombardsijde receivers all say "RF.Guru <antenna>". */
        cpyn(out, cap, a, n);
        return;
    }
    if (!host) host = "";
    char hp[80];
    if (port) snprintf(hp, sizeof hp, "%s:%u", host, (unsigned)port);
    else      snprintf(hp, sizeof hp, "%s", host);
    n = strlen(hp);
    if (n < cap) {
        cpy(out, cap, hp);
        return;
    }
    /* Too long. The start of its host tells it from the others -- unless
     * another receiver has the same host: then its end, the port with it. */
    if (!shared || cap < 4) {
        cpy(out, cap, shared ? hp + n - (cap - 1) : host);
        return;
    }
    const char *end = hp + n - (cap - 3);
    if (*end == '.') end++;                 /* behind two dots, never three */
    snprintf(out, cap, "..%s", end);
}

void kiwi_status_name(char *out, size_t cap, const kiwi_status_t *st)
{
    if (!out || !cap) return;
    out[0] = 0;
    if (!st || !st->ok) return;
    size_t n;
    const char *s = bare(st->antenna, &n);
    if (!n) {
        /* Its own name, its first part: "RF.Guru Lombardsijde | EchoTracer"
         * is "Lombardsijde". */
        s = bare(st->name, &n);
        const char *bar = strstr(s, " | ");
        if (bar && (size_t)(bar - s) < n) n = (size_t)(bar - s);
        while (n && s[n - 1] == ' ') n--;
    }
    cpyn(out, cap, s, n);
}
