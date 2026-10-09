/* A WebSDR's protocol, the plain-C parts besides the audio. See wsdr_proto.h
 * and WEBSDR-PROTOCOL.md §2, §4, §6. */
#include "wsdr_proto.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ the bands */

/* bandinfo.js as tokens: names and numbers, quoted strings, and the
 * punctuation between. What is kept:
 *   var nbands=8; var ini_freq=3630.0; var ini_mode='lsb'; var idletimeout=0;
 *   var bandinfo= [ { centerfreq: ..., samplerate: ..., name: '80m', ... }, ... ];
 *   freqbands.push( { min:3500.0, max:3800.0 } )
 * -- the top level's assignments, and each record's fields two brackets in,
 * whatever lies deeper (the scale images' names) passed by. */
enum { T_IDLE, T_WORD, T_STR, T_ESC, T_SLASH, T_LINE, T_BLOCK, T_BLOCK_STAR };
enum { C_NONE, C_BANDS, C_PLAN };
enum { H_CENTER = 1, H_SPAN = 2 };
#define REC_DEPTH 2

void wsdr_info_begin(wsdr_info_rd_t *r, wsdr_info_t *out)
{
    memset(r, 0, sizeof *r);
    memset(out, 0, sizeof *out);
    out->ini_hz = -1;
    r->out = out;
}

/* kHz as the file writes them, in Hz -- within +-1 THz, whatever it says
 * ("-1e300", "inf", "nan"), so no sum or difference of two overflows. */
#define HZ_MAX 1e12
static int64_t khz(const char *v)
{
    const double hz = strtod(v, NULL) * 1000.0;
    return hz != hz ? 0 : llround(hz > HZ_MAX ? HZ_MAX : hz < -HZ_MAX ? -HZ_MAX : hz);
}

/* Kept to `cap`, cut short past it: no key looked for is that long, so a
 * cut one never passes for one. */
static void put(char *d, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(d, s, n);
    d[n] = 0;
}

static void top_value(wsdr_info_rd_t *r, const char *v, bool str)
{
    wsdr_info_t *o = r->out;
    if (!strcmp(r->key, "nbands") && !str) o->nbands = atoi(v);
    else if (!strcmp(r->key, "ini_freq") && !str) o->ini_hz = strtod(v, NULL) > 0 ? khz(v) : -1;
    else if (!strcmp(r->key, "ini_mode") && str) put(o->ini_mode, sizeof o->ini_mode, v);
    else if (!strcmp(r->key, "idletimeout") && !str) {
        const double ms = strtod(v, NULL);
        o->idle_ms = ms > 0 && ms < 4e9 ? (uint32_t)ms : 0;
    }
}

static void rec_value(wsdr_info_rd_t *r, const char *v, bool str)
{
    if (r->ctx == C_PLAN) {
        if (!strcmp(r->key, "min") && !str) r->rng.lo_hz = khz(v), r->have |= H_CENTER;
        else if (!strcmp(r->key, "max") && !str) r->rng.hi_hz = khz(v), r->have |= H_SPAN;
        return;
    }
    wsdr_band_t *b = &r->cur;
    if (str) {
        if (!strcmp(r->key, "name")) put(b->name, sizeof b->name, v);
        return;
    }
    if (!strcmp(r->key, "centerfreq")) b->center_hz = khz(v), r->have |= H_CENTER;
    else if (!strcmp(r->key, "samplerate")) b->span_hz = khz(v), r->have |= H_SPAN;
    else if (!strcmp(r->key, "vfo")) b->vfo_hz = khz(v);
    else if (!strcmp(r->key, "tuningstep")) b->step_hz = strtod(v, NULL) * 1000.0;
    else if (!strcmp(r->key, "maxlinbw")) {
        const int64_t bw = khz(v);
        b->maxbw_hz = bw > 0 && bw <= 1000000 ? (int32_t)bw : 0;
    }
}

/* A record's end: kept where it has what it must. */
static void rec_end(wsdr_info_rd_t *r)
{
    wsdr_info_t *o = r->out;
    if (r->ctx == C_BANDS && (r->have & (H_CENTER | H_SPAN)) == (H_CENTER | H_SPAN) && r->cur.span_hz > 0 &&
        o->n_bands < WSDR_BANDS) {
        wsdr_band_t *b = &o->band[o->n_bands++];
        *b = r->cur;
        if (b->vfo_hz < b->center_hz - b->span_hz / 2 || b->vfo_hz > b->center_hz + b->span_hz / 2)
            b->vfo_hz = b->center_hz;
        if (!(b->step_hz > 0 && b->step_hz <= 1e6)) b->step_hz = 1;    /* NaN, inf too */
        if (b->maxbw_hz <= 0 || b->maxbw_hz > 1000000) b->maxbw_hz = 4000;
    } else if (r->ctx == C_PLAN && r->have == (H_CENTER | H_SPAN) && r->rng.hi_hz > r->rng.lo_hz &&
               o->n_plan < WSDR_PLAN) {
        o->plan[o->n_plan++] = r->rng;
    }
    memset(&r->cur, 0, sizeof r->cur);
    memset(&r->rng, 0, sizeof r->rng);
    r->have = 0;
}

/* `t` is `name`, or one of its members: "freqbands.push" is freqbands'. */
static bool named(const char *t, const char *name)
{
    const size_t n = strlen(name);
    return !strncmp(t, name, n) && (t[n] == 0 || t[n] == '.');
}

/* A token, whole: a word (name or number), a string, or a punctuation mark
 * (tok[0], tn 1, `punct`). */
static void token(wsdr_info_rd_t *r, bool str, bool punct)
{
    const char *t = r->tok;
    if (punct) {
        switch (t[0]) {
        case '[': case '{': case '(':
            r->want_val = false;        /* "x = [": not a value kept */
            if (r->depth < 255) r->depth++;
            if (r->depth == REC_DEPTH && t[0] == '{') {
                r->in_rec = 1;
                r->have = 0;
            }
            break;
        case ']': case '}': case ')':
            if (r->depth == REC_DEPTH && t[0] == '}' && r->in_rec) {
                rec_end(r);
                r->in_rec = 0;
            }
            if (r->depth) r->depth--;
            break;
        case '=':
            if (!r->depth) r->want_val = true;
            break;
        case ':':
            if (r->depth == REC_DEPTH) r->want_val = true;
            break;
        case ';': case ',':
            r->want_val = false;
            break;
        }
        return;
    }
    if (r->want_val) {
        r->want_val = false;
        if (!r->depth) top_value(r, t, str);
        else if (r->depth == REC_DEPTH && r->in_rec) rec_value(r, t, str);
        return;
    }
    if (str) return;
    if (!r->depth) {
        if (!strcmp(t, "var")) return;
        put(r->key, sizeof r->key, t);
        r->ctx = named(t, "bandinfo") ? C_BANDS : named(t, "freqbands") ? C_PLAN : C_NONE;
    } else if (r->depth == REC_DEPTH) {
        put(r->key, sizeof r->key, t);
    }
}

static bool word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' ||
           c == '.' || c == '-' || c == '+';
}

static void take(wsdr_info_rd_t *r, char c)
{
    if (r->tn < sizeof r->tok - 1) r->tok[r->tn++] = c;
}

static void word_end(wsdr_info_rd_t *r, bool str)
{
    r->tok[r->tn] = 0;
    token(r, str, false);
    r->tn = 0;
}

void wsdr_info_feed(wsdr_info_rd_t *r, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const char c = (char)p[i];
        switch (r->st) {
        case T_STR:
            if (c == '\\') r->st = T_ESC;
            else if ((uint8_t)c == r->quote) {
                word_end(r, true);
                r->st = T_IDLE;
            } else take(r, c);
            continue;
        case T_ESC:
            take(r, c);
            r->st = T_STR;
            continue;
        case T_LINE:
            if (c == '\n') r->st = T_IDLE;
            continue;
        case T_BLOCK:
            if (c == '*') r->st = T_BLOCK_STAR;
            continue;
        case T_BLOCK_STAR:
            r->st = c == '/' ? T_IDLE : c == '*' ? T_BLOCK_STAR : T_BLOCK;
            continue;
        case T_SLASH:
            if (c == '/') { r->st = T_LINE; continue; }
            if (c == '*') { r->st = T_BLOCK; continue; }
            r->st = T_IDLE;             /* a lone '/': nothing the file has */
            break;
        case T_WORD:
            if (word_char(c)) { take(r, c); continue; }
            word_end(r, false);
            r->st = T_IDLE;
            break;
        }
        /* Between tokens. */
        if (word_char(c)) {
            r->st = T_WORD;
            take(r, c);
        } else if (c == '\'' || c == '"') {
            r->st = T_STR;
            r->quote = (uint8_t)c;
        } else if (c == '/') {
            r->st = T_SLASH;
        } else if (c > ' ') {
            r->tok[0] = c;
            r->tok[1] = 0;
            token(r, false, true);
        }
    }
}

bool wsdr_info_end(wsdr_info_rd_t *r)
{
    if (r->st == T_WORD) word_end(r, false);
    r->st = T_IDLE;
    /* Cut short inside a band -- Twente's closes only after 60 kB of scale
     * images' names -- what it said of itself is kept. */
    if (r->in_rec && r->depth >= REC_DEPTH) rec_end(r);
    r->in_rec = 0;
    return r->out->n_bands > 0;
}

int wsdr_band_of(const wsdr_info_t *in, int64_t hz, int prefer)
{
    for (int pass = 0; pass < 2; pass++) {
        const int64_t slack = pass ? 4000 : 0;
        if (prefer >= 0 && prefer < in->n_bands) {
            const wsdr_band_t *b = &in->band[prefer];
            if (llabs(hz - b->center_hz) <= b->span_hz / 2 + slack) return prefer;
        }
        for (int i = 0; i < in->n_bands; i++) {
            const wsdr_band_t *b = &in->band[i];
            if (llabs(hz - b->center_hz) <= b->span_hz / 2 + slack) return i;
        }
    }
    return -1;
}

/* ------------------------------------------------- which stream path */

void wsdr_v11_feed(wsdr_v11_t *s, const uint8_t *p, size_t n)
{
    static const char PAT[] = "~~stream?v=11";
    for (size_t i = 0; i < n && !s->v11; i++) {
        const char c = (char)p[i];
        /* "~~" is its only part that starts it again: "~~~stream" is found. */
        if (c == PAT[s->m]) s->m++;
        else if (c == '~') s->m = s->m >= 2 ? 2 : 1;
        else s->m = 0;
        if (PAT[s->m] == 0) s->v11 = true;
    }
}

/* ---------------------------------------------------------- the tuning */

/* Twente's page's passbands (§2.3); CW below the carrier, its tone 750 Hz. */
static const wsdr_mode_t MODES[] = {
    { "usb", WSDR_M_SSB,     300,  2700 },
    { "lsb", WSDR_M_SSB,   -2700,  -300 },
    { "cw",  WSDR_M_SSB,    -950,  -550 },
    { "am",  WSDR_M_AM,    -4500,  4500 },
    { "sam", WSDR_M_AMSYNC, -4500, 4500 },
    { "nfm", WSDR_M_FM,    -5000,  5000 },
};

const wsdr_mode_t *wsdr_mode(int i)
{
    return i >= 0 && i < (int)(sizeof MODES / sizeof MODES[0]) ? &MODES[i] : NULL;
}

const wsdr_mode_t *wsdr_mode_named(const char *name)
{
    for (const wsdr_mode_t *m = MODES; m < MODES + sizeof MODES / sizeof MODES[0]; m++)
        if (name && !strcmp(m->name, name)) return m;
    return NULL;
}

void wsdr_clamp_pass(const wsdr_band_t *b, wsdr_tune_t *t)
{
    const int32_t lim = t->mode == WSDR_M_FM ? 15000 : (int32_t)(b->maxbw_hz * 0.95);
    if (t->lo < -lim) t->lo = -lim;
    if (t->hi > lim) t->hi = lim;
    if (t->lo > lim) t->lo = lim;
    if (t->hi < -lim) t->hi = -lim;
    if (t->lo > t->hi) {
        const int32_t m = t->lo;
        t->lo = t->hi;
        t->hi = m;
    }
}

int64_t wsdr_carrier_hz(const wsdr_band_t *b, const wsdr_tune_t *t)
{
    const double hz = (double)t->dial_hz + (t->cw ? WSDR_CW_TONE : 0);
    const double st = b->step_hz > 0 ? b->step_hz : 1;
    return llround(round(hz / st) * st);
}

/* kHz as the page writes a passband edge: no more digits than it has, no
 * exponent -- "0.3", "-2.7", "4.5". */
static void khz_text(char *out, size_t cap, int32_t hz)
{
    snprintf(out, cap, "%s%d.%03d", hz < 0 ? "-" : "", abs(hz) / 1000, abs(hz) % 1000);
    char *e = out + strlen(out) - 1;
    while (*e == '0') *e-- = 0;
    if (*e == '.') *e = 0;
}

static void url_enc(char *out, size_t cap, const char *in)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; in && *in && o + 4 < cap; in++) {
        const uint8_t c = (uint8_t)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.!~*'()", c)) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 15];
        }
    }
    out[o] = 0;
}

int wsdr_param_cmd(char *out, size_t cap, const wsdr_band_t *b, const wsdr_tune_t *t, const char *name)
{
    char lo[16], hi[16], nm[100];
    khz_text(lo, sizeof lo, t->lo);
    khz_text(hi, sizeof hi, t->hi);
    url_enc(nm, sizeof nm, name);
    const int64_t f = wsdr_carrier_hz(b, t);
    const int n = snprintf(out, cap, "GET /~~param?f=%lld.%03d&band=%d&lo=%s&hi=%s&mode=%u&name=%s",
                           (long long)(f / 1000), (int)(f % 1000), t->band, lo, hi, (unsigned)t->mode, nm);
    return n >= 0 && (size_t)n < cap ? n : -1;
}

int wsdr_flag_cmd(char *out, size_t cap, const char *key, bool on)
{
    const int n = snprintf(out, cap, "GET /~~param?%s=%d", key, on ? 1 : 0);
    return n >= 0 && (size_t)n < cap ? n : -1;
}

int wsdr_s10(int dbm10)
{
    if (dbm10 <= WSDR_S9_DBM10) {
        const int s = 90 + (dbm10 - WSDR_S9_DBM10) * 10 / 60;
        return s < 0 ? 0 : s;
    }
    return 90 + (dbm10 - WSDR_S9_DBM10) / 10;
}

/* ------------------------------------------------------- how it ended */

static const char *const WORD[] = {
    [WSDR_END_NONE] = "",               [WSDR_END_WANT] = "",
    [WSDR_END_NOT_FOUND] = "NOT FOUND", [WSDR_END_NO_ROUTE] = "CAN'T REACH",
    [WSDR_END_NO_SOCKET] = "NO SOCKET", [WSDR_END_NO_ANSWER] = "NO ANSWER",
    [WSDR_END_CERT] = "CERTIFICATE?",   [WSDR_END_MOVED] = "MOVED",
    [WSDR_END_NOT_WSDR] = "NOT A WEBSDR", [WSDR_END_REFUSED] = "REFUSED",
    [WSDR_END_DOWN] = "DOWN",           [WSDR_END_BUSY] = "BUSY",
    [WSDR_END_IDLE] = "IDLE",           [WSDR_END_QUIET] = "NO ANSWER",
    [WSDR_END_CLOSED] = "NO ANSWER",    [WSDR_END_PROTOCOL] = "NO ANSWER",
};

const char *wsdr_end_word(wsdr_end_t e)
{
    return (unsigned)e < sizeof WORD / sizeof WORD[0] && WORD[e] ? WORD[e] : "NO ANSWER";
}

wsdr_end_t wsdr_http(int status)
{
    if (status == 101) return WSDR_END_NONE;
    if (status == 0) return WSDR_END_NO_ANSWER;
    if (status == 401 || status == 403) return WSDR_END_REFUSED;
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) return WSDR_END_MOVED;
    if (status >= 500 && status <= 599) return WSDR_END_DOWN;
    return WSDR_END_NOT_WSDR;
}

uint32_t wsdr_retry_ms(wsdr_end_t e, unsigned tries)
{
    if (!tries) tries = 1;
    switch (e) {
    case WSDR_END_NONE: case WSDR_END_WANT: case WSDR_END_IDLE:
    case WSDR_END_CERT: case WSDR_END_MOVED: case WSDR_END_NOT_WSDR: case WSDR_END_REFUSED:
        return 0;
    case WSDR_END_BUSY:
        return 5 * 60 * 1000;
    default: {
        const unsigned sh = tries - 1 < 8 ? tries - 1 : 8;     /* 2, 4 ... 512 s */
        return 2000u << sh;
    }
    }
}

/* ------------------------------------------------------ the site's name */

/* <title> found by its letters, any case; its text to "</", the entities
 * a title has (&amp; &lt; &gt; &quot; &#39; &nbsp;) read, runs of spaces one. */
enum { TT_SEEK, TT_TAG, TT_TEXT, TT_ENT };

static void title_put(wsdr_title_t *t, char c)
{
    if ((uint8_t)c < ' ') c = ' ';
    if (c == ' ' && (!t->n || t->title[t->n - 1] == ' ')) return;
    if (t->n < sizeof t->title - 1) t->title[t->n++] = c;
    else t->done = true;
}

void wsdr_title_feed(wsdr_title_t *t, const uint8_t *p, size_t n)
{
    static const char OPEN[] = "<title";
    char *ent = t->ent;
    for (size_t i = 0; i < n && !t->done; i++) {
        const char c = (char)p[i];
        switch (t->st) {
        case TT_SEEK: {
            const char l = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            t->m = l == OPEN[t->m] ? (uint8_t)(t->m + 1) : l == '<' ? 1 : 0;
            if (!OPEN[t->m]) t->st = TT_TAG;
            break;
        }
        case TT_TAG:
            if (c == '>') t->st = TT_TEXT;
            break;
        case TT_TEXT:
            if (c == '<') t->done = true;
            else if (c == '&') {
                t->st = TT_ENT;
                t->m = 0;
            } else title_put(t, c);
            break;
        case TT_ENT:
            if (c == ';' || t->m >= sizeof t->ent - 1) {
                ent[t->m] = 0;
                const char *r = !strcmp(ent, "amp") ? "&" : !strcmp(ent, "lt") ? "<" : !strcmp(ent, "gt") ? ">" :
                                !strcmp(ent, "quot") ? "\"" : !strcmp(ent, "#39") ? "'" : " ";
                title_put(t, *r);
                t->st = TT_TEXT;
            } else {
                ent[t->m++] = c;
            }
            break;
        }
    }
}

const char *wsdr_title_end(wsdr_title_t *t)
{
    while (t->n && t->title[t->n - 1] == ' ') t->n--;
    /* A title in quotes of its own ("\"WebSDR 2.1 at ...\""): without them. */
    if (t->n >= 2 && t->title[0] == '"' && t->title[t->n - 1] == '"') {
        memmove(t->title, t->title + 1, t->n - 2);
        t->n = (uint8_t)(t->n - 2);
    }
    /* A character cut short at the end: left out whole. */
    size_t k = t->n;
    while (k && ((uint8_t)t->title[k - 1] & 0xC0) == 0x80) k--;
    if (k && ((uint8_t)t->title[k - 1] & 0x80)) {
        const uint8_t lead = (uint8_t)t->title[k - 1];
        const size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (t->n - (k - 1) < want) t->n = (uint8_t)(k - 1);
    }
    t->title[t->n] = 0;
    return t->title;
}

/* ------------------------------------------------- a band plan's names */

typedef struct { int32_t lo_khz, hi_khz; const char *name; bool ham; } named_t;

/* The amateur bands (IARU region 1, widest), the broadcast bands by their
 * metres, CB. A range is named by the first it overlaps most of. */
static const named_t NAMED[] = {
    { 135, 138, "2200 m", true },  { 472, 479, "630 m", true },     { 1810, 2000, "160 m", true },
    { 3500, 4000, "80 m", true },  { 5250, 5450, "60 m", true },    { 7000, 7300, "40 m", true },
    { 10100, 10150, "30 m", true }, { 14000, 14350, "20 m", true }, { 18068, 18168, "17 m", true },
    { 21000, 21450, "15 m", true }, { 24890, 24990, "12 m", true }, { 28000, 29700, "10 m", true },
    { 148, 284, "LW", false },     { 526, 1607, "MW", false },      { 2300, 2495, "120 m BC", false },
    { 3200, 3400, "90 m BC", false }, { 3900, 4000, "75 m BC", false }, { 4750, 5060, "60 m BC", false },
    { 5900, 6200, "49 m BC", false }, { 7200, 7450, "41 m BC", false }, { 9400, 9900, "31 m BC", false },
    { 11600, 12100, "25 m BC", false }, { 13570, 13870, "22 m BC", false }, { 15100, 15800, "19 m BC", false },
    { 17480, 17900, "16 m BC", false }, { 18900, 19020, "15 m BC", false }, { 21450, 21850, "13 m BC", false },
    { 25600, 26100, "11 m BC", false }, { 26960, 27410, "CB", false },
};

void wsdr_range_name(const wsdr_range_t *r, char *out, size_t cap, bool *ham)
{
    const int64_t lo = r->lo_hz / 1000, hi = r->hi_hz / 1000, len = hi > lo ? hi - lo : 1;
    for (size_t i = 0; i < sizeof NAMED / sizeof NAMED[0]; i++) {
        const named_t *n = &NAMED[i];
        const int64_t a = lo > n->lo_khz ? lo : n->lo_khz, b = hi < n->hi_khz ? hi : n->hi_khz;
        if (b > a && (b - a) * 2 >= len) {
            snprintf(out, cap, "%s", n->name);
            if (ham) *ham = n->ham;
            return;
        }
    }
    snprintf(out, cap, "%lld.%lld MHz", (long long)(lo / 1000), (long long)(lo % 1000 / 100));
    if (ham) *ham = false;
}
