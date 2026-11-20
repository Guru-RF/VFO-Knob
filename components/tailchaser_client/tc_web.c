/* The repeaters and Tailscale on the configuration page. Registered by
 * webcfg behind its login (radio_web_endpoints() in tailchaser_client.c).
 *
 *   GET  /api/tailchaser          the repeaters (whether each has a key, never
 *                                 the key), Tailscale's settings (whether an
 *                                 auth key is set), and the state
 *   POST /api/tailchaser          n, a0..a7 (address), m0..m7 (its name on the
 *                                 face; "" its callsign), k0..k7 (a new key; ""
 *                                 keeps the one the row had), w0..w7 (the
 *                                 row's place before, -1 for a new one), ts,
 *                                 tsauth (new; "" keeps), tsclear, tsname,
 *                                 hold (seconds the one shown is kept after
 *                                 it was busy, 0-3600, 0 never left by
 *                                 itself; absent or "" keeps)
 *   GET  /api/tailchaser/state    what the knob sees, for the page's poll
 *   POST /api/tailchaser/block    rx=0|1 or tx=0|1, for the repeater in view
 *   POST /api/tailchaser/forget   this knob's Tailscale keys erased: a new
 *                                 node on the tailnet after the restart
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "kvstore.h"

#include "tailchaser_client.h"
#include "tc_proto.h"
#include "tc_ts.h"

#define NS        "tchaser"
#define BODY_MAX  4096
#define STATE_MAX 3072        /* tc_state_json at its longest, every string full */

static esp_err_t json(httpd_req_t *r, const char *body)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, body);
}

static size_t esc(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; s && *s && o + 7 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20)         o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        else                       out[o++] = (char)c;
    }
    out[o] = 0;
    return o;
}

static char *body_of(httpd_req_t *r)
{
    if (r->content_len >= BODY_MAX) return NULL;
    char *b = heap_caps_calloc(1, r->content_len + 1, MALLOC_CAP_SPIRAM);
    if (!b) return NULL;
    size_t got = 0;
    while (got < r->content_len) {
        const int k = httpd_req_recv(r, b + got, r->content_len - got);
        if (k <= 0) {
            free(b);
            return NULL;
        }
        got += (size_t)k;
    }
    return b;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* One field of a form: 1, 0 if absent, -1 if longer than cap allows. */
static int field(const char *body, const char *key, char *out, size_t cap)
{
    const size_t kl = strlen(key);
    for (const char *p = body; p && *p;) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t o = 0;
            for (const char *v = p + kl + 1; v < end; v++) {
                if (o + 1 >= cap) return -1;
                if (*v == '+') {
                    out[o++] = ' ';
                } else if (*v == '%' && v + 2 < end && hexv(v[1]) >= 0 && hexv(v[2]) >= 0) {
                    out[o++] = (char)(hexv(v[1]) << 4 | hexv(v[2]));
                    v += 2;
                } else {
                    out[o++] = *v;
                }
            }
            out[o] = 0;
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

static void kv_str(kv_handle_t h, const char *k, char *out, size_t cap)
{
    size_t n = cap;
    if (kv_get_str(h, k, out, &n) != ESP_OK) out[0] = 0;
}

/* The settings as they are now: the addresses, and the keys for reuse. */
typedef struct {
    char addr[TC_NODES][TC_HOST + 8];
    char nm[TC_NODES][TC_NAME];
    char key[TC_NODES][TC_KEY];
    int  n;
    bool ts;
    char auth[128], name[40];
    uint16_t hold;
} settings_t;

static void load(settings_t *s)
{
    memset(s, 0, sizeof *s);
    s->hold = TC_HOLD_S;
    kv_handle_t h;
    if (kv_open(NS, &h) != ESP_OK) return;
    char *list = kv_dup(h, "nodes", NULL);
    for (char *line = list, *next; line && *line && s->n < TC_NODES; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        tc_addr_t a;
        if (!tc_parse_addr(line, &a)) continue;
        if (a.port == TC_PORT) snprintf(s->addr[s->n], sizeof s->addr[0], "%s", a.host);
        else                   snprintf(s->addr[s->n], sizeof s->addr[0], "%s:%u", a.host, (unsigned)a.port);
        tc_line_name(line, s->nm[s->n], sizeof s->nm[0]);
        char k[16];
        snprintf(k, sizeof k, "key%d", s->n);
        kv_str(h, k, s->key[s->n], sizeof s->key[0]);
        s->n++;
    }
    free(list);
    uint8_t b;
    if (kv_get_u8(h, "ts", &b) == ESP_OK) s->ts = b;
    uint16_t hold;
    if (kv_get_u16(h, "hold", &hold) == ESP_OK) s->hold = hold;
    kv_str(h, "tsauth", s->auth, sizeof s->auth);
    kv_str(h, "tsname", s->name, sizeof s->name);
    kv_close(h);
}

static void wipe(settings_t *s)
{
    memset(s, 0, sizeof *s);
    free(s);
}

static void default_name(char *out, size_t cap)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, cap, "vfo-knob-%02x%02x", mac[4], mac[5]);
}

static esp_err_t get_h(httpd_req_t *r)
{
    settings_t *s = heap_caps_calloc(1, sizeof *s, MALLOC_CAP_SPIRAM);
    char *b = heap_caps_malloc(BODY_MAX + STATE_MAX, MALLOC_CAP_SPIRAM);
    char *st = heap_caps_malloc(STATE_MAX, MALLOC_CAP_SPIRAM);
    if (!s || !b || !st) {
        free(s);
        free(b);
        free(st);
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    load(s);
    size_t o = (size_t)snprintf(b, BODY_MAX, "{\"nodes\":[");
    for (int i = 0; i < s->n; i++) {
        char a[TC_HOST * 2], m[TC_NAME * 2];
        esc(s->addr[i], a, sizeof a);
        esc(s->nm[i], m, sizeof m);
        o += (size_t)snprintf(b + o, BODY_MAX - o, "%s{\"addr\":\"%s\",\"name\":\"%s\",\"key\":%s}", i ? "," : "",
                              a, m, s->key[i][0] ? "true" : "false");
    }
    char name[80], def[40];
    esc(s->name, name, sizeof name);
    default_name(def, sizeof def);
    tc_state_json(st, STATE_MAX);
    snprintf(b + o, BODY_MAX + STATE_MAX - o,
             "],\"hold\":%u,\"ts\":%s,\"tsauth\":%s,\"tsname\":\"%s\",\"tsdefault\":\"%s\",\"state\":%s}",
             (unsigned)s->hold, s->ts ? "true" : "false", s->auth[0] ? "true" : "false", name, def, st);
    const esp_err_t e = json(r, b);
    wipe(s);
    free(b);
    free(st);
    return e;
}

static esp_err_t state_h(httpd_req_t *r)
{
    char *b = heap_caps_malloc(STATE_MAX, MALLOC_CAP_SPIRAM);
    if (!b) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    tc_state_json(b, STATE_MAX);
    const esp_err_t e = json(r, b);
    free(b);
    return e;
}

static esp_err_t set_h(httpd_req_t *r)
{
    char *body = body_of(r);
    if (!body) return json(r, "{\"ok\":false,\"why\":\"the form is too big, or was cut short\"}");
    settings_t *old = heap_caps_calloc(1, sizeof *old, MALLOC_CAP_SPIRAM);
    settings_t *neu = heap_caps_calloc(1, sizeof *neu, MALLOC_CAP_SPIRAM);
    if (!old || !neu) {
        free(body);
        free(old);
        free(neu);
        return json(r, "{\"ok\":false,\"why\":\"no memory\"}");
    }
    load(old);
    const char *why = NULL;
    static char say[160];
    char f[16];
    int n = 0;
    if (field(body, "n", f, sizeof f) == 1) n = atoi(f);
    if (n < 0 || n > TC_NODES) why = "at most 8 repeaters";
    for (int i = 0; !why && i < n; i++) {
        char k[16], addr[TC_HOST + 16], key[TC_KEY], w[8], nm[64];
        snprintf(k, sizeof k, "a%d", i);
        if (field(body, k, addr, sizeof addr) != 1) { why = "an address is too long"; break; }
        tc_addr_t a;
        if (!tc_parse_addr(addr, &a)) {
            snprintf(say, sizeof say, "repeater %d: \"%.60s\" is no address", i + 1, addr);
            why = say;
            break;
        }
        if (a.port == TC_PORT) snprintf(neu->addr[neu->n], sizeof neu->addr[0], "%s", a.host);
        else                   snprintf(neu->addr[neu->n], sizeof neu->addr[0], "%s:%u", a.host, (unsigned)a.port);
        snprintf(k, sizeof k, "m%d", i);
        nm[0] = 0;
        if (field(body, k, nm, sizeof nm) < 0) nm[sizeof nm - 1] = 'x';
        char clean[sizeof nm];
        tc_clean_name(nm, clean, sizeof clean);
        if (strlen(clean) >= TC_NAME || nm[sizeof nm - 1] == 'x') {
            snprintf(say, sizeof say, "repeater %d: a name is at most %d characters", i + 1, TC_NAME - 1);
            why = say;
            break;
        }
        snprintf(neu->nm[neu->n], sizeof neu->nm[0], "%.16s", clean);   /* < TC_NAME: checked */
        snprintf(k, sizeof k, "k%d", i);
        const int got = field(body, k, key, sizeof key);
        if (got < 0) { why = "an API key is too long"; break; }
        if (got == 1 && key[0]) {
            /* What TailChaser makes: tc_ and hex; spaces from a paste dropped. */
            size_t o = 0;
            for (const char *p = key; *p; p++) if (*p > ' ' && *p < 0x7f) key[o++] = *p;
            key[o] = 0;
        }
        if (got == 1 && key[0]) {
            snprintf(neu->key[neu->n], sizeof neu->key[0], "%s", key);
        } else {
            snprintf(k, sizeof k, "w%d", i);
            const int was = field(body, k, w, sizeof w) == 1 ? atoi(w) : -1;
            if (was >= 0 && was < old->n) snprintf(neu->key[neu->n], sizeof neu->key[0], "%s", old->key[was]);
        }
        memset(key, 0, sizeof key);
        neu->n++;
    }
    neu->ts = field(body, "ts", f, sizeof f) == 1 && f[0] == '1';
    neu->hold = old->hold;
    char hs[8];
    const int hf = field(body, "hold", hs, sizeof hs);
    if (hf < 0) {
        if (!why) why = "the hold: 0 to 3600 seconds";
    } else if (hf == 1 && hs[0]) {
        char *end;
        const long v = strtol(hs, &end, 10);
        if (*end || v < 0 || v > 3600) {
            if (!why) why = "the hold: 0 to 3600 seconds";
        } else {
            neu->hold = (uint16_t)v;
        }
    }
    char auth[128] = "";
    if (!why && field(body, "tsauth", auth, sizeof auth) < 0) why = "the auth key is too long";
    if (field(body, "tsclear", f, sizeof f) == 1 && f[0] == '1') neu->auth[0] = 0;
    else if (auth[0])                                            snprintf(neu->auth, sizeof neu->auth, "%s", auth);
    else                                                         snprintf(neu->auth, sizeof neu->auth, "%s", old->auth);
    memset(auth, 0, sizeof auth);
    if (!why && field(body, "tsname", neu->name, sizeof neu->name) < 0) why = "the name is too long";
    for (char *p = neu->name; *p; p++) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            if (c >= 'A' && c <= 'Z') *p = (char)(c - 'A' + 'a');
            else if (!why) why = "the name on the tailnet: letters, digits and - only";
        }
    }
    memset(body, 0, strlen(body));
    free(body);
    if (why) {
        char e[200], out[240];
        esc(why, e, sizeof e);
        snprintf(out, sizeof out, "{\"ok\":false,\"why\":\"%s\"}", e);
        wipe(old);
        wipe(neu);
        return json(r, out);
    }

    char *list = heap_caps_calloc(1, TC_NODES * (TC_HOST + 9 + TC_NAME + 1) + 1, MALLOC_CAP_SPIRAM);
    kv_handle_t h;
    esp_err_t err = list ? kv_open(NS, &h) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        size_t o = 0;
        /* A name after its address, a tab between: a firmware before names
         * reads the address alone. */
        for (int i = 0; i < neu->n; i++)
            o += (size_t)sprintf(list + o, "%s%s%s\n", neu->addr[i], neu->nm[i][0] ? "\t" : "", neu->nm[i]);
        kv_edit_begin(h);
        err = kv_set_str(h, "nodes", list);
        for (int i = 0; i < TC_NODES && err == ESP_OK; i++) {
            char k[16];
            snprintf(k, sizeof k, "key%d", i);
            if (i < neu->n && neu->key[i][0]) err = kv_set_str(h, k, neu->key[i]);
            else                              err = kv_erase_key(h, k);
        }
        if (err == ESP_OK) err = kv_set_u8(h, "ts", neu->ts);
        if (err == ESP_OK) err = kv_set_u16(h, "hold", neu->hold);
        if (err == ESP_OK) err = neu->auth[0] ? kv_set_str(h, "tsauth", neu->auth) : kv_erase_key(h, "tsauth");
        if (err == ESP_OK) err = neu->name[0] ? kv_set_str(h, "tsname", neu->name) : kv_erase_key(h, "tsname");
        /* Saved again: Tailscale is tried again, however it went before. */
        if (err == ESP_OK) err = kv_erase_key(h, "tsfail");
        kv_edit_end(h);
        if (err == ESP_OK) err = kv_commit(h);
        kv_close(h);
    }
    free(list);
    /* Tailscale starts once a boot: a change to it waits for a restart. */
    const bool restart = (tc_ts_held() && neu->ts) ||
                         (tc_ts_state() != TC_TS_OFF &&
                          (neu->ts != old->ts || strcmp(neu->auth, old->auth) || strcmp(neu->name, old->name)));
    wipe(old);
    wipe(neu);
    if (err != ESP_OK) return json(r, "{\"ok\":false,\"why\":\"could not be saved\"}");
    tc_reload();
    return json(r, restart ? "{\"ok\":true,\"restart\":true}" : "{\"ok\":true,\"restart\":false}");
}

static esp_err_t block_h(httpd_req_t *r)
{
    char *body = body_of(r);
    if (!body) return json(r, "{\"ok\":false,\"why\":\"no form\"}");
    char v[8];
    int which = -1;
    bool on = false;
    if (field(body, "rx", v, sizeof v) == 1)      { which = 0; on = v[0] == '1'; }
    else if (field(body, "tx", v, sizeof v) == 1) { which = 1; on = v[0] == '1'; }
    free(body);
    if (which < 0) return json(r, "{\"ok\":false,\"why\":\"rx or tx?\"}");
    tc_request_block(which, on);
    return json(r, "{\"ok\":true}");
}

static esp_err_t forget_h(httpd_req_t *r)
{
    tc_ts_forget();
    return json(r, "{\"ok\":true,\"restart\":true}");
}

const httpd_uri_t tc_web_uris[] = {
    { .uri = "/api/tailchaser",        .method = HTTP_GET,  .handler = get_h },
    { .uri = "/api/tailchaser",        .method = HTTP_POST, .handler = set_h },
    { .uri = "/api/tailchaser/state",  .method = HTTP_GET,  .handler = state_h },
    { .uri = "/api/tailchaser/block",  .method = HTTP_POST, .handler = block_h },
    { .uri = "/api/tailchaser/forget", .method = HTTP_POST, .handler = forget_h },
};
const size_t tc_web_uris_n = sizeof tc_web_uris / sizeof tc_web_uris[0];
