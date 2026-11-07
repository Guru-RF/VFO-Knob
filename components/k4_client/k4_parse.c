/* The Elecraft K4's remote protocol, pure C: see k4_parse.h. */
#include "k4_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A length past this is a corrupt stream, not a frame to skip: skipping it
 * would swallow whatever follows for as long as the length says. */
#define LEN_CORRUPT (1u << 20)

static const uint8_t START[4] = { 0xFE, 0xFD, 0xFC, 0xFB };
static const uint8_t END[4]   = { 0xFB, 0xFC, 0xFD, 0xFE };

enum { S_HUNT = 0, S_LEN, S_BODY, S_SKIP, S_END };

void k4_framer_init(k4_framer_t *f)
{
    memset(f, 0, sizeof *f);
}

void k4_framer_push(k4_framer_t *f, const uint8_t *data, size_t n, k4_frame_cb cb, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = data[i];
        switch (f->state) {
        case S_HUNT:
            /* The marker's four bytes are all different, so a mismatch
             * restarts at 1 for its first byte and at 0 for anything else. */
            if (b == START[f->m]) {
                if (++f->m == 4) { f->state = S_LEN; f->m = 0; f->len = 0; f->got = 0; }
            } else {
                f->m = (b == START[0]) ? 1 : 0;
            }
            break;
        case S_LEN:
            f->len = (f->len << 8) | b;
            if (++f->got == 4) {
                f->got = 0;
                if (f->len > LEN_CORRUPT) {
                    f->bad++;
                    f->state = S_HUNT;
                    f->m = 0;
                } else if (f->len == 0) {
                    f->state = S_END;
                    f->m = 0;
                } else if (f->len > K4_FRAME_MAX) {
                    f->skipped++;
                    f->state = S_SKIP;
                } else {
                    f->state = S_BODY;
                }
            }
            break;
        case S_BODY:
            f->buf[f->got++] = b;
            if (f->got == f->len) { f->state = S_END; f->m = 0; }
            break;
        case S_SKIP:
            if (++f->got == f->len) { f->state = S_END; f->m = 0; }
            break;
        case S_END:
            if (b == END[f->m]) {
                if (++f->m == 4) {
                    const bool kept = f->len <= K4_FRAME_MAX;
                    f->state = S_HUNT;
                    f->m = 0;
                    if (kept && cb) cb(ctx, f->buf, f->len);
                }
            } else {
                /* Not where the length said it would end: this was no frame.
                 * The byte may itself start the next one. */
                f->bad++;
                f->state = S_HUNT;
                f->m = (b == START[0]) ? 1 : 0;
            }
            break;
        }
    }
}

static size_t put_frame_hdr(uint8_t *out, uint32_t len)
{
    memcpy(out, START, 4);
    out[4] = (uint8_t)(len >> 24); out[5] = (uint8_t)(len >> 16);
    out[6] = (uint8_t)(len >> 8);  out[7] = (uint8_t)len;
    return 8;
}

size_t k4_encode_cat(uint8_t *out, size_t cap, const char *cmd)
{
    const size_t cn = strlen(cmd);
    const size_t total = 8 + 3 + cn + 4;
    if (total > cap) return 0;
    size_t o = put_frame_hdr(out, (uint32_t)(3 + cn));
    out[o++] = K4_PAY_CAT; out[o++] = 0; out[o++] = 0;   /* type, version, reserved */
    memcpy(out + o, cmd, cn);
    o += cn;
    memcpy(out + o, END, 4);
    return o + 4;
}

bool k4_audio_parse(const uint8_t *p, size_t len, k4_audio_t *out)
{
    if (len < K4_AUDIO_HDR || p[0] != K4_PAY_AUDIO) return false;
    out->seq     = p[2];
    out->mode    = p[3];
    out->samples = (uint16_t)(p[4] | (p[5] << 8));
    out->data    = p + K4_AUDIO_HDR;
    out->n       = len - K4_AUDIO_HDR;
    return true;
}

size_t k4_encode_audio(uint8_t *out, size_t cap, uint8_t seq, uint8_t mode,
                       uint16_t samples, const uint8_t *data, size_t n)
{
    const size_t total = 8 + K4_AUDIO_HDR + n + 4;
    if (total > cap) return 0;
    size_t o = put_frame_hdr(out, (uint32_t)(K4_AUDIO_HDR + n));
    out[o++] = K4_PAY_AUDIO;
    out[o++] = 1;                       /* version */
    out[o++] = seq;
    out[o++] = mode;
    out[o++] = (uint8_t)(samples & 0xFF);
    out[o++] = (uint8_t)(samples >> 8);
    out[o++] = 0;                       /* 12 kHz, the only rate there is */
    memcpy(out + o, data, n);
    o += n;
    memcpy(out + o, END, 4);
    return o + 4;
}

bool k4_cat_next(const uint8_t *p, size_t len, size_t *pos, char *cmd, size_t cap)
{
    if (len <= 3 || p[0] != K4_PAY_CAT || cap == 0) return false;
    size_t i = *pos ? *pos : 3;
    /* A stray ';' between commands is nothing. */
    while (i < len && p[i] == ';') i++;
    if (i >= len) { *pos = len; return false; }
    size_t n = 0;
    while (i < len && p[i] != ';') {
        if (n + 1 < cap) cmd[n++] = (char)p[i];
        i++;
    }
    cmd[n] = 0;
    *pos = i < len ? i + 1 : len;
    /* Whatever is left, without its ';', is not a command yet. */
    if (i >= len) { *pos = len; return false; }
    return true;
}

/* ---------------------------------------------------------------- status */

const char *k4_mode_name(char md)
{
    switch (md) {
    case '1': return "lsb";
    case '2': return "usb";
    case '3': return "cw";
    case '4': return "fm";
    case '5': return "am";
    case '6': return "digu";     /* DATA: on the upper sideband */
    case '7': return "cwr";
    case '9': return "digl";     /* DATA-R: the lower */
    default:  return NULL;
    }
}

char k4_mode_digit(const char *name)
{
    static const struct { const char *n; char d; } T[] = {
        { "lsb", '1' }, { "usb", '2' }, { "cw", '3' }, { "fm", '4' }, { "am", '5' },
        { "digu", '6' }, { "cwr", '7' }, { "digl", '9' },
    };
    for (size_t i = 0; name && i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(name, T[i].n)) return T[i].d;
    return 0;
}

const char *k4_agc_name(uint8_t gt)
{
    switch (gt) {
    case 0: return "off";
    case 1: return "slow";
    case 2: return "fast";
    default: return NULL;
    }
}

int k4_agc_digit(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "off"))  return 0;
    if (!strcmp(name, "slow")) return 1;
    if (!strcmp(name, "fast")) return 2;
    return -1;
}

/* n digits of s, or false: strtol would take a sign, a space, a hex prefix. */
static bool digits(const char *s, size_t n, long *v)
{
    long r = 0;
    /* 18 digits fit a long; more is not a number this protocol sends. */
    if (n > 18) return false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        r = r * 10 + (s[i] - '0');
    }
    *v = r;
    return true;
}

/* [+-]ddd... to the end of the command. */
static bool signed_num(const char *s, long *v)
{
    int neg = 0;
    if (*s == '+' || *s == '-') neg = (*s++ == '-');
    const size_t n = strlen(s);
    if (n == 0 || n > 9 || !digits(s, n, v)) return false;
    if (neg) *v = -*v;
    return true;
}

static bool starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

static void set_mode(k4_status_t *st, char md)
{
    const char *m = k4_mode_name(md);
    if (!m) return;
    strncpy(st->mode, m, sizeof st->mode - 1);
    st->mode[sizeof st->mode - 1] = 0;
    st->have_mode = true;
}

/* IF -- the fixed-width status line. After "IF": the frequency at 0-10, the
 * RIT/XIT offset's sign at 16 and its four digits after, RIT on at 21, XIT at
 * 22, transmitting at 26, the mode at 27. */
static bool apply_if(k4_status_t *st, const char *a)
{
    if (strlen(a) < 31) return false;
    long f, rit;
    if (!digits(a, 11, &f)) return false;
    st->f_hz = f; st->have_f = true;
    if (digits(a + 17, 4, &rit) && (a[16] == '+' || a[16] == '-')) {
        st->rit_hz = (int32_t)(a[16] == '-' ? -rit : rit);
        st->rit_on = a[21] == '1';
        st->have_rit = true;
    }
    st->tx = a[26] == '1'; st->have_tx = true;
    set_mode(st, a[27]);
    return true;
}

bool k4_apply(k4_status_t *st, const char *c)
{
    long v;
    /* Longest prefix first, and anything with a '$' is the sub receiver's,
     * which this knob does not work. */
    if (starts(c, "PONG")) { st->pong = true; return true; }
    if (starts(c, "SMH$") || starts(c, "SM$") || starts(c, "MD$") || starts(c, "BW$") ||
        starts(c, "GT$")  || starts(c, "RG$") || starts(c, "RO$") || starts(c, "PC$") ||
        starts(c, "IS$") || starts(c, "NB$") || starts(c, "NR$") || starts(c, "NA$") ||
        starts(c, "AP$"))
        return false;
    if (starts(c, "SMH")) {
        if (!signed_num(c + 3, &v)) return false;
        st->smeter_dbm = (float)v; st->have_smeter = true;
        return true;
    }
    if (starts(c, "SM")) {
        if (!digits(c + 2, strlen(c + 2), &v) || !c[2]) return false;
        st->bars = (int)v; st->have_bars = true;
        return true;
    }
    if (starts(c, "IF")) return apply_if(st, c + 2);
    if (starts(c, "FA")) {
        if (!digits(c + 2, strlen(c + 2), &v) || !c[2]) return false;
        st->f_hz = v; st->have_f = true;
        return true;
    }
    if (starts(c, "MD")) {
        if (!c[2] || !k4_mode_name(c[2])) return false;
        set_mode(st, c[2]);
        return true;
    }
    if (starts(c, "BW")) {
        if (!c[2] || !digits(c + 2, strlen(c + 2), &v)) return false;
        st->bw_hz = (int32_t)(v * 10); st->have_bw = true;
        return true;
    }
    if (starts(c, "TM")) {
        long alc, cmp, fwd, swr;
        if (strlen(c + 2) < 12 || !digits(c + 2, 3, &alc) || !digits(c + 5, 3, &cmp) ||
            !digits(c + 8, 3, &fwd) || !digits(c + 11, 3, &swr))
            return false;
        st->alc = (float)alc;
        st->cmp_db = (int)cmp;
        /* Watts in the QRO range, tenths of a watt below it: the range is the
         * last PC's. Before one is heard, the tenths are the safe reading. */
        st->fwd_w = (st->have_pc && st->pc_range == 'H') ? (float)fwd : (float)fwd / 10.0f;
        st->swr = (float)swr / 10.0f;
        st->have_tm = true;
        return true;
    }
    if (starts(c, "TQ")) {
        if (c[2] != '0' && c[2] != '1') return false;
        st->tx = c[2] == '1'; st->have_tx = true;
        return true;
    }
    if (!strcmp(c, "RX")) { st->tx = false; st->have_tx = true; return true; }
    if (starts(c, "GT")) {
        if (!c[2] || !digits(c + 2, strlen(c + 2), &v) || v > 9) return false;
        st->agc = (uint8_t)v; st->have_agc = true;
        return true;
    }
    if (starts(c, "RG")) {
        if (!signed_num(c + 2, &v)) return false;
        st->rg_db = (int)(v > 0 ? -v : v); st->have_rg = true;
        return true;
    }
    if (starts(c, "RO")) {
        if (!signed_num(c + 2, &v)) return false;
        st->rit_hz = (int32_t)v; st->have_rit = true;
        return true;
    }
    if (starts(c, "RT")) {
        if (c[2] != '0' && c[2] != '1') return false;
        st->rit_on = c[2] == '1';
        st->have_rit = true;                    /* the offset stands; what it means moved */
        return true;
    }
    if (starts(c, "PC")) {
        size_t n = strlen(c + 2);
        char range = 'L';                       /* L if it says none */
        if (n && (c[2 + n - 1] == 'L' || c[2 + n - 1] == 'H' || c[2 + n - 1] == 'X')) {
            range = c[2 + n - 1];
            n--;
        }
        if (!n || !digits(c + 2, n, &v)) return false;
        st->pc = (uint16_t)v; st->pc_range = range; st->have_pc = true;
        return true;
    }
    if (starts(c, "IS")) {
        /* "IS 0060": the K4 answers in the K3's form, a space first (its
         * reference gives both), in the same 10 Hz units. */
        const char *a = c + 2;
        while (*a == ' ') a++;
        if (!*a || !digits(a, strlen(a), &v)) return false;
        st->is_hz = (int32_t)(v * 10); st->have_is = true;
        return true;
    }
    if (starts(c, "NB")) {                     /* NBnnmf: level, on, filter */
        /* ...or the reference's alternate form, NBm: on or off alone. The K4
         * answers a GET with it (seen on a real one). */
        if (strlen(c + 2) == 1 && (c[2] == '0' || c[2] == '1')) {
            st->nb_on = c[2] == '1'; st->have_nb = true;
            return true;
        }
        if (strlen(c + 2) < 3 || !digits(c + 2, 2, &v) || (c[4] != '0' && c[4] != '1')) return false;
        st->nb_on = c[4] == '1'; st->have_nb = true;
        return true;
    }
    if (starts(c, "NR")) {                     /* NRnnm: level, mode */
        if (strlen(c + 2) < 3 || !digits(c + 2, 2, &v) || (c[4] != '0' && c[4] != '1' && c[4] != '2'))
            return false;
        st->nr_level = (uint8_t)(v > 10 ? 10 : v);
        st->nr_on = c[4] == '1'; st->have_nr = true;
        return true;
    }
    if (starts(c, "NA")) {
        if (c[2] != '0' && c[2] != '1') return false;
        st->na_on = c[2] == '1'; st->have_na = true;
        return true;
    }
    if (starts(c, "AP")) {                     /* APmb: on, width */
        if (strlen(c + 2) < 2 || (c[2] != '0' && c[2] != '1') || c[3] < '0' || c[3] > '2') return false;
        st->ap_on = c[2] == '1'; st->ap_width = (uint8_t)(c[3] - '0'); st->have_ap = true;
        return true;
    }
    if (starts(c, "ID")) {
        strncpy(st->id, c + 2, sizeof st->id - 1);
        st->id[sizeof st->id - 1] = 0;
        st->have_id = true;
        return true;
    }
    if (starts(c, "ER") && strchr(c, ':')) {
        /* ERnn:text -- an error, where a bare ERn is only the mode's echo. */
        const char *colon = strchr(c, ':');
        const size_t dn = (size_t)(colon - (c + 2));
        if (dn && dn <= 4 && digits(c + 2, dn, &v)) {
            st->err = (int)v;
            strncpy(st->err_text, colon + 1, sizeof st->err_text - 1);
            st->err_text[sizeof st->err_text - 1] = 0;
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------- commands */

size_t k4_cmd_freq(char *out, size_t cap, int64_t hz)
{
    if (hz < 0) hz = 0;
    if (hz > 99999999999LL) hz = 99999999999LL;
    const int n = snprintf(out, cap, "FA%011lld;", (long long)hz);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t k4_cmd_mode(char *out, size_t cap, char md)
{
    if (!k4_mode_name(md)) return 0;
    const int n = snprintf(out, cap, "MD%c;", md);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t k4_cmd_bw(char *out, size_t cap, int32_t hz)
{
    if (hz < 0) hz = 0;
    if (hz > 99990) hz = 99990;
    const int n = snprintf(out, cap, "BW%04d;", (int)(hz / 10));
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t k4_cmd_rit(char *out, size_t cap, int32_t hz)
{
    if (hz < -9999) hz = -9999;
    if (hz > 9999) hz = 9999;
    const int n = snprintf(out, cap, "RO%c%04d;", hz < 0 ? '-' : '+', (int)(hz < 0 ? -hz : hz));
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t k4_cmd_rg(char *out, size_t cap, int db)
{
    if (db > 0) db = -db;
    if (db < -60) db = -60;
    const int n = snprintf(out, cap, "RG-%02d;", -db);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

void k4_hex(char *out, const uint8_t *d, size_t n)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = H[d[i] >> 4];
        out[2 * i + 1] = H[d[i] & 15];
    }
    out[2 * n] = 0;
}
