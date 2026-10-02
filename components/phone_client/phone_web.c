/* The telephone on the configuration page, and for other programs. Registered
 * by webcfg behind its login (radio_web_endpoints() in phone_client.c).
 *
 *   GET  /api/phone                  the state, as JSON
 *   GET  /api/phone/dial?number=...  call (POST with the same field too)
 *   GET  /api/phone/answer           answer the call that rings
 *   GET  /api/phone/hangup           hang up, cancel or decline
 *   GET  /api/phone/dtmf?digits=...  keys, in a call
 *   GET  /api/phone/account          the SIP account, without its password
 *   POST /api/phone/account          user, pass, domain, port, number
 *   GET  /api/phone/history          the calls, newest first, as JSON
 *   GET  /api/phone/favourites       the favourites, as JSON
 *   POST /api/phone/favourites       n, name0, number0, name1, ...
 *   GET  /api/phone/google           Google Contacts: signed in, synced?
 *   POST /api/phone/google           client_id, client_secret, relay
 *   GET  /api/phone/google/signin    to Google's sign-in, and back
 *   GET  /api/phone/google/callback  where the relay page brings the code
 *   POST /api/phone/google/sync      the starred contacts, now
 *   POST /api/phone/google/forget    signed out: the token deleted
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "contacts.h"
#include "phone_client.h"
#include "phone_priv.h"

/* ---------------------------------------------------------------- forms */

/* A form's fields, from the body of a POST or the query of a GET. */
static char *args_of(httpd_req_t *r)
{
    const size_t cap = 2048;
    char *b = calloc(1, cap);
    if (!b) return NULL;
    if (r->method == HTTP_POST && r->content_len > 0) {
        if (r->content_len >= cap) { free(b); return NULL; }
        int got = 0;
        while (got < (int)r->content_len) {
            const int k = httpd_req_recv(r, b + got, r->content_len - got);
            if (k <= 0) { free(b); return NULL; }
            got += k;
        }
        b[got] = 0;
    } else if (httpd_req_get_url_query_str(r, b, cap) != ESP_OK) {
        b[0] = 0;
    }
    return b;
}

static bool field(const char *args, const char *key, char *out, size_t cap)
{
    char raw[256];
    if (!args || httpd_query_key_value(args, key, raw, sizeof raw) != ESP_OK) return false;
    size_t o = 0;
    for (size_t i = 0; raw[i] && o + 1 < cap; i++) {
        if (raw[i] == '+') {
            out[o++] = ' ';
        } else if (raw[i] == '%' && raw[i + 1] && raw[i + 2]) {
            const char h[3] = { raw[i + 1], raw[i + 2], 0 };
            out[o++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else {
            out[o++] = raw[i];
        }
    }
    out[o] = 0;
    return true;
}

static void esc(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; s && *s && o + 7 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20)         o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        else                       out[o++] = (char)c;
    }
    out[o] = 0;
}

static esp_err_t json(httpd_req_t *r, const char *body)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, body);
}

static esp_err_t ok(httpd_req_t *r, bool done, const char *why)
{
    char b[96];
    snprintf(b, sizeof b, "{\"ok\":%s%s%s%s}", done ? "true" : "false",
             why ? ",\"why\":\"" : "", why ? why : "", why ? "\"" : "");
    return json(r, b);
}

/* ---------------------------------------------------------------- calls */

static esp_err_t state_get(httpd_req_t *r)
{
    char b[640];
    phone_state_json(b, sizeof b);
    return json(r, b);
}

static esp_err_t dial_h(httpd_req_t *r)
{
    char *a = args_of(r);
    char num[32] = "";
    field(a, "number", num, sizeof num);
    free(a);
    if (!num[0]) return ok(r, false, "no number");
    return ok(r, phone_dial(num), NULL);
}

static esp_err_t answer_h(httpd_req_t *r) { phone_answer(); return ok(r, true, NULL); }
static esp_err_t hangup_h(httpd_req_t *r) { phone_hangup(); return ok(r, true, NULL); }

static esp_err_t dtmf_h(httpd_req_t *r)
{
    char *a = args_of(r);
    char d[24] = "";
    field(a, "digits", d, sizeof d);
    free(a);
    for (const char *p = d; *p; p++) phone_dtmf(*p);
    return ok(r, d[0] != 0, d[0] ? NULL : "no digits");
}

/* The test switch for the call face's meters (phone_set_meters). */
static esp_err_t meters_h(httpd_req_t *r)
{
    char *a = args_of(r);
    char v[8] = "";
    if (field(a, "on", v, sizeof v)) phone_set_meters(v[0] == '1' || v[0] == 't' || v[0] == 'y');
    free(a);
    char b[48];
    snprintf(b, sizeof b, "{\"ok\":true,\"meters\":%s}", phone_meters() ? "true" : "false");
    return json(r, b);
}

/* ---------------------------------------------------------------- account */

static esp_err_t account_get(httpd_req_t *r)
{
    sip_account_t a;
    bool has_pass;
    phone_account_get(&a, &has_pass);
    char u[96], d[128], n[48], b[400];
    esc(a.user, u, sizeof u);
    esc(a.domain, d, sizeof d);
    esc(a.number, n, sizeof n);
    snprintf(b, sizeof b, "{\"user\":\"%s\",\"domain\":\"%s\",\"port\":%u,\"number\":\"%s\",\"pass\":%s}",
             u, d, (unsigned)a.port, n, has_pass ? "true" : "false");
    return json(r, b);
}

static esp_err_t account_post(httpd_req_t *r)
{
    char *a = args_of(r);
    if (!a) return ok(r, false, "no form");
    sip_account_t n = { 0 };
    char port[8] = "";
    field(a, "user", n.user, sizeof n.user);
    field(a, "pass", n.pass, sizeof n.pass);
    field(a, "domain", n.domain, sizeof n.domain);
    field(a, "number", n.number, sizeof n.number);
    field(a, "port", port, sizeof port);
    free(a);
    n.port = (uint16_t)atoi(port);
    if (!n.user[0] || !n.domain[0]) return ok(r, false, "user and server are needed");
    return ok(r, phone_account_set(&n) == ESP_OK, NULL);
}

/* ---------------------------------------------------------------- history */

static esp_err_t history_get(httpd_req_t *r)
{
    static const char *KIND[] = { "out", "in", "missed", "declined" };
    phone_call_t *c = calloc(PHONE_HIST_MAX, sizeof *c);
    char *b = malloc(4096);
    if (!c || !b) { free(c); free(b); return httpd_resp_send_500(r); }
    const int n = phone_history(c, PHONE_HIST_MAX);
    size_t o = (size_t)snprintf(b, 4096, "{\"calls\":[");
    for (int i = 0; i < n && o < 3800; i++) {
        char nm[64], nu[64];
        esc(c[i].name, nm, sizeof nm);
        esc(c[i].number, nu, sizeof nu);
        o += (size_t)snprintf(b + o, 4096 - o,
            "%s{\"kind\":\"%s\",\"number\":\"%s\",\"name\":\"%s\",\"when\":%lu,"
            "\"seconds\":%u,\"new\":%s}", i ? "," : "", KIND[c[i].kind & 3], nu, nm,
            (unsigned long)c[i].when, (unsigned)c[i].secs, c[i].fresh ? "true" : "false");
    }
    snprintf(b + o, 4096 - o, "]}");
    const esp_err_t e = json(r, b);
    free(c);
    free(b);
    return e;
}

/* ---------------------------------------------------------------- favourites */

static esp_err_t favs_get(httpd_req_t *r)
{
    phone_fav_t *f = calloc(PHONE_FAV_MAX, sizeof *f);
    char *b = malloc(4096);
    if (!f || !b) { free(f); free(b); return httpd_resp_send_500(r); }
    const int n = phone_favs_get(f, PHONE_FAV_MAX);
    size_t o = (size_t)snprintf(b, 4096, "{\"list\":[");
    for (int i = 0; i < n && o < 3900; i++) {
        char nm[64], nu[64];
        esc(f[i].name, nm, sizeof nm);
        esc(f[i].number, nu, sizeof nu);
        o += (size_t)snprintf(b + o, 4096 - o, "%s{\"name\":\"%s\",\"number\":\"%s\"}", i ? "," : "", nm, nu);
    }
    snprintf(b + o, 4096 - o, "]}");
    const esp_err_t e = json(r, b);
    free(f);
    free(b);
    return e;
}

static esp_err_t favs_post(httpd_req_t *r)
{
    char *a = args_of(r);
    if (!a) return ok(r, false, "no form");
    phone_fav_t *f = calloc(PHONE_FAV_MAX, sizeof *f);
    if (!f) { free(a); return httpd_resp_send_500(r); }
    char nb[8] = "";
    field(a, "n", nb, sizeof nb);
    const int n = atoi(nb);
    int k = 0;
    for (int i = 0; i < n && k < PHONE_FAV_MAX; i++) {
        char key[24];
        snprintf(key, sizeof key, "number%d", i);
        if (!field(a, key, f[k].number, sizeof f[k].number) || !f[k].number[0]) continue;
        snprintf(key, sizeof key, "name%d", i);
        if (!field(a, key, f[k].name, sizeof f[k].name) || !f[k].name[0])
            strlcpy(f[k].name, f[k].number, sizeof f[k].name);
        k++;
    }
    free(a);
    const esp_err_t e = phone_favs_set(f, k);
    free(f);
    return ok(r, e == ESP_OK, NULL);
}

/* ---------------------------------------------------------------- Google */

static esp_err_t google_get(httpd_req_t *r)
{
    char b[640];
    contacts_state_json(b, sizeof b);
    return json(r, b);
}

static esp_err_t google_post(httpd_req_t *r)
{
    char *a = args_of(r);
    if (!a) return ok(r, false, "no form");
    char cid[96] = "", sec[64] = "", relay[128] = "";
    const bool have_cid = field(a, "client_id", cid, sizeof cid);
    field(a, "client_secret", sec, sizeof sec);
    field(a, "relay", relay, sizeof relay);
    free(a);
    return ok(r, contacts_set_client(have_cid ? cid : NULL, sec, relay) == ESP_OK, NULL);
}

static esp_err_t redirect(httpd_req_t *r, const char *to)
{
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", to);
    return httpd_resp_send(r, NULL, 0);
}

/* To Google, with the knob's address as the browser has it in `state`: the
 * relay page brings the code back there. */
static esp_err_t google_signin(httpd_req_t *r)
{
    char host[64] = "", *url = malloc(1024);
    if (!url) return httpd_resp_send_500(r);
    httpd_req_get_hdr_value_str(r, "Host", host, sizeof host);
    esp_err_t e;
    if (contacts_signin_url(host, url, 1024)) e = redirect(r, url);
    else e = ok(r, false, "set the Google client first");
    free(url);
    return e;
}

static esp_err_t google_callback(httpd_req_t *r)
{
    char *a = args_of(r);
    char code[256] = "", state[96] = "", error[48] = "";
    field(a, "code", code, sizeof code);
    field(a, "state", state, sizeof state);
    field(a, "error", error, sizeof error);
    free(a);
    if (code[0] || error[0]) contacts_set_code(code, state, error);
    return redirect(r, "/#phonegoogle");
}

static esp_err_t google_sync(httpd_req_t *r)   { contacts_sync();   return ok(r, true, NULL); }
static esp_err_t google_forget(httpd_req_t *r) { contacts_forget(); return ok(r, true, NULL); }

const httpd_uri_t phone_web_uris[] = {
    { .uri = "/api/phone/google",          .method = HTTP_GET,  .handler = google_get },
    { .uri = "/api/phone/google",          .method = HTTP_POST, .handler = google_post },
    { .uri = "/api/phone/google/signin",   .method = HTTP_GET,  .handler = google_signin },
    { .uri = "/api/phone/google/callback", .method = HTTP_GET,  .handler = google_callback },
    { .uri = "/api/phone/google/sync",     .method = HTTP_POST, .handler = google_sync },
    { .uri = "/api/phone/google/forget",   .method = HTTP_POST, .handler = google_forget },
    { .uri = "/api/phone",            .method = HTTP_GET,  .handler = state_get },
    { .uri = "/api/phone/history",    .method = HTTP_GET,  .handler = history_get },
    { .uri = "/api/phone/dial",       .method = HTTP_GET,  .handler = dial_h },
    { .uri = "/api/phone/dial",       .method = HTTP_POST, .handler = dial_h },
    { .uri = "/api/phone/answer",     .method = HTTP_GET,  .handler = answer_h },
    { .uri = "/api/phone/answer",     .method = HTTP_POST, .handler = answer_h },
    { .uri = "/api/phone/hangup",     .method = HTTP_GET,  .handler = hangup_h },
    { .uri = "/api/phone/hangup",     .method = HTTP_POST, .handler = hangup_h },
    { .uri = "/api/phone/dtmf",       .method = HTTP_GET,  .handler = dtmf_h },
    { .uri = "/api/phone/dtmf",       .method = HTTP_POST, .handler = dtmf_h },
    { .uri = "/api/phone/meters",     .method = HTTP_GET,  .handler = meters_h },
    { .uri = "/api/phone/account",    .method = HTTP_GET,  .handler = account_get },
    { .uri = "/api/phone/account",    .method = HTTP_POST, .handler = account_post },
    { .uri = "/api/phone/favourites", .method = HTTP_GET,  .handler = favs_get },
    { .uri = "/api/phone/favourites", .method = HTTP_POST, .handler = favs_post },
};
const size_t phone_web_uris_n = sizeof phone_web_uris / sizeof phone_web_uris[0];
