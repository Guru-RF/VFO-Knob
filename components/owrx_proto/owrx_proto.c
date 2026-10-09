/* OpenWebRX's protocol, the plain-C parts. See owrx_proto.h.
 *
 * Checked against the servers' sources (jketterl/openwebrx 1.2.2 and
 * develop, luarvique/openwebrx 1.2.126), OWRX-PROTOCOL.md giving the lines:
 * every message the server sends is Python's json.dumps of {"type": T, ...},
 * the type first, non-ASCII as \u escapes, no NaN; its config comes as diffs,
 * a key it removed as null; status.json lists each SDR's profiles with where
 * they are, by name, and the WebSocket's profiles by id and name alone. */
#include "owrx_proto.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* `n` bytes of src at most, cut where a character ends. */
static void cpyn(char *dst, size_t cap, const char *src, size_t n)
{
    if (!cap) return;
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xC0) == 0x80) n--;     /* a continuation byte: back */
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void cpy(char *dst, size_t cap, const char *src) { cpyn(dst, cap, src, strlen(src)); }

/* ------------------------------------------------------------- an address */

uint32_t owrx_fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static bool host_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_';
}

static bool ends_with(const char *s, size_t n, const char *tail)
{
    const size_t t = strlen(tail);
    return n >= t && !strncasecmp(s + n - t, tail, t);
}

bool owrx_url(const char *in, owrx_url_t *u)
{
    if (!in || !u) return false;
    memset(u, 0, sizeof *u);
    while (*in == ' ' || *in == '\t') in++;
    u->port = 80;
    /* A scheme: its letters, then "://", before any '/'. */
    const char *sep = strstr(in, "://");
    if (sep && sep < in + strcspn(in, "/")) {
        if (sep - in == 5 && !strncasecmp(in, "https", 5)) {
            u->tls  = true;
            u->port = 443;
        } else if (!(sep - in == 4 && !strncasecmp(in, "http", 4))) {
            return false;
        }
        in = sep + 3;
    }
    const size_t n = strcspn(in, ":/?# \t\r\n");
    if (!n || n >= sizeof u->host) return false;
    for (size_t i = 0; i < n; i++)
        if (!host_char(in[i])) return false;            /* "[::1]", a stray character */
    memcpy(u->host, in, n);
    in += n;
    if (*in == ':') {
        char *e;
        const long v = strtol(in + 1, &e, 10);
        if (e == in + 1 || v < 1 || v > 65535 || !strchr("/?# \t\r\n", *e)) return false;
        u->port = (uint16_t)v;
        in = e;
    }
    /* The path, '/' doubled once, its page's name left off, a '/' last. */
    size_t o = 0;
    u->path[o++] = '/';
    if (*in == '/') {
        const size_t len = strcspn(in, "?# \t\r\n");
        for (size_t i = 0; i < len; i++) {
            if (in[i] == '/' && u->path[o - 1] == '/') continue;
            if (o >= sizeof u->path - 1) return false;
            u->path[o++] = in[i];
        }
        size_t last = o;
        while (last && u->path[last - 1] != '/') last--;
        if (last < o && (ends_with(u->path, o, ".html") || ends_with(u->path, o, ".htm"))) o = last;
        if (u->path[o - 1] != '/') {
            if (o >= sizeof u->path - 1) return false;
            u->path[o++] = '/';
        }
    }
    u->path[o] = 0;
    return true;
}

int owrx_path(const owrx_url_t *u, const char *leaf, char *out, size_t cap)
{
    if (!u || !leaf || !out || !cap) return -1;
    const int n = snprintf(out, cap, "%s%s", u->path[0] ? u->path : "/", leaf);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

int owrx_url_text(const owrx_url_t *u, char *out, size_t cap)
{
    if (!u || !out || !cap) return -1;
    char pp[8] = "";
    if (u->port != (u->tls ? 443 : 80)) snprintf(pp, sizeof pp, ":%u", (unsigned)u->port);
    const int n = snprintf(out, cap, "%s://%s%s%s", u->tls ? "https" : "http", u->host, pp,
                           u->path[0] ? u->path : "/");
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

uint32_t owrx_key(const owrx_url_t *u)
{
    uint32_t h = OWRX_FNV0;
    for (const char *p = u->host; *p; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        h = owrx_fnv(h, &c, 1);
    }
    char ps[8];
    const int n = snprintf(ps, sizeof ps, ":%u", (unsigned)u->port);
    h = owrx_fnv(h, ps, (size_t)n);
    if (u->path[0] && strcmp(u->path, "/")) h = owrx_fnv(h, u->path, strlen(u->path));
    return h ? h : 1;
}

/* ------------------------------------------------------------- JSON */

/* What the reader is waiting for. */
enum { J_VAL, J_OKEY, J_KEY, J_COLON, J_NEXT, J_STR, J_WORD, J_DONE };
/* What it tells its reader of: a string, a bare word (a number, true, false,
 * null) -- each with the path to it -- and a container closed, with the
 * path to the container. */
enum { JE_STR, JE_WORD, JE_END };
typedef bool (*jev_t)(void *ctx, owrx_json_t *j, int ev);

static void jinit(owrx_json_t *j)
{
    memset(j, 0, sizeof *j);
    j->st = J_VAL;
}

/* Whether the reader is at `p`: its levels '.' apart, each a key or "[]"
 * for an array's element. */
static bool jat(const owrx_json_t *j, const char *p)
{
    if (j->depth > OWRX_JDEPTH) return false;
    for (unsigned l = 0; l < j->depth; l++) {
        const size_t n = strcspn(p, ".");
        if ((j->arr >> l) & 1u) {
            if (n != 2 || p[0] != '[' || p[1] != ']') return false;
        } else if (strlen(j->key[l]) != n || memcmp(j->key[l], p, n)) {
            return false;
        }
        p += n;
        if (*p == '.') p++;
        else if (l + 1 < j->depth) return false;
    }
    return !*p;
}

static void jput(owrx_json_t *j, const void *b, size_t k)
{
    j->h = owrx_fnv(j->h, b, k);
    if (j->cut || j->n + k >= sizeof j->s) {
        j->cut = true;
        return;
    }
    memcpy(j->s + j->n, b, k);
    j->n = (uint16_t)(j->n + k);
}

static void jput_cp(owrx_json_t *j, uint32_t cp)
{
    uint8_t b[4];
    size_t k;
    if (cp < 0x80)        { b[0] = (uint8_t)cp; k = 1; }
    else if (cp < 0x800)  { b[0] = (uint8_t)(0xC0 | cp >> 6); b[1] = (uint8_t)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000) {
        b[0] = (uint8_t)(0xE0 | cp >> 12);
        b[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (uint8_t)(0x80 | (cp & 0x3F));
        k = 3;
    } else {
        b[0] = (uint8_t)(0xF0 | cp >> 18);
        b[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (uint8_t)(0x80 | (cp & 0x3F));
        k = 4;
    }
    jput(j, b, k);
}

/* A high surrogate with no low one after it: U+FFFD in its place. */
static void jflush_hi(owrx_json_t *j)
{
    if (j->hi) jput_cp(j, 0xFFFD);
    j->hi = 0;
}

/* The string ended: no character left cut in two at its end. */
static void jstr_end(owrx_json_t *j)
{
    jflush_hi(j);
    if (j->cut && j->n) {
        size_t lead = j->n - 1, back = 0;
        while (lead && back < 3 && ((unsigned char)j->s[lead] & 0xC0) == 0x80) { lead--; back++; }
        const unsigned char c = (unsigned char)j->s[lead];
        const size_t want = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (j->n - lead < want) j->n = (uint16_t)lead;
    }
    j->s[j->n] = 0;
}

static void jstr_begin(owrx_json_t *j)
{
    j->n = 0;
    j->h = OWRX_FNV0;
    j->cut = false;
    j->esc = 0;
    j->hi = 0;
    j->s[0] = 0;
}

static bool word_char(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '+' || c == '-' || c == '.';
}

static int hexv(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A value read: what follows it. */
static void jvalue_done(owrx_json_t *j) { j->st = j->depth ? J_NEXT : J_DONE; }

/* A container closed by `c`: false where it is not the one open. */
static bool jclose(owrx_json_t *j, uint8_t c, jev_t ev, void *ctx, bool *go)
{
    if (!j->depth) return false;
    const unsigned l = j->depth - 1u;
    if ((bool)((j->arr >> l) & 1u) != (c == ']')) return false;
    j->depth--;
    j->arr &= ~(1ull << l);
    *go = ev(ctx, j, JE_END);
    jvalue_done(j);
    return true;
}

static bool jopen(owrx_json_t *j, bool arr)
{
    if (j->depth >= 64) return false;                   /* deeper than anything it sends */
    const unsigned l = j->depth;
    if (arr) j->arr |= 1ull << l;
    else     j->arr &= ~(1ull << l);
    if (l < OWRX_JDEPTH) j->key[l][0] = 0;
    j->depth++;
    j->st = arr ? J_VAL : J_OKEY;
    return true;
}

/* `n` bytes of JSON on from where the last piece left it. False where the
 * reader asked to stop. */
static bool jfeed(owrx_json_t *j, const uint8_t *p, size_t n, jev_t ev, void *ctx)
{
    bool go = true;
    for (size_t i = 0; i < n && go && !j->bad; i++) {
        const uint8_t c = p[i];
        switch (j->st) {
        case J_DONE:
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') j->bad = true;
            break;
        case J_VAL:
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
            if (c == '"') {
                jstr_begin(j);
                j->st = J_STR;
            } else if (c == '{' || c == '[') {
                if (!jopen(j, c == '[')) j->bad = true;
            } else if (c == ']') {
                if (!jclose(j, c, ev, ctx, &go)) j->bad = true;   /* [] -- or [1,] */
            } else if (word_char(c)) {
                jstr_begin(j);
                jput(j, &c, 1);
                j->st = J_WORD;
            } else {
                j->bad = true;
            }
            break;
        case J_OKEY:
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
            if (c == '"') {
                jstr_begin(j);
                j->st = J_KEY;
            } else if (c != '}' || !jclose(j, c, ev, ctx, &go)) {
                j->bad = true;
            }
            break;
        case J_COLON:
            if (c == ':') j->st = J_VAL;
            else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') j->bad = true;
            break;
        case J_NEXT:
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
            if (c == ',') j->st = (j->arr >> (j->depth - 1u)) & 1u ? J_VAL : J_OKEY;
            else if ((c != '}' && c != ']') || !jclose(j, c, ev, ctx, &go)) j->bad = true;
            break;
        case J_WORD:
            if (word_char(c)) {
                jput(j, &c, 1);
                break;
            }
            jstr_end(j);
            go = ev(ctx, j, JE_WORD);
            jvalue_done(j);
            i--;                                        /* this one again, as what follows */
            break;
        case J_STR:
        case J_KEY:
            if (j->esc == 1) {
                static const char FROM[] = "\"\\/bfnrt", TO[] = "\"\\/\b\f\n\r\t";
                const char *k = c ? strchr(FROM, c) : NULL;
                if (c == 'u') {
                    j->esc = 2;
                    j->u = 0;
                    break;
                }
                if (!k) {
                    j->bad = true;
                    break;
                }
                jflush_hi(j);
                jput(j, &TO[k - FROM], 1);
                j->esc = 0;
            } else if (j->esc >= 2) {
                const int v = hexv(c);
                if (v < 0) {
                    j->bad = true;
                    break;
                }
                j->u = (uint16_t)(j->u << 4 | (unsigned)v);
                if (++j->esc < 6) break;
                j->esc = 0;
                const uint16_t u = j->u;
                if (u >= 0xD800 && u <= 0xDBFF) {
                    jflush_hi(j);
                    j->hi = u;
                } else if (u >= 0xDC00 && u <= 0xDFFF) {
                    if (j->hi) {
                        const uint32_t cp = 0x10000u + ((uint32_t)(j->hi - 0xD800) << 10) + (u - 0xDC00u);
                        j->hi = 0;
                        jput_cp(j, cp);
                    } else {
                        jput_cp(j, 0xFFFD);
                    }
                } else {
                    jflush_hi(j);
                    jput_cp(j, u);
                }
            } else if (c == '\\') {
                j->esc = 1;
            } else if (c == '"') {
                jstr_end(j);
                if (j->st == J_KEY) {
                    const unsigned l = j->depth - 1u;
                    if (l < OWRX_JDEPTH) {
                        if (j->cut || j->n >= OWRX_JKEY) memcpy(j->key[l], "\x01", 2);  /* matches none */
                        else memcpy(j->key[l], j->s, (size_t)j->n + 1);
                    }
                    j->st = J_COLON;
                } else {
                    go = ev(ctx, j, JE_STR);
                    jvalue_done(j);
                }
            } else if (c < 0x20) {
                j->bad = true;
            } else {
                jflush_hi(j);
                jput(j, &c, 1);
            }
            break;
        }
    }
    return go;
}

/* The bare word that ends a message without anything after it. */
static void jfinish(owrx_json_t *j, jev_t ev, void *ctx)
{
    if (j->st == J_WORD && !j->bad) {
        jstr_end(j);
        ev(ctx, j, JE_WORD);
        jvalue_done(j);
    }
}

static bool is_null(const owrx_json_t *j) { return !strcmp(j->s, "null"); }

/* A number as a 64-bit integer: "14070000", "1.407e7", "14070000.0". */
static bool num_i64(const char *s, int64_t *v)
{
    if (!*s || !strcmp(s, "null") || !strcmp(s, "true") || !strcmp(s, "false")) return false;
    char *e;
    if (!strpbrk(s, ".eE")) {
        const long long x = strtoll(s, &e, 10);
        if (*e || x == LLONG_MAX || x == LLONG_MIN) return false;
        *v = x;
        return true;
    }
    const double d = strtod(s, &e);
    if (*e || !(d > -9.0e18 && d < 9.0e18)) return false;
    *v = (int64_t)llround(d);
    return true;
}

static bool num_f(const char *s, float *v)
{
    if (!*s) return false;
    char *e;
    const double d = strtod(s, &e);
    if (*e || d != d || d > 3.0e38 || d < -3.0e38) return false;
    *v = (float)d;
    return true;
}

static int32_t clamp32(int64_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : (int32_t)v;
}

/* ------------------------------------------------------- what it says */

/* The types a text frame has, and what the knob does with each. */
enum { K_NONE, K_SKIP, K_PLUS, K_CONFIG, K_SMETER, K_DETAILS, K_PROFILES, K_CLIENTS,
       K_BACKOFF, K_SDR_ERROR, K_LOG, K_DEMOD_ERROR };
static const struct { const char *t; uint8_t k; } KINDS[] = {
    { "config", K_CONFIG },             { "smeter", K_SMETER },
    { "receiver_details", K_DETAILS },  { "profiles", K_PROFILES },
    { "clients", K_CLIENTS },           { "backoff", K_BACKOFF },
    { "sdr_error", K_SDR_ERROR },       { "log_message", K_LOG },
    { "demodulator_error", K_DEMOD_ERROR },
    /* OpenWebRX+'s alone: they say which it is, and no more is read. */
    { "bands", K_PLUS },                { "temperature", K_PLUS },
    { "battery", K_PLUS },              { "chat_message", K_PLUS },
};

/* config's keys OpenWebRX+ has and upstream has not. */
static const char *const PLUS_KEYS[] = {
    "tuning_step", "allow_center_freq_changes", "allow_chat", "allow_audio_recording",
    "ui_theme", "callsign_url", "vessel_url", "flight_url", "modes_url", "receiver_gps",
    "initial_nr_level",
};

enum { M_SNIFF, M_JSON, M_LINE };

void owrx_said_init(owrx_said_t *s)
{
    memset(s, 0, sizeof *s);
    s->sq_init = -150;
    s->wf_min  = -88.0f;                            /* the servers' own defaults */
    s->wf_max  = -20.0f;
    s->clients = -1;
    jinit(&s->j);
}

void owrx_text_begin(owrx_said_t *s)
{
    jinit(&s->j);
    s->ev     = 0;
    s->kind   = K_NONE;
    s->mode   = M_SNIFF;
    s->skip   = false;
    s->type[0] = 0;
    s->line_n = 0;
    s->nb     = 0;
    s->seen   = 0;
    s->b_id   = false;
    memset(&s->b, 0, sizeof s->b);
    cpy(s->sdr_was, sizeof s->sdr_was, s->sdr_id);
}

static void config_key(owrx_said_t *s, owrx_json_t *j, const char *k, int ev)
{
    for (size_t i = 0; i < sizeof PLUS_KEYS / sizeof PLUS_KEYS[0]; i++)
        if (!strcmp(k, PLUS_KEYS[i])) s->plus = true;
    if (ev == JE_END) return;                           /* an object's: receiver_gps */
    if (ev == JE_WORD && is_null(j)) {                  /* removed: what it was stays */
        if (!strcmp(k, "initial_squelch_level")) s->sq_init = -150;
        return;
    }
    int64_t v;
    const char *t = j->s;
    if (!strcmp(k, "center_freq")) {
        if (ev == JE_WORD && num_i64(t, &v) && v > 0) {
            s->center = v;
            s->ev |= OWRX_EV_SPAN;
        }
    } else if (!strcmp(k, "samp_rate")) {
        if (ev == JE_WORD && num_i64(t, &v) && v > 0) {
            s->rate = clamp32(v, 1, 2000000000);
            s->ev |= OWRX_EV_SPAN;
        }
    } else if (!strcmp(k, "start_freq")) {
        if (ev == JE_WORD && num_i64(t, &v) && v > 0) s->start = v;
    } else if (!strcmp(k, "start_mod")) {
        if (ev == JE_STR) cpy(s->start_mod, sizeof s->start_mod, t);
    } else if (!strcmp(k, "sdr_id") || !strcmp(k, "profile_id")) {
        if (ev != JE_STR) return;
        if (k[0] == 's') cpy(s->sdr_id, sizeof s->sdr_id, t);
        else             cpy(s->profile_id, sizeof s->profile_id, t);
        s->ev |= OWRX_EV_SPAN;
    } else if (!strcmp(k, "initial_squelch_level")) {
        float f;
        /* An integer, as the page takes one; else open. */
        s->sq_init = ev == JE_WORD && num_f(t, &f) && f == floorf(f) && f >= -150.0f && f <= 0.0f
                   ? (int16_t)f : -150;
    } else if (!strcmp(k, "audio_compression")) {
        if (ev == JE_STR) s->audio_raw = !strcmp(t, "none");
    } else if (!strcmp(k, "fft_compression")) {
        if (ev == JE_STR) s->fft_raw = !strcmp(t, "none");
    } else if (!strcmp(k, "fft_size")) {
        if (ev == JE_WORD && num_i64(t, &v)) s->fft_size = clamp32(v, 0, 1 << 20);
    } else if (!strcmp(k, "max_clients")) {
        if (ev == JE_WORD && num_i64(t, &v)) s->max_clients = clamp32(v, 0, 100000);
    } else if (!strcmp(k, "tuning_step")) {
        /* An integer, or a string of one in its factory profiles. */
        if (num_i64(t, &v)) s->tuning_step = clamp32(v, 0, 10000000);
    } else {
        return;
    }
    s->ev |= OWRX_EV_CONFIG;
}

static bool said_ev(void *ctx, owrx_json_t *j, int ev)
{
    owrx_said_t *s = ctx;
    if (ev == JE_STR && s->kind == K_NONE && jat(j, "type")) {
        cpy(s->type, sizeof s->type, j->s);
        s->kind = K_SKIP;
        for (size_t i = 0; i < sizeof KINDS / sizeof KINDS[0]; i++)
            if (!strcmp(j->s, KINDS[i].t)) s->kind = KINDS[i].k;
        if (s->kind == K_PLUS) s->plus = true;
        s->skip = s->kind == K_SKIP || s->kind == K_PLUS;
        return !s->skip;
    }
    switch (s->kind) {
    case K_CONFIG:
        if (j->depth == 2 && !(j->arr & 3u) && !strcmp(j->key[0], "value"))
            config_key(s, j, j->key[1], ev);
        else if (ev == JE_WORD && (jat(j, "value.waterfall_levels.min") || jat(j, "value.waterfall_levels.max"))) {
            float f;
            if (num_f(j->s, &f) && f > -200.0f && f < 100.0f) {
                if (j->key[2][1] == 'i') s->wf_min = f;
                else                     s->wf_max = f;
                s->ev |= OWRX_EV_CONFIG;
            }
        }
        break;
    case K_SMETER:
        if (ev == JE_WORD && jat(j, "value") && num_f(j->s, &s->meter)) {
            s->have_meter = true;
            s->ev |= OWRX_EV_METER;
        }
        break;
    case K_DETAILS:
        if (ev == JE_STR && jat(j, "value.receiver_name")) {
            cpy(s->name, sizeof s->name, j->s);
            s->ev |= OWRX_EV_NAME;
        }
        break;
    case K_PROFILES:
        if (ev == JE_STR && jat(j, "value.[].id")) {
            if (!j->cut && j->n < sizeof s->b.id) memcpy(s->b.id, j->s, (size_t)j->n + 1);
            else s->b.id[0] = 0;                        /* too long to send back */
            s->b_id = true;
        } else if (ev == JE_STR && jat(j, "value.[].name")) {
            cpy(s->b.name, sizeof s->b.name, j->s);
            s->b.name_h = j->h;
        } else if (ev == JE_END && jat(j, "value.[]")) {
            if (s->b_id) {
                if (s->nb < OWRX_BANDS) s->bands[s->nb++] = s->b;
                if (s->seen < 0xFFFF) s->seen++;
            }
            memset(&s->b, 0, sizeof s->b);
            s->b_id = false;
        } else if (ev == JE_END && jat(j, "value")) {
            s->n_bands    = s->nb;
            s->bands_seen = s->seen;
            s->ev |= OWRX_EV_BANDS;
        }
        break;
    case K_CLIENTS: {
        int64_t v;
        if (ev == JE_WORD && jat(j, "value") && num_i64(j->s, &v)) {
            s->clients = clamp32(v, 0, 1000000);
            s->ev |= OWRX_EV_CLIENTS;
        }
        break;
    }
    case K_BACKOFF:
        if (ev == JE_STR && jat(j, "reason")) {
            cpy(s->reason, sizeof s->reason, j->s);
            s->end = strstr(j->s, "banned") ? OWRX_END_BANNED : OWRX_END_FULL;   /* "Client address banned" */
            s->ev |= OWRX_EV_END;
        }
        break;
    case K_SDR_ERROR:
        if (ev == JE_STR && jat(j, "value")) {
            cpy(s->reason, sizeof s->reason, j->s);
            s->end = OWRX_END_NO_SDR;
            s->ev |= OWRX_EV_END;
        }
        break;
    case K_LOG:
    case K_DEMOD_ERROR:
        if (ev == JE_STR && jat(j, "value")) {
            cpy(s->log, sizeof s->log, j->s);
            s->ev |= s->kind == K_LOG ? OWRX_EV_LOG : OWRX_EV_DEMOD;
        }
        break;
    default:
        break;
    }
    return true;
}

void owrx_text_feed(owrx_said_t *s, const uint8_t *p, size_t n)
{
    if (!s || !p) return;
    size_t i = 0;
    if (s->mode == M_SNIFF) {
        while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) i++;
        if (i == n) return;
        s->mode = p[i] == '{' ? M_JSON : M_LINE;
    }
    if (s->mode == M_LINE) {
        for (; i < n && s->line_n < sizeof s->line - 1; i++) s->line[s->line_n++] = (char)p[i];
        return;
    }
    if (!s->skip) jfeed(&s->j, p + i, n - i, said_ev, s);
}

/* "CLIENT DE SERVER server=openwebrx version=v1.2.126" */
static void hello(owrx_said_t *s)
{
    s->line[s->line_n] = 0;
    static const char HELLO[] = "CLIENT DE SERVER";
    if (strncmp(s->line, HELLO, sizeof HELLO - 1)) return;
    bool owrx = false;
    char *save = NULL;
    for (char *t = strtok_r(s->line + sizeof HELLO - 1, " \r\n", &save); t; t = strtok_r(NULL, " \r\n", &save)) {
        if (!strcmp(t, "server=openwebrx")) owrx = true;
        else if (!strncmp(t, "version=", 8)) cpy(s->version, sizeof s->version, t + 8);
    }
    if (owrx) {
        s->hello = true;
        s->ev |= OWRX_EV_HELLO;
    } else {
        s->end = OWRX_END_NOT_OWRX;
        s->ev |= OWRX_EV_END;
    }
}

uint32_t owrx_text_end(owrx_said_t *s)
{
    if (s->mode == M_LINE) {
        hello(s);
    } else if (s->mode == M_JSON && !s->skip) {
        jfinish(&s->j, said_ev, s);
        /* A refusal that gave no reason is a refusal all the same. */
        if (s->kind == K_BACKOFF && !(s->ev & OWRX_EV_END)) {
            s->end = OWRX_END_FULL;
            s->ev |= OWRX_EV_END;
        } else if (s->kind == K_SDR_ERROR && !(s->ev & OWRX_EV_END)) {
            s->end = OWRX_END_NO_SDR;
            s->ev |= OWRX_EV_END;
        }
    }
    if ((s->ev & OWRX_EV_SPAN) && strcmp(s->sdr_was, s->sdr_id)) s->ev |= OWRX_EV_SDR;
    const uint32_t ev = s->ev;
    s->ev = 0;
    s->mode = M_SNIFF;
    return ev;
}

bool owrx_in_span(const owrx_said_t *s, int64_t hz)
{
    if (!s || s->center <= 0 || s->rate <= 0) return false;
    const int64_t d = hz - s->center;
    return d >= -(int64_t)(s->rate / 2) && d <= (int64_t)(s->rate / 2);
}

int owrx_band_now(const owrx_said_t *s)
{
    if (!s || !s->sdr_id[0] || !s->profile_id[0]) return -1;
    char id[OWRX_ID_MAX];
    const int n = snprintf(id, sizeof id, "%s|%s", s->sdr_id, s->profile_id);
    if (n < 0 || (size_t)n >= sizeof id) return -1;
    for (int i = 0; i < s->n_bands; i++)
        if (!strcmp(s->bands[i].id, id)) return i;
    return -1;
}

int owrx_band_for(const owrx_said_t *s, int64_t hz)
{
    for (int i = 0; s && i < s->n_bands; i++) {
        const owrx_band_t *b = &s->bands[i];
        if (b->center > 0 && b->rate > 0 && b->id[0] && hz >= b->center - b->rate / 2 &&
            hz <= b->center + b->rate / 2)
            return i;
    }
    return -1;
}

float owrx_db(float power)
{
    if (!(power > 1e-15f)) return -150.0f;              /* nothing, or not a number */
    const float db = 10.0f * log10f(power);
    return db < -150.0f ? -150.0f : db;
}

void owrx_meter_range(const owrx_said_t *s, float *lo, float *hi)
{
    float l = s->wf_min - 20.0f, h = s->wf_max + 20.0f;
    if (!(h - l >= 10.0f)) {                            /* a range it cannot mean */
        l = -108.0f;
        h = 0.0f;
    }
    if (lo) *lo = l;
    if (hi) *hi = h;
}

int16_t owrx_squelch_db(const owrx_said_t *s, uint8_t pct)
{
    if (!pct) return -150;
    if (pct > 100) pct = 100;
    float lo, hi;
    owrx_meter_range(s, &lo, &hi);
    const float db = lo + (hi - lo) * (float)pct / 100.0f;
    return (int16_t)(db < -150.0f ? -150.0f : db > 0.0f ? 0.0f : lroundf(db));
}

/* ----------------------------------------------------------- status.json */

void owrx_status_init(owrx_status_t *st)
{
    memset(st, 0, sizeof *st);
    st->max_clients = -1;
    jinit(&st->j);
}

static bool status_ev(void *ctx, owrx_json_t *j, int ev)
{
    owrx_status_t *st = ctx;
    int64_t v;
    if (ev == JE_STR && jat(j, "receiver.name")) {
        cpy(st->name, sizeof st->name, j->s);
    } else if (ev == JE_STR && jat(j, "version")) {
        cpy(st->version, sizeof st->version, j->s);
    } else if (ev == JE_WORD && jat(j, "max_clients") && num_i64(j->s, &v)) {
        st->max_clients = clamp32(v, 0, 100000);
    } else if (ev == JE_STR && jat(j, "sdrs.[].name")) {
        st->sdr_named = !j->cut && j->n < sizeof st->sdr && j->n < 255;
        cpy(st->sdr, sizeof st->sdr, st->sdr_named ? j->s : "");
    } else if (ev == JE_STR && jat(j, "sdrs.[].profiles.[].name")) {
        st->pnamed = !j->cut && j->n < sizeof st->pname;
        cpy(st->pname, sizeof st->pname, st->pnamed ? j->s : "");
    } else if (ev == JE_WORD && jat(j, "sdrs.[].profiles.[].center_freq") && num_i64(j->s, &v)) {
        st->cur.center = v > 0 ? v : 0;
    } else if (ev == JE_WORD && jat(j, "sdrs.[].profiles.[].sample_rate") && num_i64(j->s, &v)) {
        st->cur.rate = v > 0 ? clamp32(v, 1, 2000000000) : 0;
    } else if (ev == JE_END && jat(j, "sdrs.[].profiles.[]")) {
        owrx_sprof_t p = st->cur;
        p.sdr = st->n_sdrs;
        p.k   = st->k;
        if (st->sdr_named && st->pnamed) {
            const size_t sl = strlen(st->sdr);
            p.h = owrx_fnv(owrx_fnv(owrx_fnv(OWRX_FNV0, st->sdr, sl), " ", 1), st->pname, strlen(st->pname));
            if (!p.h) p.h = 1;
            p.sdr_len = (uint8_t)(sl + 1 < 255 ? sl + 1 : 255);
        }
        if (st->n < OWRX_STATUS_MAX) st->p[st->n++] = p;
        if (st->profiles < 0xFFFF) st->profiles++;
        if (st->k < 0xFF) st->k++;
        memset(&st->cur, 0, sizeof st->cur);
        st->pnamed = false;
    } else if (ev == JE_END && jat(j, "sdrs.[]")) {
        if (st->n_sdrs < 0xFF) st->n_sdrs++;
        st->k = 0;
        st->sdr_named = false;
        st->sdr[0] = 0;
    } else if (ev == JE_END && jat(j, "sdrs")) {
        st->ok = true;
    }
    return true;
}

void owrx_status_feed(owrx_status_t *st, const uint8_t *p, size_t n)
{
    if (st && p) jfeed(&st->j, p, n, status_ev, st);
}

bool owrx_status_end(owrx_status_t *st)
{
    jfinish(&st->j, status_ev, st);
    return st->ok;
}

/* How long the SDR's part of a band's id is: up to its '|'. */
static size_t sdr_part(const char *id)
{
    const char *bar = strchr(id, '|');
    return bar ? (size_t)(bar - id) : strlen(id);
}

int owrx_join(owrx_said_t *s, const owrx_status_t *st)
{
    if (!s || !st) return 0;
    bool used[OWRX_STATUS_MAX] = { false }, placed[OWRX_BANDS] = { false };
    int n = 0;
    for (int i = 0; i < s->n_bands; i++) {
        owrx_band_t *b = &s->bands[i];
        b->center = 0;
        b->rate   = 0;
        b->label  = 0;
        for (int k = 0; k < st->n; k++) {
            if (used[k] || !st->p[k].h || st->p[k].h != b->name_h) continue;
            used[k]   = true;
            placed[i] = true;
            b->center = st->p[k].center;
            b->rate   = st->p[k].rate;
            b->label  = st->p[k].sdr_len < strlen(b->name) ? st->p[k].sdr_len : 0;
            n++;
            break;
        }
    }
    /* By their order, where every SDR's count agrees: the k-th profile of
     * the g-th SDR, both ways, as both lists come in the same order. */
    if (n == s->n_bands || st->n != st->profiles || s->bands_seen != s->n_bands) return n;
    uint8_t group[OWRX_BANDS], pos[OWRX_BANDS], size[OWRX_BANDS] = { 0 };
    int groups = 0;
    for (int i = 0; i < s->n_bands; i++) {
        const char *id = s->bands[i].id;
        int g = -1;
        for (int k = 0; k < i && g < 0; k++)
            if (sdr_part(s->bands[k].id) == sdr_part(id) && !strncmp(s->bands[k].id, id, sdr_part(id)))
                g = group[k];
        if (g < 0) g = groups++;
        group[i] = (uint8_t)g;
        pos[i]   = size[g]++;
    }
    if (groups != st->n_sdrs) return n;
    for (int g = 0; g < groups; g++) {
        int c = 0;
        for (int k = 0; k < st->n; k++) c += st->p[k].sdr == g;
        if (c != size[g]) return n;
    }
    for (int i = 0; i < s->n_bands; i++) {
        if (placed[i]) continue;
        for (int k = 0; k < st->n; k++) {
            if (used[k] || st->p[k].sdr != group[i] || st->p[k].k != pos[i]) continue;
            owrx_band_t *b = &s->bands[i];
            used[k]   = true;
            b->center = st->p[k].center;
            b->rate   = st->p[k].rate;
            n++;
            break;
        }
    }
    return n;
}

bool owrx_plus_version(const char *v)
{
    if (!v) return false;
    if (*v == 'v' || *v == 'V') v++;
    int maj = 0, min = 0, pat = 0;
    if (sscanf(v, "%d.%d.%d", &maj, &min, &pat) != 3) return false;
    return maj == 1 && min == 2 && pat >= 3;
}

/* ------------------------------------------------------------ the end */

/* The face's 28 pt words, within the warning panel's 264 px. */
static const char *const WORD[] = {
    [OWRX_END_NONE] = "",                 [OWRX_END_WANT] = "",
    [OWRX_END_NOT_FOUND] = "NOT FOUND",   [OWRX_END_NO_ROUTE] = "CAN'T REACH",
    [OWRX_END_NO_SOCKET] = "NO SOCKET",   [OWRX_END_CERT] = "CERTIFICATE?",
    [OWRX_END_NO_ANSWER] = "NO ANSWER",   [OWRX_END_CLOSED] = "NO ANSWER",
    [OWRX_END_QUIET] = "NO ANSWER",       [OWRX_END_PROTOCOL] = "NO ANSWER",
    [OWRX_END_NOT_OWRX] = "NOT OPENWEBRX", [OWRX_END_REFUSED] = "REFUSED",
    [OWRX_END_DOWN] = "DOWN",             [OWRX_END_MOVED] = "MOVED",
    [OWRX_END_FULL] = "RECEIVER FULL",    [OWRX_END_BANNED] = "BANNED",
    [OWRX_END_NO_SDR] = "NO SDR",
};

const char *owrx_end_word(owrx_end_t e)
{
    return (unsigned)e < sizeof WORD / sizeof WORD[0] && WORD[e] ? WORD[e] : "NO ANSWER";
}

owrx_end_t owrx_http(int status)
{
    if (status == 101) return OWRX_END_NONE;
    if (status == 0) return OWRX_END_NO_ANSWER;
    if (status == 401 || status == 403) return OWRX_END_REFUSED;
    if (status == 429) return OWRX_END_FULL;
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
        return OWRX_END_MOVED;
    if (status >= 500 && status <= 599) return OWRX_END_DOWN;
    return OWRX_END_NOT_OWRX;
}

uint32_t owrx_retry_ms(owrx_end_t e, unsigned tries)
{
    if (!tries) tries = 1;
    switch (e) {
    case OWRX_END_NONE: case OWRX_END_WANT:
    case OWRX_END_BANNED: case OWRX_END_CERT: case OWRX_END_MOVED:
    case OWRX_END_NOT_OWRX: case OWRX_END_REFUSED:
        return 0;
    case OWRX_END_FULL:
        return 32000;
    case OWRX_END_NO_SDR: {
        const unsigned sh = tries - 1 < 3 ? tries - 1 : 3;   /* 60, 120, 240, 480 s */
        return 60000u << sh;
    }
    default: {
        const unsigned sh = tries - 1 < 9 ? tries - 1 : 9;   /* 1, 2, 4 ... 512 s */
        return 1000u << sh;
    }
    }
}

/* ------------------------------------------------------------- modes */

static const owrx_mode_t MODES[] = {
    { "usb",    300,   3000,    150,   2750, 0 },
    { "lsb",  -3000,   -300,  -2750,   -150, 0 },
    { "cw",    -100,    100,   -100,    100, 0 },   /* 700..900 over the tone's 800 */
    { "am",   -4000,   4000,  -4000,   4000, 0 },
    { "sam",  -4000,   4000,  -4000,   4000, OWRX_M_PLUS },
    { "nfm",  -4000,   4000,  -4000,   4000, 0 },
    { "wfm", -75000,  75000, -75000,  75000, OWRX_M_HD },
};

const owrx_mode_t *owrx_mode_find(const char *mod)
{
    for (size_t i = 0; mod && i < sizeof MODES / sizeof MODES[0]; i++)
        if (!strcasecmp(MODES[i].mod, mod)) return &MODES[i];
    return NULL;
}

/* Other names, the knob's other radios' among them, and each digital mode
 * by the one under it -- OpenWebRX+'s modes.py has the most of them. */
static const struct { const char *any, *mod; } UNDER[] = {
    { "fm", "nfm" }, { "nbfm", "nfm" }, { "cwu", "cw" }, { "cwl", "cw" }, { "cwr", "cw" },
    { "digu", "usb" }, { "usbd", "usb" }, { "digl", "lsb" }, { "lsbd", "lsb" }, { "rtty", "usb" },
    { "sal", "sam" }, { "sau", "sam" },
    { "bpsk31", "usb" }, { "bpsk63", "usb" }, { "sitorb", "usb" }, { "navtex", "usb" },
    { "dsc", "usb" }, { "ft8", "usb" }, { "ft4", "usb" }, { "jt65", "usb" }, { "jt9", "usb" },
    { "wspr", "usb" }, { "fst4", "usb" }, { "fst4w", "usb" }, { "q65", "usb" },
    { "msk144", "usb" }, { "js8", "usb" }, { "cwdecoder", "usb" }, { "fax", "usb" },
    { "hfdl", "usb" }, { "sstv", "usb" }, { "freedv", "usb" }, { "radeu", "usb" },
    { "radel", "lsb" },
    { "packet", "nfm" }, { "ais", "nfm" }, { "pocsag", "nfm" }, { "page", "nfm" },
    { "selcall", "nfm" }, { "zvei", "nfm" }, { "eas", "nfm" }, { "vdl2", "nfm" },
    { "dmr", "nfm" }, { "dstar", "nfm" }, { "nxdn", "nfm" }, { "ysf", "nfm" }, { "p25", "nfm" },
    { "m17", "nfm" }, { "tetra", "nfm" },
    { "acars", "am" }, { "drm", "am" }, { "speech", "am" }, { "audio", "am" },
    { "dab", "wfm" }, { "hdr", "wfm" },
};

const char *owrx_mode_of(const char *any, int64_t hz, bool plus)
{
    const char *m = NULL;
    const owrx_mode_t *k = owrx_mode_find(any);
    if (k) m = k->mod;
    for (size_t i = 0; !m && any && i < sizeof UNDER / sizeof UNDER[0]; i++)
        if (!strcasecmp(any, UNDER[i].any)) m = UNDER[i].mod;
    if (!m && any && !strncasecmp(any, "rtty", 4)) m = "usb";
    if (!m && any && !strncasecmp(any, "sonde-", 6)) m = "nfm";
    if (!m) m = hz > 0 && hz < 10000000 ? "lsb" : hz > 0 && hz < 30000000 ? "usb" : hz > 0 ? "nfm" : "usb";
    if (!strcmp(m, "sam") && !plus) m = "am";
    return m;
}

void owrx_passband(const owrx_mode_t *m, bool plus, int32_t *lo, int32_t *hi)
{
    if (!m) return;
    if (lo) *lo = plus ? m->plo : m->lo;
    if (hi) *hi = plus ? m->phi : m->hi;
}

bool owrx_retune(const owrx_said_t *s, owrx_tune_t *t)
{
    if (!s || !t) return false;
    bool moved = false;
    if (s->center > 0 && s->rate > 0 && !owrx_in_span(s, t->hz)) {
        t->hz = s->start > 0 && owrx_in_span(s, s->start) ? s->start : s->center;
        const char *m = owrx_mode_of(s->start_mod[0] ? s->start_mod : NULL, t->hz, s->plus);
        snprintf(t->mod, sizeof t->mod, "%s", m);
        owrx_passband(owrx_mode_find(m), s->plus, &t->lo, &t->hi);
        moved = true;
    }
    const char *m = owrx_mode_of(t->mod, t->hz, s->plus);
    if (strcmp(m, t->mod)) {
        snprintf(t->mod, sizeof t->mod, "%s", m);
        owrx_passband(owrx_mode_find(m), s->plus, &t->lo, &t->hi);
        moved = true;
    }
    return moved;
}

/* ------------------------------------------------------- what the knob says */

static int done(int n, size_t cap) { return n < 0 || (size_t)n >= cap ? -1 : n; }

/* A message written a part at a time: `over` once a part did not fit. */
typedef struct { char *p; size_t cap, n; bool over; } wbuf_t;
static void wput(wbuf_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void wput(wbuf_t *w, const char *fmt, ...)
{
    if (w->over) return;
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(w->p + w->n, w->cap - w->n, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= w->cap - w->n) w->over = true;
    else w->n += (size_t)k;
}

int owrx_hello_cmd(char *out, size_t cap)
{
    if (!out || !cap) return -1;
    return done(snprintf(out, cap, "SERVER DE CLIENT client=VFO-Knob type=receiver"), cap);
}

int owrx_props_cmd(char *out, size_t cap)
{
    if (!out || !cap) return -1;
    return done(snprintf(out, cap,
                         "{\"type\":\"connectionproperties\",\"params\":{\"output_rate\":%d,"
                         "\"hd_output_rate\":%d}}", OWRX_RATE, OWRX_HD_RATE), cap);
}

int owrx_tune_cmd(char *out, size_t cap, const owrx_tune_t *t, int64_t center, int32_t rate,
                  bool plus, unsigned what)
{
    if (!out || !cap || !t || !(what & OWRX_P_ALL)) return -1;
    const owrx_mode_t *m = owrx_mode_find(t->mod);
    if (!m || ((m->flags & OWRX_M_PLUS) && !plus)) return -1;
    /* In CW the receiver listens a tone below the carrier on the dial. */
    const int32_t shift = !strcmp(m->mod, "cw") ? OWRX_CW_TONE : 0;
    const int64_t half = rate > 0 ? rate / 2 : 0;
    int64_t off = t->hz - shift - center;
    if (off < -half) off = -half;
    if (off > half) off = half;
    /* Its page's limits: +-(output_rate / 2 - 1), WFM's +-100 kHz; 100 Hz at
     * the least. */
    const int32_t lim = (m->flags & OWRX_M_HD) ? 100000 : OWRX_RATE / 2 - 1;
    int64_t lo = (int64_t)t->lo + shift, hi = (int64_t)t->hi + shift;
    if (lo > hi) {
        const int64_t x = lo;
        lo = hi;
        hi = x;
    }
    if (lo < -lim) lo = -lim;
    if (hi > lim) hi = lim;
    if (hi - lo < 100) {
        const int64_t mid = (lo + hi) / 2;
        lo = mid - 50;
        hi = mid + 50;
        if (hi > lim) { hi = lim; lo = lim - 100; }
        if (lo < -lim) { lo = -lim; hi = -lim + 100; }
    }
    const int sq = t->sq < -150 ? -150 : t->sq > 0 ? 0 : t->sq;
    wbuf_t w = { out, cap, 0, false };
    const char *sep = "";
    wput(&w, "{\"type\":\"dspcontrol\",\"params\":{");
    if (what & OWRX_P_PASS) {
        wput(&w, "\"low_cut\":%lld,\"high_cut\":%lld", (long long)lo, (long long)hi);
        sep = ",";
    }
    if (what & OWRX_P_OFFSET) {
        wput(&w, "%s\"offset_freq\":%lld", sep, (long long)off);
        sep = ",";
    }
    if (what & OWRX_P_MOD) {
        wput(&w, "%s\"mod\":\"%s\"", sep, m->mod);
        sep = ",";
    }
    if (what & OWRX_P_SQ) {
        wput(&w, "%s\"squelch_level\":%d", sep, sq);
        sep = ",";
    }
    if (what & OWRX_P_PLAIN) wput(&w, "%s\"secondary_mod\":false", sep);
    wput(&w, "}}");
    return w.over ? -1 : (int)w.n;
}

int owrx_start_cmd(char *out, size_t cap)
{
    if (!out || !cap) return -1;
    return done(snprintf(out, cap, "{\"type\":\"dspcontrol\",\"action\":\"start\"}"), cap);
}

int owrx_select_cmd(char *out, size_t cap, const char *id)
{
    if (!out || !cap || !id || !*id) return -1;
    static const char HEAD[] = "{\"type\":\"selectprofile\",\"params\":{\"profile\":\"";
    static const char TAIL[] = "\"}}";
    size_t o = 0;
    if (sizeof HEAD - 1 >= cap) return -1;
    memcpy(out, HEAD, sizeof HEAD - 1);
    o = sizeof HEAD - 1;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
        char e[8];
        size_t k;
        if (*p == '"' || *p == '\\') { e[0] = '\\'; e[1] = (char)*p; k = 2; }
        else if (*p < 0x20)          { k = (size_t)snprintf(e, sizeof e, "\\u%04x", *p); }
        else                         { e[0] = (char)*p; k = 1; }
        if (o + k >= cap) return -1;
        memcpy(out + o, e, k);
        o += k;
    }
    if (o + sizeof TAIL > cap) return -1;
    memcpy(out + o, TAIL, sizeof TAIL);
    return (int)(o + sizeof TAIL - 1);
}
