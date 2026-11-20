/* TailChaser's API: the parsing. See tc_proto.h. */
#include "tc_proto.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"

static const cJSON *get(const cJSON *o, const char *k)
{
    return cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
}

static bool b_of(const cJSON *o, const char *k)
{
    const cJSON *v = get(o, k);
    return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0);
}

static double n_of(const cJSON *o, const char *k, double def)
{
    const cJSON *v = get(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static uint32_t u_of(const cJSON *o, const char *k)
{
    const double d = n_of(o, k, 0);
    return d > 0 && d < 4294967295.0 ? (uint32_t)d : 0;
}

/* Printable ASCII only: the face's fonts have nothing else, and the page
 * puts it in text. */
static void s_of(const cJSON *o, const char *k, char *out, size_t cap)
{
    out[0] = 0;
    const cJSON *v = get(o, k);
    if (!cJSON_IsString(v) || !v->valuestring) return;
    size_t n = 0;
    for (const char *p = v->valuestring; *p && n + 1 < cap; p++)
        if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f) out[n++] = *p;
    out[n] = 0;
}

/* A receiver's squelch and signal: its gate. */
static void rx_of(const cJSON *node, tc_status_t *st)
{
    const cJSON *g = get(node, "gate");
    st->rx_known = true;
    st->cos      = b_of(g, "cos");
    st->open     = b_of(g, "open");
    s_of(g, "state", st->gate, sizeof st->gate);
    st->rssi     = (int)n_of(g, "rssi", -1);
    st->rssi_lo  = (int)n_of(g, "rssi_lo", 0);
    st->rssi_hi  = (int)n_of(g, "rssi_hi", 0);
    st->level_db = (float)n_of(g, "level_db", -120);
}

/* The last opening of a node's squelch: the newest of its history. */
static void local_of(const cJSON *node, tc_status_t *st)
{
    const cJSON *h = get(node, "history");
    const cJSON *o = cJSON_IsArray(h) ? cJSON_GetArrayItem(h, 0) : NULL;
    if (!cJSON_IsObject(o)) return;
    st->local_t  = u_of(o, "t");
    st->local_ms = u_of(o, "ms");
    s_of(o, "pass", st->local_pass, sizeof st->local_pass);
}

bool tc_parse_status(const char *json, size_t len, tc_status_t *st, char *why, size_t why_cap)
{
    memset(st, 0, sizeof *st);
    st->rssi = -1;
    st->level_db = -120;
    if (why_cap) why[0] = 0;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        if (why_cap) snprintf(why, why_cap, "not JSON");
        cJSON_Delete(root);
        return false;
    }
    if (!cJSON_IsString(get(root, "role"))) {
        if (why_cap) snprintf(why, why_cap, "not a TailChaser status");
        cJSON_Delete(root);
        return false;
    }
    s_of(root, "role", st->role, sizeof st->role);
    s_of(root, "version", st->version, sizeof st->version);
    st->uptime_s = u_of(root, "uptime_s");
    st->now      = u_of(root, "now");

    const cJSON *has = get(root, "has");
    st->has_rx        = b_of(has, "rx");
    st->has_tx        = b_of(has, "tx");
    st->has_reflector = b_of(has, "reflector");
    st->has_repeater  = b_of(has, "repeater");
    st->has_rxlink    = b_of(has, "rxlink");

    const cJSON *blk = get(root, "block");
    st->block_rx = b_of(blk, "rx");
    st->block_tx = b_of(blk, "tx");

    s_of(get(root, "ident"), "callsign", st->callsign, sizeof st->callsign);
    if (!st->callsign[0]) s_of(get(root, "link"), "callsign", st->callsign, sizeof st->callsign);

    const cJSON *radio = get(root, "radio");
    if (st->has_tx) {
        st->tx_mhz = n_of(radio, "freq_tx_mhz", 0);
        if (st->tx_mhz <= 0) st->tx_mhz = n_of(radio, "freq_mhz", 0);
        s_of(radio, "ctcss_tx", st->ctcss_tx, sizeof st->ctcss_tx);
    }
    if (st->has_rx) {
        st->rx_mhz = n_of(radio, "freq_mhz", 0);
        s_of(radio, "ctcss_rx", st->ctcss_rx, sizeof st->ctcss_rx);
    }

    const cJSON *tx = get(root, "tx");
    st->tx_keyed    = b_of(tx, "keyed");
    s_of(tx, "why", st->tx_why, sizeof st->tx_why);
    st->tx_keyed_ms = u_of(tx, "keyed_ms");

    /* The receiver: this node's own, else the receive site's, as the
     * transmit site sees it -- its whole status when it answers, else only
     * the RemoteTrx link's squelch. */
    const cJSON *remote = get(root, "remote_receiver");
    if (st->has_rx) {
        rx_of(root, st);
        local_of(root, st);
    } else if (cJSON_IsObject(remote)) {
        rx_of(remote, st);
        local_of(remote, st);
        const cJSON *rr = get(remote, "radio");
        st->rx_mhz = n_of(rr, "freq_mhz", 0);
        s_of(rr, "ctcss_rx", st->ctcss_rx, sizeof st->ctcss_rx);
    } else if (st->has_rxlink) {
        st->rx_known = true;
        st->cos = b_of(get(root, "receiver"), "squelch");
    }
    /* A transmit site of today's TailChaser says, in its own gate, the
     * signal of whichever receiver has the over -- a voter's too -- on a
     * scale of its own (0..100): that one, when it says one. */
    if (!st->has_rx) {
        const cJSON *g = get(root, "gate");
        const int r = (int)n_of(g, "rssi", -1), lo = (int)n_of(g, "rssi_lo", 0), hi = (int)n_of(g, "rssi_hi", 0);
        if (r >= 0 && hi > lo) {
            st->rssi    = r;
            st->rssi_lo = lo;
            st->rssi_hi = hi;
        }
    }
    if (st->has_rxlink) st->rx_site_up = b_of(get(root, "receiver"), "connected");
    st->monitor = b_of(root, "monitor");

    const cJSON *link = get(root, "link");
    st->link_up = b_of(link, "connected");
    st->tg      = u_of(link, "tg");
    st->home_tg = u_of(link, "home_tg");
    s_of(link, "talker", st->talker, sizeof st->talker);
    s_of(link, "host", st->reflector, sizeof st->reflector);

    const cJSON *nh = get(root, "net_history");
    const cJSON *o  = cJSON_IsArray(nh) ? cJSON_GetArrayItem(nh, 0) : NULL;
    if (cJSON_IsObject(o)) {
        s_of(o, "call", st->net_call, sizeof st->net_call);
        st->net_t  = u_of(o, "t");
        st->net_ms = u_of(o, "ms");
    }
    cJSON_Delete(root);
    return true;
}

bool tc_parse_block(const char *json, size_t len, bool *rx, bool *tx)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    const bool ok = cJSON_IsBool(get(root, "rx")) && cJSON_IsBool(get(root, "tx"));
    if (ok) {
        *rx = b_of(root, "rx");
        *tx = b_of(root, "tx");
    }
    cJSON_Delete(root);
    return ok;
}

void tc_parse_error(const char *json, size_t len, char *out, size_t cap)
{
    if (!cap) return;
    out[0] = 0;
    cJSON *root = cJSON_ParseWithLength(json, len);
    s_of(root, "error", out, cap);
    cJSON_Delete(root);
}

/* The step on TailChaser's own scale (its status page, src/status.c's sl()):
 * rssi_lo step 1 (S1), rssi_hi step 13 (S9+40), twelve equal steps between
 * -- S1..S9, then +10 each -- and on to 15 (S9+60). */
static double tc_step(int rssi, int lo, int hi)
{
    return 1.0 + (double)(rssi - lo) * 12.0 / (double)(hi - lo);
}

/* A step in dB over S1: six an S-point to S9, ten after. */
static double step_db(double l)
{
    return l <= 9.0 ? (l - 1.0) * 6.0 : 48.0 + (l - 9.0) * 10.0;
}

int tc_smeter(int rssi, int lo, int hi, char *out, size_t cap)
{
    if (cap) out[0] = 0;
    if (rssi < 0 || hi <= lo) return -1;
    long l = lround(tc_step(rssi, lo, hi));
    if (l < 1) l = 1;
    if (l > 15) l = 15;
    if (cap) {
        if (l <= 9) snprintf(out, cap, "S%ld", l);
        else        snprintf(out, cap, "S9+%ld", (l - 9) * 10);
    }
    return (int)lround(step_db((double)l));
}

bool tc_http_answer(const char *buf, size_t len, int *code, const char **body, size_t *body_len)
{
    if (len < 12 || strncmp(buf, "HTTP/1.", 7) != 0) return false;
    *code = atoi(buf + 9);
    const char *end = NULL;
    for (size_t i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            end = buf + i + 4;
            break;
        }
    if (!end) return false;
    long cl = -1;
    for (const char *p = buf; p < end;) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) break;
        if ((size_t)(eol - p) > 15 && strncasecmp(p, "content-length:", 15) == 0) cl = strtol(p + 15, NULL, 10);
        p = eol + 1;
    }
    const size_t have = len - (size_t)(end - buf);
    if (cl >= 0) {
        if ((size_t)cl > have) return false;
        *body_len = (size_t)cl;
    } else {
        *body_len = have;
    }
    *body = end;
    return true;
}

bool tc_parse_addr(const char *s, tc_addr_t *a)
{
    memset(a, 0, sizeof *a);
    a->port = TC_PORT;
    while (*s && isspace((unsigned char)*s)) s++;
    if (strncasecmp(s, "http://", 7) == 0) s += 7;
    size_t n = 0;
    while (s[n] && s[n] != ':' && s[n] != '/' && !isspace((unsigned char)s[n])) n++;
    if (!n || n >= sizeof a->host) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_') return false;
    }
    memcpy(a->host, s, n);
    a->host[n] = 0;
    if (s[n] == ':') {
        char *e;
        const long p = strtol(s + n + 1, &e, 10);
        if (p < 1 || p > 65535 || (*e && *e != '/' && !isspace((unsigned char)*e))) return false;
        a->port = (uint16_t)p;
    }
    return true;
}

void tc_clean_name(const char *in, char *out, size_t cap)
{
    if (!cap) return;
    size_t o = 0;
    bool gap = false;
    for (; in && *in && o + 1 < cap; in++) {
        const unsigned char c = (unsigned char)*in;
        if (c == ' ' || c == '\t') { gap = o > 0; continue; }
        if (c < 0x21 || c > 0x7e) continue;
        if (gap) {
            if (o + 2 >= cap) break;
            out[o++] = ' ';
            gap = false;
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
}

void tc_line_name(const char *line, char *out, size_t cap)
{
    const char *tab = line ? strchr(line, '\t') : NULL;
    if (!tab) {
        if (cap) out[0] = 0;
        return;
    }
    tc_clean_name(tab + 1, out, cap);
}

void tc_mhz(double mhz, char *out, size_t cap)
{
    if (mhz <= 0) {
        if (cap) out[0] = 0;
        return;
    }
    /* to the 100 Hz: a 12.5 kHz channel (145.2875) keeps its fourth digit */
    const long hz100 = lround(mhz * 10000.0);
    if (hz100 % 10) snprintf(out, cap, "%ld.%04ld", hz100 / 10000, hz100 % 10000);
    else            snprintf(out, cap, "%ld.%03ld", hz100 / 10000, (hz100 % 10000) / 10);
}

float tc_smeter_dbm(int rssi, int lo, int hi)
{
    if (rssi < 0 || hi <= lo) return -127.0f;
    double l = tc_step(rssi, lo, hi);
    if (l < 0) l = 0;                   /* below S1: down to S0, no further */
    if (l > 15) l = 15;                 /* S9+60, the meter's end */
    return (float)(-121.0 + step_db(l));
}

long tc_ws_frame(const uint8_t *p, size_t n, tc_ws_frame_t *f)
{
    if (n < 2) return 0;
    if ((p[0] & 0x70) || (p[1] & 0x80)) return -1;     /* no extension asked; a server never masks */
    const uint8_t op = p[0] & 0x0f;
    if ((op > 2 && op < 8) || op > 10) return -1;
    f->op  = op;
    f->fin = (p[0] & 0x80) != 0;
    uint64_t len = p[1] & 0x7f;
    size_t h = 2;
    if (len == 126) {
        if (n < 4) return 0;
        len = (uint64_t)p[2] << 8 | p[3];
        h = 4;
    } else if (len == 127) {
        if (n < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = len << 8 | p[2 + i];
        h = 10;
    }
    if (op >= 8 && (len > 125 || !f->fin)) return -1;
    if (len > (16u << 20)) return -1;
    if (n < h + len) return 0;
    f->off = h;
    f->len = (size_t)len;
    return (long)(h + len);
}

size_t tc_ws_client_frame(uint8_t op, const uint8_t *payload, size_t n, const uint8_t mask[4], uint8_t *out,
                          size_t cap)
{
    if (n > 125 || cap < 6 + n) return 0;
    out[0] = (uint8_t)(0x80 | (op & 0x0f));
    out[1] = (uint8_t)(0x80 | n);
    memcpy(out + 2, mask, 4);
    for (size_t i = 0; i < n; i++) out[6 + i] = payload[i] ^ mask[i & 3];
    return 6 + n;
}

size_t tc_ws_answer(const char *buf, size_t n, int *code)
{
    *code = 0;
    for (size_t i = 0; i + 3 < n; i++) {
        if (memcmp(buf + i, "\r\n\r\n", 4) != 0) continue;
        if (i >= 12 && !strncmp(buf, "HTTP/1.", 7) && buf[8] == ' ' && isdigit((unsigned char)buf[9]) &&
            isdigit((unsigned char)buf[10]) && isdigit((unsigned char)buf[11]))
            *code = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
        return i + 4;
    }
    return 0;
}

float tc_pcm_peak_db(const int16_t *pcm, size_t n)
{
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        const int v = pcm[i] < 0 ? -(int)pcm[i] : pcm[i];
        if (v > peak) peak = v;
    }
    if (!peak) return -120.0f;
    const float db = 20.0f * log10f((float)peak / 32768.0f);
    return db < -120.0f ? -120.0f : db;
}
