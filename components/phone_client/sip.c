/* A small SIP user agent. See sip.h. */
#include "sip.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_rom_md5.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "sip";

#define T1_MS          500          /* RFC 3261's round-trip estimate        */
#define T2_MS          4000         /* its cap on a retransmission interval  */
#define TIMEOUT_MS     32000        /* 64*T1: a transaction gives up         */
#define RING_MS        120000       /* a call left ringing, either way: over */
#define EXPIRES_S      300          /* the registration asked for            */
#define KEEPALIVE_MS   25000        /* a CRLF, for the router's mapping      */
#define RETRY_MS       30000        /* registration again, after a failure   */
#define ENDED_MS       3000         /* "ended" on the face, then idle        */
#define UA             "VFO-Knob"
#define MSG_CAP        2048
#define ALLOW          "INVITE, ACK, CANCEL, BYE, OPTIONS, INFO, UPDATE, NOTIFY"

/* ------------------------------------------------------------ parsing */

#define MAXH 48
typedef struct {
    bool  req;
    int   code;                      /* a response's */
    char *method, *ruri;             /* a request's */
    int   nh;
    char *name[MAXH], *val[MAXH];
    char *body;
    size_t blen;
} msg_t;

/* The compact forms (RFC 3261 7.3.3). */
static const char *canon(const char *n)
{
    if (strlen(n) != 1) return n;
    switch (tolower((unsigned char)n[0])) {
    case 'i': return "Call-ID";   case 'm': return "Contact";
    case 'l': return "Content-Length"; case 'c': return "Content-Type";
    case 'f': return "From";      case 't': return "To";
    case 'v': return "Via";       case 'k': return "Supported";
    default:  return n;
    }
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

/* In place: the buffer is cut into the message's lines. */
static bool parse(char *b, size_t n, msg_t *m)
{
    memset(m, 0, sizeof *m);
    b[n] = 0;
    char *hend = strstr(b, "\r\n\r\n");
    if (!hend) return false;
    m->body = hend + 4;
    m->blen = n - (size_t)(m->body - b);
    *hend = 0;
    char *line = b, *next;
    bool first = true;
    for (; line; line = next) {
        next = strstr(line, "\r\n");
        if (next) { *next = 0; next += 2; }
        if (first) {
            first = false;
            if (!strncmp(line, "SIP/2.0 ", 8)) {
                m->code = atoi(line + 8);
                if (m->code < 100) return false;
            } else {
                m->req = true;
                m->method = line;
                char *sp = strchr(line, ' ');
                if (!sp) return false;
                *sp = 0;
                m->ruri = sp + 1;
                sp = strchr(m->ruri, ' ');
                if (sp) *sp = 0;
            }
            continue;
        }
        char *colon = strchr(line, ':');
        if (!colon || m->nh >= MAXH) continue;
        *colon = 0;
        m->name[m->nh] = trim(line);
        m->val[m->nh] = trim(colon + 1);
        m->nh++;
    }
    const char *cl = NULL;
    for (int i = 0; i < m->nh; i++)
        if (!strcasecmp(canon(m->name[i]), "Content-Length")) cl = m->val[i];
    if (cl) {
        const size_t len = (size_t)atoi(cl);
        if (len < m->blen) m->blen = len;
    }
    m->body[m->blen] = 0;
    return true;
}

static const char *hdr(const msg_t *m, const char *name)
{
    for (int i = 0; i < m->nh; i++)
        if (!strcasecmp(canon(m->name[i]), name)) return m->val[i];
    return NULL;
}

/* Every value of a header, list headers (Via, Record-Route) split at their
 * commas -- those outside quotes and angle brackets. Copies into `store`. */
static int hdr_all(const msg_t *m, const char *name, char out[][192], int max)
{
    int k = 0;
    for (int i = 0; i < m->nh && k < max; i++) {
        if (strcasecmp(canon(m->name[i]), name)) continue;
        const char *v = m->val[i], *s = v;
        int depth = 0;
        bool q = false;
        for (const char *p = v;; p++) {
            if (*p == '"') q = !q;
            else if (!q && *p == '<') depth++;
            else if (!q && *p == '>') depth--;
            if (*p == 0 || (*p == ',' && !q && depth == 0)) {
                size_t len = (size_t)(p - s);
                while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
                while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
                if (len && k < max) {
                    if (len > 191) len = 191;
                    memcpy(out[k], s, len);
                    out[k][len] = 0;
                    k++;
                }
                if (*p == 0) break;
                s = p + 1;
            }
        }
    }
    return k;
}

/* A header parameter's value: ;name=value (quotes kept off). */
static bool param(const char *h, const char *name, char *out, size_t cap)
{
    if (!h) return false;
    const size_t nl = strlen(name);
    bool q = false;
    for (const char *p = h; *p; p++) {
        if (*p == '"') { q = !q; continue; }
        if (q || (*p != ';' && *p != ',' && *p != ' ')) continue;
        const char *s = p + 1;
        while (*s == ' ') s++;
        if (strncasecmp(s, name, nl)) continue;
        s += nl;
        while (*s == ' ') s++;
        if (*s != '=') {
            if (*s == ';' || *s == 0 || *s == ',' || *s == '>') { if (cap) out[0] = 0; return true; }
            continue;
        }
        s++;
        while (*s == ' ') s++;
        size_t n = 0;
        if (*s == '"') {
            s++;
            while (s[n] && s[n] != '"') n++;
        } else {
            while (s[n] && !strchr(";,> \t", s[n])) n++;
        }
        if (n >= cap) n = cap - 1;
        memcpy(out, s, n);
        out[n] = 0;
        return true;
    }
    return false;
}

/* The URI in a name-addr ("Name" <sip:...>;tag=x) or a plain addr-spec. */
static void uri_of(const char *h, char *out, size_t cap)
{
    out[0] = 0;
    if (!h) return;
    const char *lt = strchr(h, '<');
    if (lt) {
        const char *gt = strchr(lt, '>');
        size_t n = gt ? (size_t)(gt - lt - 1) : strlen(lt + 1);
        if (n >= cap) n = cap - 1;
        memcpy(out, lt + 1, n);
        out[n] = 0;
        return;
    }
    size_t n = strcspn(h, ";");
    if (n >= cap) n = cap - 1;
    memcpy(out, h, n);
    out[n] = 0;
}

/* Who a From says it is: its display name, else its user part. */
static void who_of(const char *h, char *name, size_t nc, char *num, size_t uc)
{
    name[0] = num[0] = 0;
    if (!h) return;
    const char *q = strchr(h, '"');
    const char *lt = strchr(h, '<');
    if (q && (!lt || q < lt)) {
        const char *e = strchr(q + 1, '"');
        if (e) {
            size_t n = (size_t)(e - q - 1);
            if (n >= nc) n = nc - 1;
            memcpy(name, q + 1, n);
            name[n] = 0;
        }
    } else if (lt) {
        const char *s = h;
        while (*s == ' ') s++;
        size_t n = (size_t)(lt - s);
        while (n && s[n - 1] == ' ') n--;
        if (n >= nc) n = nc - 1;
        memcpy(name, s, n);
        name[n] = 0;
    }
    char uri[160];
    uri_of(h, uri, sizeof uri);
    const char *u = strchr(uri, ':');
    u = u ? u + 1 : uri;
    size_t n = strcspn(u, "@;>");
    if (n >= uc) n = uc - 1;
    memcpy(num, u, n);
    num[n] = 0;
    if (!name[0]) strlcpy(name, num, nc);
}

/* ------------------------------------------------------------ digest */

typedef struct {
    bool have, proxy;                /* a challenge, and from a proxy (407) */
    char realm[64], nonce[128], opaque[96], qop[16];
    uint32_t nc;
} auth_t;

static void md5_hex(const char *s, char out[33])
{
    md5_context_t c;
    uint8_t d[16];
    esp_rom_md5_init(&c);
    esp_rom_md5_update(&c, s, strlen(s));
    esp_rom_md5_final(d, &c);
    for (int i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static bool challenge(const msg_t *m, auth_t *a)
{
    const bool proxy = m->code == 407;
    const char *h = hdr(m, proxy ? "Proxy-Authenticate" : "WWW-Authenticate");
    if (!h || strncasecmp(h, "Digest", 6)) return false;
    char nonce[128] = "";
    param(h + 6, "nonce", nonce, sizeof nonce);
    /* The same nonce refused again, and not stale: the password is wrong. */
    char stale[8] = "";
    param(h + 6, "stale", stale, sizeof stale);
    const bool again = a->have && !strcmp(a->nonce, nonce) && strcasecmp(stale, "true");
    a->have = true;
    a->proxy = proxy;
    strlcpy(a->nonce, nonce, sizeof a->nonce);
    a->realm[0] = a->opaque[0] = a->qop[0] = 0;
    param(h + 6, "realm", a->realm, sizeof a->realm);
    param(h + 6, "opaque", a->opaque, sizeof a->opaque);
    char qop[32] = "";
    if (param(h + 6, "qop", qop, sizeof qop) && strstr(qop, "auth")) strlcpy(a->qop, "auth", sizeof a->qop);
    a->nc = 0;
    return !again;
}

/* The Authorization (or Proxy-Authorization) line for a request. */
static void authorize(auth_t *a, const sip_account_t *acc, const char *method, const char *uri,
                      char *out, size_t cap)
{
    out[0] = 0;
    if (!a->have) return;
    char ha1[33], ha2[33], resp[33], buf[512], cnonce[17], nc[9];
    snprintf(buf, sizeof buf, "%s:%s:%s", acc->user, a->realm, acc->pass);
    md5_hex(buf, ha1);
    snprintf(buf, sizeof buf, "%s:%s", method, uri);
    md5_hex(buf, ha2);
    snprintf(cnonce, sizeof cnonce, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    snprintf(nc, sizeof nc, "%08lx", (unsigned long)++a->nc);
    if (a->qop[0]) snprintf(buf, sizeof buf, "%s:%s:%s:%s:%s:%s", ha1, a->nonce, nc, cnonce, a->qop, ha2);
    else           snprintf(buf, sizeof buf, "%s:%s:%s", ha1, a->nonce, ha2);
    md5_hex(buf, resp);
    int n = snprintf(out, cap, "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                     "response=\"%s\", algorithm=MD5",
                     a->proxy ? "Proxy-Authorization" : "Authorization",
                     acc->user, a->realm, a->nonce, uri, resp);
    if (a->qop[0] && n > 0 && (size_t)n < cap)
        n += snprintf(out + n, cap - (size_t)n, ", cnonce=\"%s\", qop=%s, nc=%s", cnonce, a->qop, nc);
    if (a->opaque[0] && n > 0 && (size_t)n < cap)
        n += snprintf(out + n, cap - (size_t)n, ", opaque=\"%s\"", a->opaque);
    if (n > 0 && (size_t)n + 2 < cap) strlcat(out, "\r\n", cap);
}

/* ------------------------------------------------------------ state */

typedef struct {                     /* a request we retransmit */
    bool     active;
    char    *buf;
    size_t   len;
    char     branch[32];
    uint32_t cseq;
    uint64_t t_next, t_start;
    uint32_t interval;
} tx_t;

typedef struct {
    int     codec;                   /* the chosen: 9 G.722, 8 PCMA, 0 PCMU, -1 none */
    int     dtmf;                    /* telephone-event's PT, -1 none */
    uint32_t ip;
    uint16_t port;
    bool    hold;                    /* sendonly or inactive from there */
} sdp_t;

EXT_RAM_BSS_ATTR static char s_reg_buf[MSG_CAP], s_inv_buf[MSG_CAP], s_bye_buf[MSG_CAP],
                             s_resp_buf[MSG_CAP], s_rx[MSG_CAP + 1], s_scratch[MSG_CAP];

static struct {
    bool          on;
    sip_account_t acc;
    sip_events_t  ev;
    int           fd;
    struct sockaddr_in srv;
    bool          srv_ok;
    uint64_t      t_resolved;
    char          lip[16];           /* our address on the LAN */
    uint16_t      lport;
    char          pip[16];           /* ...and as the far end sees it */
    uint16_t      pport;
    uint16_t      rtp_port;

    sip_reg_t     reg;
    char          reg_why[32];
    char          reg_callid[48], reg_tag[16];
    uint32_t      reg_cseq;
    tx_t          reg_tx;
    auth_t        reg_auth;
    uint64_t      t_reg_next, t_keepalive;
    char          reg_contact[64];   /* the Contact registered */

    sip_call_t    call;
    bool          outgoing;
    char          peer[32], peer_num[24], why[24];
    char          callid[48], ltag[16], rtag[64];
    char          from_hdr[224], to_hdr[224]; /* ours and theirs, tags included */
    char          ruri[128];         /* the INVITE's Request-URI */
    char          rtarget[160];      /* the far end's Contact */
    char          route[4][192];
    int           nroute;
    uint32_t      lcseq;             /* our next CSeq in the dialog */
    tx_t          inv_tx;            /* our INVITE */
    auth_t        inv_auth;
    bool          inv_prov, cancel_due, cancelled;
    uint64_t      t_call;            /* placed, or ringing since */
    tx_t          bye_tx;            /* our BYE or CANCEL */
    auth_t        bye_auth;
    char          bye_method[8];
    /* A call taken: the INVITE's headers, for each response to it. */
    char          in_hdrs[1024];
    uint32_t      in_cseq;
    tx_t          resp_tx;           /* our final response, until its ACK */
    sdp_t         offer;             /* theirs, being answered */
    sip_media_t   media;
    uint64_t      t_ended;
    uint32_t      sdp_ver;
} S = { .fd = -1 };

static uint64_t s_now;

static void rand_hex(char *out, size_t n)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i + 1 < n; i++) out[i] = H[esp_random() & 15];
    out[n - 1] = 0;
}

static void new_branch(char out[32])
{
    strcpy(out, "z9hG4bK");
    rand_hex(out + 7, 17);
}

static void send_raw(const char *b, size_t n)
{
    if (S.fd < 0 || !S.srv_ok) return;
    sendto(S.fd, b, n, 0, (const struct sockaddr *)&S.srv, sizeof S.srv);
}

static void tx_go(tx_t *t, char *buf, size_t len)
{
    t->active = true;
    t->buf = buf;
    t->len = len;
    t->t_start = t->t_next = s_now;
    t->interval = T1_MS;
    send_raw(buf, len);
    t->t_next = s_now + T1_MS;
}

/* Retransmit what has not been answered; false once it has given up. */
static bool tx_poll(tx_t *t, uint32_t cap_ms)
{
    if (!t->active) return true;
    if (s_now - t->t_start >= TIMEOUT_MS) {
        t->active = false;
        return false;
    }
    if (s_now >= t->t_next) {
        send_raw(t->buf, t->len);
        t->interval = t->interval * 2 > cap_ms ? cap_ms : t->interval * 2;
        t->t_next = s_now + t->interval;
    }
    return true;
}

static const char *cip(void) { return S.pip[0] ? S.pip : S.lip; }
static uint16_t cport(void)  { return S.pport ? S.pport : S.lport; }

/* Our address as the registrar saw it, from a response's Via. */
static void learn_public(const msg_t *m)
{
    const char *v = hdr(m, "Via");
    char ip[48] = "", port[8] = "";
    if (!param(v, "received", ip, sizeof ip) || !ip[0]) return;
    param(v, "rport", port, sizeof port);
    const uint16_t p = (uint16_t)atoi(port);
    if (strlen(ip) < sizeof S.pip && (strcmp(ip, S.pip) || (p && p != S.pport))) {
        strlcpy(S.pip, ip, sizeof S.pip);
        if (p) S.pport = p;
        ESP_LOGI(TAG, "seen as %s:%u from outside", S.pip, (unsigned)cport());
    }
}

static int b_printf(char *b, size_t cap, size_t *n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int b_printf(char *b, size_t cap, size_t *n, const char *fmt, ...)
{
    if (*n >= cap) return 0;
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(b + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (k > 0) *n = *n + (size_t)k < cap ? *n + (size_t)k : cap;
    return k;
}

/* ------------------------------------------------------------ SDP */

static size_t sdp_build(char *b, size_t cap, int pt, int dtmf)
{
    size_t n = 0;
    S.sdp_ver++;
    b_printf(b, cap, &n, "v=0\r\no=- %lu %lu IN IP4 %s\r\ns=" UA "\r\nc=IN IP4 %s\r\nt=0 0\r\n",
             (unsigned long)(S.sdp_ver * 7919u), (unsigned long)S.sdp_ver, cip(), cip());
    /* G.722 first: HD voice, 16 kHz as the knob's own audio is, so nothing
     * is resampled. Its RTP clock is 8000 all the same (RFC 3551). */
    const char *name = pt == 9 ? "G722" : pt == 8 ? "PCMA" : "PCMU";
    if (pt < 0) {                    /* an offer: all we have */
        b_printf(b, cap, &n, "m=audio %u RTP/AVP 9 8 0 101\r\n"
                 "a=rtpmap:9 G722/8000\r\na=rtpmap:8 PCMA/8000\r\na=rtpmap:0 PCMU/8000\r\n"
                 "a=rtpmap:101 telephone-event/8000\r\na=fmtp:101 0-16\r\n",
                 (unsigned)S.rtp_port);
    } else if (dtmf >= 0) {
        b_printf(b, cap, &n, "m=audio %u RTP/AVP %d %d\r\na=rtpmap:%d %s/8000\r\n"
                 "a=rtpmap:%d telephone-event/8000\r\na=fmtp:%d 0-16\r\n",
                 (unsigned)S.rtp_port, pt, dtmf, pt, name, dtmf, dtmf);
    } else {
        b_printf(b, cap, &n, "m=audio %u RTP/AVP %d\r\na=rtpmap:%d %s/8000\r\n",
                 (unsigned)S.rtp_port, pt, pt, name);
    }
    /* No silence suppression, said outright (RFC 3108): audio all through,
     * every 20 ms, as we send it -- no comfort noise (13) is offered either. */
    b_printf(b, cap, &n, "a=ptime:20\r\na=silenceSupp:off - - - -\r\na=sendrecv\r\n");
    return n;
}

/* Theirs: where, and which of the codecs we have -- G.722 wherever they list
 * it, else the first of PCMA and PCMU in their order. In an answer the first
 * listed is the one chosen. Their list is logged: whether a provider carries
 * G.722 shows there. */
static bool sdp_parse(const char *body, sdp_t *o, bool answer)
{
    memset(o, 0, sizeof *o);
    o->codec = o->dtmf = -1;
    if (!body || !*body) return false;
    char ip[48] = "";
    bool in_audio = false, have_m = false;
    int fmts[16], nf = 0;
    const char *l = body;
    while (*l) {
        const char *e = strpbrk(l, "\r\n");
        const size_t n = e ? (size_t)(e - l) : strlen(l);
        char line[192];
        const size_t k = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, l, k);
        line[k] = 0;
        if (!strncmp(line, "m=", 2)) {
            in_audio = !strncmp(line, "m=audio ", 8);
            if (in_audio && !have_m) {
                have_m = true;
                o->port = (uint16_t)atoi(line + 8);
                const char *p = strstr(line, "RTP/AVP");
                p = p ? p + 7 : line + strlen(line);
                while (*p && nf < 16) {
                    while (*p == ' ') p++;
                    if (!*p) break;
                    fmts[nf++] = atoi(p);
                    while (*p && *p != ' ') p++;
                }
            }
        } else if (!strncmp(line, "c=IN IP4 ", 9) && (in_audio || !have_m)) {
            strlcpy(ip, line + 9, sizeof ip);
            ip[strcspn(ip, " /")] = 0;
        } else if (in_audio && !strncmp(line, "a=rtpmap:", 9)) {
            const int pt = atoi(line + 9);
            if (strcasestr(line, "telephone-event/8000")) o->dtmf = pt;
        } else if (in_audio && (!strcmp(line, "a=sendonly") || !strcmp(line, "a=inactive"))) {
            o->hold = true;
        }
        l += n;
        while (*l == '\r' || *l == '\n') l++;
    }
    {
        char list[64];
        size_t ln = 0;
        list[0] = 0;
        for (int i = 0; i < nf && ln < sizeof list - 5; i++)
            ln += (size_t)snprintf(list + ln, sizeof list - ln, " %d", fmts[i]);
        ESP_LOGI(TAG, "their %s's codecs:%s", answer ? "answer" : "offer", list);
    }
    for (int i = 0; i < nf && o->codec < 0; i++)
        if (fmts[i] == 9) o->codec = 9;
    for (int i = 0; i < nf && o->codec < 0; i++)
        if (fmts[i] == 8 || fmts[i] == 0) o->codec = fmts[i];
    if (!answer && o->codec < 0) return false;
    if (answer && nf && fmts[0] != 9 && fmts[0] != 8 && fmts[0] != 0) o->codec = -1;
    if (answer && nf && o->codec >= 0) o->codec = fmts[0];   /* what they chose */
    struct in_addr a;
    if (!ip[0] || !inet_aton(ip, &a)) return false;
    o->ip = a.s_addr;
    if (!strcmp(ip, "0.0.0.0")) o->hold = true;
    return o->codec >= 0 && o->port;
}

static void media_from(const sdp_t *d)
{
    S.media.on = true;
    S.media.ip = d->ip;
    S.media.port = d->port;
    S.media.pt = (uint8_t)d->codec;
    S.media.dtmf_pt = (int16_t)d->dtmf;
    if (S.ev.on_media) S.ev.on_media(&S.media);
}

static void media_off(void)
{
    if (!S.media.on) return;
    S.media.on = false;
    if (S.ev.on_media) S.ev.on_media(&S.media);
}

/* ------------------------------------------------------------ the call */

static void call_state(sip_call_t c, const char *why)
{
    S.call = c;
    strlcpy(S.why, why ? why : "", sizeof S.why);
    if (c == SIP_CALL_ENDED) {
        S.t_ended = s_now;
        media_off();
        /* Not our final response to a call taken: that goes on until its
         * ACK, or the caller would ring again with every retransmission. */
        S.inv_tx.active = false;
    }
    if (S.ev.on_call) S.ev.on_call(c, S.peer, S.peer_num, S.why);
}

static const char *why_of(int code)
{
    switch (code) {
    case 486: case 600: return "busy";
    case 603: return "declined";
    case 404: case 484: case 604: return "no such number";
    case 480: return "not available";
    case 408: return "no answer";
    case 487: return "";
    case 401: case 403: case 407: return "refused";
    default:  return code >= 500 ? "network trouble" : "failed";
    }
}

/* Why a call ended, by the network's own word where it gives one -- a
 * Reason header's Q.850 cause (RFC 3326) -- else by the status code. A call
 * nobody answers can end in a 503, the cause saying what happened. */
static const char *why_of_msg(const msg_t *m)
{
    const char *r = hdr(m, "Reason");
    if (r && !strncasecmp(r, "Q.850", 5)) {
        for (const char *p = r; *p; p++) {
            if (strncasecmp(p, "cause=", 6)) continue;
            switch (atoi(p + 6)) {
            case 16: case 31:            return "";                 /* normal */
            case 17:                     return "busy";
            case 18: case 19: case 102: return "no answer";
            case 20:                     return "not available";
            case 21:                     return "declined";
            case 1: case 22: case 28:    return "no such number";
            default:                     break;
            }
            break;
        }
    }
    return why_of(m->code);
}

/* In-dialog request headers: Via, Route set, From, To, Call-ID, CSeq. */
static size_t dialog_request(char *b, size_t cap, const char *method, uint32_t cseq, const char *branch,
                             const char *auth_line)
{
    size_t n = 0;
    const char *target = S.rtarget[0] ? S.rtarget : S.ruri;
    b_printf(b, cap, &n, "%s %s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%u;rport;branch=%s\r\nMax-Forwards: 70\r\n",
             method, target, S.lip, (unsigned)S.lport, branch);
    for (int i = 0; i < S.nroute; i++) b_printf(b, cap, &n, "Route: %s\r\n", S.route[i]);
    b_printf(b, cap, &n, "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %lu %s\r\n%sUser-Agent: " UA "\r\n",
             S.from_hdr, S.to_hdr, S.callid, (unsigned long)cseq, method, auth_line ? auth_line : "");
    return n;
}

static void send_ack(uint32_t cseq, const char *branch_if_non2xx, const msg_t *resp)
{
    size_t n = 0;
    if (branch_if_non2xx) {
        /* For a failure, ACK goes where the INVITE went, in its transaction. */
        const char *to = hdr(resp, "To");
        b_printf(s_scratch, MSG_CAP, &n, "ACK %s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%u;rport;branch=%s\r\n"
                 "Max-Forwards: 70\r\nFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %lu ACK\r\n"
                 "User-Agent: " UA "\r\nContent-Length: 0\r\n\r\n",
                 S.ruri, S.lip, (unsigned)S.lport, branch_if_non2xx, S.from_hdr, to ? to : S.to_hdr,
                 S.callid, (unsigned long)cseq);
    } else {
        char br[32];
        new_branch(br);
        n = dialog_request(s_scratch, MSG_CAP, "ACK", cseq, br, NULL);
        b_printf(s_scratch, MSG_CAP, &n, "Content-Length: 0\r\n\r\n");
    }
    send_raw(s_scratch, n);
}

static size_t build_invite(void)
{
    char auth[640];
    authorize(&S.inv_auth, &S.acc, "INVITE", S.ruri, auth, sizeof auth);
    char sdp[512];
    const size_t sl = sdp_build(sdp, sizeof sdp, -1, -1);
    size_t n = 0;
    new_branch(S.inv_tx.branch);
    S.inv_tx.cseq = S.lcseq++;
    b_printf(s_inv_buf, MSG_CAP, &n,
             "INVITE %s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%u;rport;branch=%s\r\nMax-Forwards: 70\r\n"
             "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %lu INVITE\r\n"
             "Contact: <sip:%s@%s:%u;transport=udp>\r\n%sAllow: " ALLOW "\r\n"
             "User-Agent: " UA "\r\nContent-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s",
             S.ruri, S.lip, (unsigned)S.lport, S.inv_tx.branch, S.from_hdr, S.to_hdr, S.callid,
             (unsigned long)S.inv_tx.cseq, S.acc.user, cip(), (unsigned)cport(), auth,
             (unsigned)sl, sdp);
    return n;
}

bool sip_call(const char *number)
{
    if (!S.on || S.reg != SIP_REG_OK) return false;
    if (S.call != SIP_CALL_IDLE && S.call != SIP_CALL_ENDED) return false;
    char num[32];
    size_t k = 0;
    for (const char *p = number; p && *p && k + 1 < sizeof num; p++)
        if (isdigit((unsigned char)*p) || (*p == '+' && k == 0) || *p == '*' || *p == '#') num[k++] = *p;
    num[k] = 0;
    if (!k) return false;
    S.outgoing = true;
    S.inv_prov = S.cancel_due = S.cancelled = false;
    S.nroute = 0;
    S.rtarget[0] = S.rtag[0] = 0;
    memset(&S.inv_auth, 0, sizeof S.inv_auth);
    memset(&S.bye_auth, 0, sizeof S.bye_auth);
    rand_hex(S.callid, 33);
    rand_hex(S.ltag, 11);
    S.lcseq = 1 + (esp_random() % 1000);
    snprintf(S.ruri, sizeof S.ruri, "sip:%s@%s", num, S.acc.domain);
    snprintf(S.from_hdr, sizeof S.from_hdr, "<sip:%s@%s>;tag=%s", S.acc.user, S.acc.domain, S.ltag);
    snprintf(S.to_hdr, sizeof S.to_hdr, "<sip:%s@%s>", num, S.acc.domain);
    strlcpy(S.peer_num, num, sizeof S.peer_num);
    strlcpy(S.peer, num, sizeof S.peer);
    S.t_call = s_now;
    const size_t n = build_invite();
    tx_go(&S.inv_tx, s_inv_buf, n);
    ESP_LOGI(TAG, "calling %s", num);
    call_state(SIP_CALL_OUT, "");
    return true;
}

static void send_bye_like(const char *method)
{
    char auth[640] = "";
    strlcpy(S.bye_method, method, sizeof S.bye_method);
    if (!strcmp(method, "CANCEL")) {
        /* CANCEL rides the INVITE's own transaction: its branch and CSeq. */
        size_t n = 0;
        b_printf(s_bye_buf, MSG_CAP, &n,
                 "CANCEL %s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%u;rport;branch=%s\r\nMax-Forwards: 70\r\n"
                 "From: %s\r\nTo: <%s>\r\nCall-ID: %s\r\nCSeq: %lu CANCEL\r\nUser-Agent: " UA "\r\n"
                 "Content-Length: 0\r\n\r\n",
                 S.ruri, S.lip, (unsigned)S.lport, S.inv_tx.branch, S.from_hdr, S.ruri, S.callid,
                 (unsigned long)S.inv_tx.cseq);
        strlcpy(S.bye_tx.branch, S.inv_tx.branch, sizeof S.bye_tx.branch);
        S.bye_tx.cseq = S.inv_tx.cseq;
        tx_go(&S.bye_tx, s_bye_buf, n);
        return;
    }
    const char *target = S.rtarget[0] ? S.rtarget : S.ruri;
    authorize(&S.bye_auth, &S.acc, method, target, auth, sizeof auth);
    new_branch(S.bye_tx.branch);
    S.bye_tx.cseq = S.lcseq++;
    size_t n = dialog_request(s_bye_buf, MSG_CAP, method, S.bye_tx.cseq, S.bye_tx.branch, auth);
    b_printf(s_bye_buf, MSG_CAP, &n, "Content-Length: 0\r\n\r\n");
    tx_go(&S.bye_tx, s_bye_buf, n);
}

/* A response to the call taken, from its stored headers. */
static size_t in_response(int code, const char *reason, bool contact, const char *body)
{
    size_t n = 0;
    b_printf(s_resp_buf, MSG_CAP, &n, "SIP/2.0 %d %s\r\n%s", code, reason, S.in_hdrs);
    if (contact) b_printf(s_resp_buf, MSG_CAP, &n, "Contact: <sip:%s@%s:%u;transport=udp>\r\n",
                          S.acc.user, cip(), (unsigned)cport());
    b_printf(s_resp_buf, MSG_CAP, &n, "Allow: " ALLOW "\r\nUser-Agent: " UA "\r\n");
    if (body) b_printf(s_resp_buf, MSG_CAP, &n, "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s",
                       (unsigned)strlen(body), body);
    else      b_printf(s_resp_buf, MSG_CAP, &n, "Content-Length: 0\r\n\r\n");
    return n;
}

void sip_answer(void)
{
    if (S.call != SIP_CALL_IN) return;
    char sdp[512];
    sdp_build(sdp, sizeof sdp, S.offer.codec, S.offer.dtmf);
    const size_t n = in_response(200, "OK", true, sdp);
    tx_go(&S.resp_tx, s_resp_buf, n);        /* until its ACK */
    media_from(&S.offer);
    ESP_LOGI(TAG, "answered %s", S.peer_num);
    call_state(SIP_CALL_UP, "");
}

void sip_hangup(void)
{
    switch (S.call) {
    case SIP_CALL_OUT:
        if (S.inv_prov) send_bye_like("CANCEL");
        else            S.cancel_due = true;     /* once it has answered at all */
        S.cancelled = true;
        call_state(SIP_CALL_ENDED, "");
        break;
    case SIP_CALL_IN: {
        const size_t n = in_response(603, "Decline", false, NULL);
        tx_go(&S.resp_tx, s_resp_buf, n);
        ESP_LOGI(TAG, "declined %s", S.peer_num);
        call_state(SIP_CALL_ENDED, "declined");
        break;
    }
    case SIP_CALL_UP:
        send_bye_like("BYE");
        ESP_LOGI(TAG, "hung up");
        call_state(SIP_CALL_ENDED, "");
        break;
    default:
        break;
    }
}

/* A call ringing in that will not be answered here -- it rang too long, or
 * the session it came on is going: the caller told, and the call missed, as
 * one the caller gave up on. Not declined: nobody here said no. */
static void ring_off(void)
{
    const size_t n = in_response(480, "Temporarily Unavailable", false, NULL);
    tx_go(&S.resp_tx, s_resp_buf, n);        /* until its ACK */
    call_state(SIP_CALL_ENDED, "missed");
}

sip_call_t sip_call_state(void) { return S.call; }

/* ------------------------------------------------------------ requests in */

static void respond(const msg_t *m, int code, const char *reason, const char *extra)
{
    char vias[6][192];
    const int nv = hdr_all(m, "Via", vias, 6);
    size_t n = 0;
    b_printf(s_scratch, MSG_CAP, &n, "SIP/2.0 %d %s\r\n", code, reason);
    for (int i = 0; i < nv; i++) b_printf(s_scratch, MSG_CAP, &n, "Via: %s\r\n", vias[i]);
    const char *to = hdr(m, "To");
    char tag[64];
    const bool has_tag = param(to, "tag", tag, sizeof tag);
    b_printf(s_scratch, MSG_CAP, &n, "From: %s\r\nTo: %s%s%s\r\nCall-ID: %s\r\nCSeq: %s\r\n%s"
             "User-Agent: " UA "\r\nContent-Length: 0\r\n\r\n",
             hdr(m, "From") ? hdr(m, "From") : "", to ? to : "",
             has_tag ? "" : ";tag=", has_tag ? "" : S.ltag[0] ? S.ltag : "k",
             hdr(m, "Call-ID") ? hdr(m, "Call-ID") : "", hdr(m, "CSeq") ? hdr(m, "CSeq") : "",
             extra ? extra : "");
    send_raw(s_scratch, n);
}

static bool our_dialog(const msg_t *m)
{
    const char *cid = hdr(m, "Call-ID");
    return cid && S.callid[0] && !strcmp(cid, S.callid) &&
           S.call != SIP_CALL_IDLE;
}

static void on_invite(msg_t *m)
{
    char totag[64];
    const bool re = param(hdr(m, "To"), "tag", totag, sizeof totag);
    if (re) {
        /* A re-INVITE: answered as the call stands, the far end's new media
         * address taken. */
        if (!our_dialog(m) || S.call != SIP_CALL_UP) { respond(m, 481, "Call/Transaction Does Not Exist", NULL); return; }
        sdp_t d;
        if (sdp_parse(m->body, &d, false)) {
            if (d.codec != S.media.pt) d.codec = S.media.pt;
            if (!d.hold) media_from(&d);
        }
        char sdp[512];
        sdp_build(sdp, sizeof sdp, S.media.pt, S.media.dtmf_pt);
        char vias[6][192];
        const int nv = hdr_all(m, "Via", vias, 6);
        size_t n = 0;
        b_printf(s_scratch, MSG_CAP, &n, "SIP/2.0 200 OK\r\n");
        for (int i = 0; i < nv; i++) b_printf(s_scratch, MSG_CAP, &n, "Via: %s\r\n", vias[i]);
        b_printf(s_scratch, MSG_CAP, &n, "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n"
                 "Contact: <sip:%s@%s:%u;transport=udp>\r\nUser-Agent: " UA "\r\n"
                 "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s",
                 hdr(m, "From"), hdr(m, "To"), hdr(m, "Call-ID"), hdr(m, "CSeq"),
                 S.acc.user, cip(), (unsigned)cport(), (unsigned)strlen(sdp), sdp);
        send_raw(s_scratch, n);
        return;
    }
    if (!S.outgoing && S.callid[0] && hdr(m, "Call-ID") && !strcmp(hdr(m, "Call-ID"), S.callid)) {
        send_raw(s_resp_buf, strlen(s_resp_buf));         /* its retransmission */
        return;
    }
    if (S.call != SIP_CALL_IDLE && S.call != SIP_CALL_ENDED) {
        respond(m, 486, "Busy Here", NULL);
        return;
    }
    sdp_t d;
    if (!sdp_parse(m->body, &d, false)) {
        respond(m, 488, "Not Acceptable Here", NULL);
        return;
    }
    /* Taken: remember what every response to it repeats. */
    S.outgoing = false;
    S.offer = d;
    strlcpy(S.callid, hdr(m, "Call-ID"), sizeof S.callid);
    rand_hex(S.ltag, 11);
    S.in_cseq = (uint32_t)strtoul(hdr(m, "CSeq") ? hdr(m, "CSeq") : "0", NULL, 10);
    S.lcseq = 1 + (esp_random() % 1000);
    who_of(hdr(m, "From"), S.peer, sizeof S.peer, S.peer_num, sizeof S.peer_num);
    uri_of(hdr(m, "Contact"), S.rtarget, sizeof S.rtarget);
    strlcpy(S.ruri, m->ruri, sizeof S.ruri);
    S.nroute = hdr_all(m, "Record-Route", S.route, 4);      /* as they came: we are the callee */
    snprintf(S.from_hdr, sizeof S.from_hdr, "%s;tag=%s", hdr(m, "To"), S.ltag);
    strlcpy(S.to_hdr, hdr(m, "From"), sizeof S.to_hdr);
    char vias[6][192];
    const int nv = hdr_all(m, "Via", vias, 6);
    size_t n = 0;
    S.in_hdrs[0] = 0;
    for (int i = 0; i < nv; i++) b_printf(S.in_hdrs, sizeof S.in_hdrs, &n, "Via: %s\r\n", vias[i]);
    for (int i = 0; i < S.nroute; i++) b_printf(S.in_hdrs, sizeof S.in_hdrs, &n, "Record-Route: %s\r\n", S.route[i]);
    b_printf(S.in_hdrs, sizeof S.in_hdrs, &n, "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n",
             hdr(m, "From"), S.from_hdr, S.callid, hdr(m, "CSeq"));
    respond(m, 100, "Trying", NULL);
    const size_t rn = in_response(180, "Ringing", true, NULL);
    send_raw(s_resp_buf, rn);
    S.t_call = s_now;
    ESP_LOGI(TAG, "call from %s (%s)", S.peer, S.peer_num);
    call_state(SIP_CALL_IN, "");
}

static void on_request(msg_t *m)
{
    const char *meth = m->method;
    if (!strcmp(meth, "INVITE")) { on_invite(m); return; }
    if (!strcmp(meth, "ACK")) {
        const char *cid = hdr(m, "Call-ID");
        if (cid && !strcmp(cid, S.callid)) S.resp_tx.active = false;   /* our final response arrived */
        return;
    }
    if (!strcmp(meth, "CANCEL")) {
        if (S.call == SIP_CALL_IN && our_dialog(m)) {
            respond(m, 200, "OK", NULL);
            const size_t n = in_response(487, "Request Terminated", false, NULL);
            tx_go(&S.resp_tx, s_resp_buf, n);
            ESP_LOGI(TAG, "%s gave up", S.peer_num);
            call_state(SIP_CALL_ENDED, "missed");
        } else {
            respond(m, 481, "Call/Transaction Does Not Exist", NULL);
        }
        return;
    }
    if (!strcmp(meth, "BYE")) {
        if (our_dialog(m)) {
            respond(m, 200, "OK", NULL);
            if (S.call == SIP_CALL_UP) {
                ESP_LOGI(TAG, "%s hung up", S.peer_num);
                call_state(SIP_CALL_ENDED, "");
            }
        } else {
            respond(m, 481, "Call/Transaction Does Not Exist", NULL);
        }
        return;
    }
    if (!strcmp(meth, "OPTIONS")) {
        respond(m, 200, "OK", "Allow: " ALLOW "\r\nAccept: application/sdp\r\n");
        return;
    }
    if (!strcmp(meth, "INFO") || !strcmp(meth, "UPDATE") || !strcmp(meth, "NOTIFY")) {
        respond(m, 200, "OK", NULL);
        return;
    }
    respond(m, 405, "Method Not Allowed", "Allow: " ALLOW "\r\n");
}

/* ------------------------------------------------------------ responses in */

static void reg_send(bool with_auth);

static void on_register_response(msg_t *m)
{
    learn_public(m);
    if (m->code < 200) return;
    S.reg_tx.active = false;
    if (m->code == 401 || m->code == 407) {
        if (challenge(m, &S.reg_auth)) { reg_send(true); return; }
        S.reg = SIP_REG_FAILED;
        strlcpy(S.reg_why, "user or password refused", sizeof S.reg_why);
        ESP_LOGW(TAG, "registration refused: user or password");
        S.t_reg_next = s_now + RETRY_MS * 4;
        return;
    }
    if (m->code >= 200 && m->code < 300) {
        /* Our binding's expiry, else the response's, else what we asked. */
        uint32_t exp = EXPIRES_S;
        const char *e = hdr(m, "Expires");
        if (e) exp = (uint32_t)atoi(e);
        char ce[16];
        if (param(hdr(m, "Contact"), "expires", ce, sizeof ce)) exp = (uint32_t)atoi(ce);
        if (exp < 30) exp = 30;
        /* Registered with the LAN address, and now we know the outside one:
         * again, with that, so calls can find us. */
        char want[64];
        snprintf(want, sizeof want, "%s:%u", cip(), (unsigned)cport());
        if (strcmp(want, S.reg_contact)) {
            ESP_LOGI(TAG, "registering again as %s", want);
            reg_send(S.reg_auth.have);
            return;
        }
        if (S.reg != SIP_REG_OK) ESP_LOGI(TAG, "registered as %s@%s, for %lu s", S.acc.user,
                                          S.acc.domain, (unsigned long)exp);
        S.reg = SIP_REG_OK;
        S.reg_why[0] = 0;
        S.t_reg_next = s_now + (uint64_t)exp * 800;      /* refreshed at 80 % */
        return;
    }
    S.reg = SIP_REG_FAILED;
    snprintf(S.reg_why, sizeof S.reg_why, "registrar said %d", m->code);
    ESP_LOGW(TAG, "registration failed: %d", m->code);
    S.t_reg_next = s_now + RETRY_MS;
}

static void on_invite_response(msg_t *m, uint32_t cseq)
{
    if (cseq != S.inv_tx.cseq) return;
    learn_public(m);
    if (m->code < 200) {
        S.inv_tx.active = false;                /* no more retransmissions */
        if (!S.inv_prov) {
            S.inv_prov = true;
            if (S.cancel_due) { send_bye_like("CANCEL"); S.cancel_due = false; }
        }
        if (S.call == SIP_CALL_OUT && (m->code == 180 || m->code == 183)) {
            strlcpy(S.why, "ringing", sizeof S.why);
            if (S.ev.on_call) S.ev.on_call(S.call, S.peer, S.peer_num, S.why);
            sdp_t d;                            /* early media: their ringback */
            if (m->code == 183 && sdp_parse(m->body, &d, true)) media_from(&d);
        }
        return;
    }
    S.inv_tx.active = false;
    if (m->code >= 200 && m->code < 300) {
        /* The first 2xx makes the dialog -- the far end's tag and Contact,
         * the route set reversed -- and every 2xx, retransmissions too, is
         * ACKed within it. One that comes as we cancel is ACKed, then BYE. */
        const bool first = !S.rtarget[0] && (S.call == SIP_CALL_OUT || S.cancelled);
        if (first) {
            param(hdr(m, "To"), "tag", S.rtag, sizeof S.rtag);
            strlcpy(S.to_hdr, hdr(m, "To"), sizeof S.to_hdr);
            uri_of(hdr(m, "Contact"), S.rtarget, sizeof S.rtarget);
            char rr[4][192];
            const int n = hdr_all(m, "Record-Route", rr, 4);
            S.nroute = n;
            for (int i = 0; i < n; i++) strlcpy(S.route[i], rr[n - 1 - i], sizeof S.route[i]);
        }
        send_ack(cseq, NULL, m);
        if (S.call != SIP_CALL_OUT) {
            if (first && S.cancelled) send_bye_like("BYE");
            return;
        }
        sdp_t d;
        if (sdp_parse(m->body, &d, true)) media_from(&d);
        else ESP_LOGW(TAG, "no usable media in the answer");
        ESP_LOGI(TAG, "%s answered", S.peer_num);
        call_state(SIP_CALL_UP, "");
        return;
    }
    send_ack(cseq, S.inv_tx.branch, m);
    if ((m->code == 401 || m->code == 407) && S.call == SIP_CALL_OUT && challenge(m, &S.inv_auth)) {
        const size_t n = build_invite();
        tx_go(&S.inv_tx, s_inv_buf, n);
        return;
    }
    if (S.call == SIP_CALL_OUT) {
        const char *r = hdr(m, "Reason");
        ESP_LOGI(TAG, "call to %s: %d%s%s", S.peer_num, m->code, r ? ", " : "", r ? r : "");
        call_state(SIP_CALL_ENDED, why_of_msg(m));
    }
}

static void on_bye_response(msg_t *m, uint32_t cseq)
{
    if (cseq != S.bye_tx.cseq || m->code < 200) return;
    S.bye_tx.active = false;
    if ((m->code == 401 || m->code == 407) && strcmp(S.bye_method, "CANCEL") &&
        challenge(m, &S.bye_auth))
        send_bye_like(S.bye_method);
}

static void on_response(msg_t *m)
{
    const char *cs = hdr(m, "CSeq");
    if (!cs) return;
    const uint32_t cseq = (uint32_t)strtoul(cs, NULL, 10);
    const char *meth = strchr(cs, ' ');
    meth = meth ? meth + 1 : "";
    while (*meth == ' ') meth++;
    const char *cid = hdr(m, "Call-ID");
    if (!strcmp(meth, "REGISTER")) {
        if (cid && !strcmp(cid, S.reg_callid) && cseq == S.reg_tx.cseq) on_register_response(m);
        return;
    }
    if (!cid || strcmp(cid, S.callid)) return;
    if (!strcmp(meth, "INVITE")) on_invite_response(m, cseq);
    else if (!strcmp(meth, "BYE") || !strcmp(meth, "CANCEL")) on_bye_response(m, cseq);
}

/* ------------------------------------------------------------ registration */

static void reg_send(bool with_auth)
{
    if (!S.srv_ok) return;
    char auth[640] = "", uri[96];
    snprintf(uri, sizeof uri, "sip:%s", S.acc.domain);
    if (with_auth) authorize(&S.reg_auth, &S.acc, "REGISTER", uri, auth, sizeof auth);
    snprintf(S.reg_contact, sizeof S.reg_contact, "%s:%u", cip(), (unsigned)cport());
    new_branch(S.reg_tx.branch);
    S.reg_tx.cseq = ++S.reg_cseq;
    size_t n = 0;
    b_printf(s_reg_buf, MSG_CAP, &n,
             "REGISTER %s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%u;rport;branch=%s\r\nMax-Forwards: 70\r\n"
             "From: <sip:%s@%s>;tag=%s\r\nTo: <sip:%s@%s>\r\nCall-ID: %s\r\nCSeq: %lu REGISTER\r\n"
             "Contact: <sip:%s@%s;transport=udp>\r\nExpires: %d\r\n%sAllow: " ALLOW "\r\n"
             "User-Agent: " UA "\r\nContent-Length: 0\r\n\r\n",
             uri, S.lip, (unsigned)S.lport, S.reg_tx.branch, S.acc.user, S.acc.domain, S.reg_tag,
             S.acc.user, S.acc.domain, S.reg_callid, (unsigned long)S.reg_tx.cseq,
             S.acc.user, S.reg_contact, EXPIRES_S, auth);
    tx_go(&S.reg_tx, s_reg_buf, n);
    if (S.reg != SIP_REG_OK) S.reg = SIP_REG_TRYING;
}

static bool resolve(void)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
    char port[8];
    snprintf(port, sizeof port, "%u", (unsigned)(S.acc.port ? S.acc.port : 5060));
    if (getaddrinfo(S.acc.domain, port, &hints, &res) != 0 || !res) {
        strlcpy(S.reg_why, "cannot find the server", sizeof S.reg_why);
        return false;
    }
    memcpy(&S.srv, res->ai_addr, sizeof S.srv);
    freeaddrinfo(res);
    S.srv_ok = true;
    S.t_resolved = s_now;
    ESP_LOGI(TAG, "%s is %s", S.acc.domain, inet_ntoa(S.srv.sin_addr));
    return true;
}

static void local_ip(void)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK)
        snprintf(S.lip, sizeof S.lip, IPSTR, IP2STR(&ip.ip));
}

/* ------------------------------------------------------------ the loop */

void sip_start(const sip_account_t *a, const sip_events_t *ev, uint16_t rtp_port)
{
    sip_stop();
    if (!a || !a->user[0] || !a->domain[0]) return;
    S.acc = *a;
    if (ev) S.ev = *ev;
    S.rtp_port = rtp_port;
    S.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (S.fd < 0) return;
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(S.fd, (struct sockaddr *)&me, sizeof me);
    socklen_t sl = sizeof me;
    getsockname(S.fd, (struct sockaddr *)&me, &sl);
    S.lport = ntohs(me.sin_port);
    local_ip();
    rand_hex(S.reg_callid, 33);
    rand_hex(S.reg_tag, 11);
    S.reg_cseq = esp_random() % 1000;
    S.on = true;
    S.reg = SIP_REG_TRYING;
    S.t_reg_next = 0;
    ESP_LOGI(TAG, "SIP on %s:%u, RTP %u", S.lip, (unsigned)S.lport, (unsigned)rtp_port);
}

void sip_stop(void)
{
    /* A call goes with the session -- the WiFi gone, or the account saved
     * anew: ended as any call ends, the far end told while it still can
     * be, and the client told it is over. Left as it was, the knob went on
     * ringing, or calling, for good -- buzzing, its screen held lit -- with
     * nothing here any more to answer, decline or hang up. */
    if (S.on) {
        if (S.call == SIP_CALL_UP || S.call == SIP_CALL_OUT) {
            ESP_LOGI(TAG, "the session goes: the call ends with it");
            sip_hangup();
        } else if (S.call == SIP_CALL_IN) {
            ESP_LOGI(TAG, "the session goes: %s rang unanswered", S.peer_num);
            ring_off();
        }
    }
    const bool told = S.call != SIP_CALL_IDLE;
    if (S.fd >= 0) close(S.fd);
    S.fd = -1;
    S.on = false;
    S.srv_ok = false;
    S.reg = SIP_REG_OFF;
    S.call = SIP_CALL_IDLE;
    S.pip[0] = 0;
    S.pport = 0;
    S.reg_tx.active = S.inv_tx.active = S.bye_tx.active = S.resp_tx.active = false;
    memset(&S.reg_auth, 0, sizeof S.reg_auth);
    /* Idle at once, not after ENDED_MS: sip_tick(), which would say so,
     * does nothing more until the session is back. */
    if (told && S.ev.on_call) S.ev.on_call(SIP_CALL_IDLE, "", "", "");
}

int sip_socket(void) { return S.on ? S.fd : -1; }

void sip_on_readable(uint64_t now_ms)
{
    s_now = now_ms;
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    const int n = recvfrom(S.fd, s_rx, MSG_CAP, 0, (struct sockaddr *)&from, &fl);
    if (n <= 4) return;                      /* a keep-alive's CRLF */
    msg_t m;
    if (!parse(s_rx, (size_t)n, &m)) return;
    if (m.req) on_request(&m);
    else       on_response(&m);
}

void sip_tick(uint64_t now_ms)
{
    s_now = now_ms;
    if (!S.on) return;
    if (!S.srv_ok || s_now - S.t_resolved > 3600000) {
        local_ip();
        if (!S.lip[0] || !resolve()) {
            S.reg = SIP_REG_FAILED;
            return;
        }
    }
    /* Registration: due, or refreshed; a transaction that gave up, again later. */
    if (!S.reg_tx.active && s_now >= S.t_reg_next) {
        local_ip();
        reg_send(S.reg_auth.have);
        S.t_reg_next = s_now + RETRY_MS;     /* a response sets the real one */
    }
    if (!tx_poll(&S.reg_tx, T2_MS)) {
        S.reg = SIP_REG_FAILED;
        strlcpy(S.reg_why, "no answer from the server", sizeof S.reg_why);
        S.pip[0] = 0;
        S.pport = 0;
        S.t_reg_next = s_now + RETRY_MS;
        ESP_LOGW(TAG, "registration: no answer");
    }
    if (S.reg == SIP_REG_OK && s_now - S.t_keepalive >= KEEPALIVE_MS) {
        send_raw("\r\n\r\n", 4);
        S.t_keepalive = s_now;
    }
    /* Our INVITE: retransmitted until anything comes back. */
    if (!tx_poll(&S.inv_tx, T2_MS) && S.call == SIP_CALL_OUT) call_state(SIP_CALL_ENDED, "no answer");
    if (S.call == SIP_CALL_OUT && S.inv_prov && s_now - S.t_call > RING_MS) sip_hangup();
    /* One ringing in, the same: a caller long gone whose CANCEL never got
     * here would ring the knob, and hold its screen lit, for good. */
    if (S.call == SIP_CALL_IN && s_now - S.t_call > RING_MS) {
        ESP_LOGI(TAG, "%s rang two minutes unanswered", S.peer_num);
        ring_off();
    }
    tx_poll(&S.bye_tx, T2_MS);
    if (!tx_poll(&S.resp_tx, T2_MS) && S.call == SIP_CALL_UP && !S.outgoing) {
        ESP_LOGW(TAG, "no ACK for our answer: hanging up");
        sip_hangup();
    }
    if (S.call == SIP_CALL_ENDED && s_now - S.t_ended >= ENDED_MS) {
        S.call = SIP_CALL_IDLE;
        if (S.ev.on_call) S.ev.on_call(SIP_CALL_IDLE, "", "", "");
    }
}

sip_reg_t sip_reg_state(char *why, size_t cap)
{
    if (why && cap) strlcpy(why, S.reg_why, cap);
    return S.reg;
}

const char *sip_public_ip(void) { return S.pip; }
