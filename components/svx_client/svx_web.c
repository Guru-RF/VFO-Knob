/* The reflector station on the configuration page: SVXConnect-CLI's
 * svxconnect.conf and its --enroll, over HTTP. Registered by webcfg, behind
 * its login (radio_web_endpoints() in svx_client.c).
 *
 *   GET  /api/svx          settings, link and certificate, as JSON
 *   POST /api/svx          settings, form-encoded; the client logs in again
 *   POST /api/svx/enroll   request a certificate; cancel=1 stops asking
 *   POST /api/svx/forget   delete the key, the request and the certificate
 *   GET  /api/svx/csr      the certificate request, for a sysop by mail
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "svx_client.h"
#include "svx_pki.h"

/* ---------------------------------------------------------------- JSON */

typedef struct {
    char  *b;
    size_t n, cap;
} jb_t;

static void jb_raw(jb_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb_raw(jb_t *j, const char *fmt, ...)
{
    if (j->n >= j->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(j->b + j->n, j->cap - j->n, fmt, ap);
    va_end(ap);
    if (k > 0) j->n = j->n + (size_t)k < j->cap ? j->n + (size_t)k : j->cap;
}

/* "key":"value", escaped. */
static void jb_str(jb_t *j, const char *key, const char *s)
{
    jb_raw(j, "\"%s\":\"", key);
    for (; s && *s && j->n + 8 < j->cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { j->b[j->n++] = '\\'; j->b[j->n++] = (char)c; }
        else if (c < 0x20)         jb_raw(j, "\\u%04x", c);
        else                       j->b[j->n++] = (char)c;
    }
    jb_raw(j, "\",");
}

static const char *cert_state(pki_state_t s)
{
    static const char *N[] = {
        [PKI_NO_KEY] = "none", [PKI_NO_CERT] = "requested", [PKI_VALID] = "valid",
        [PKI_NOT_YET_VALID] = "not yet valid", [PKI_RENEW_DUE] = "valid, renewal due",
        [PKI_EXPIRING] = "expiring", [PKI_EXPIRED] = "expired", [PKI_REFUSED] = "refused",
        [PKI_WRONG_CALL] = "for another callsign",
    };
    return (unsigned)s < sizeof N / sizeof N[0] && N[s] ? N[s] : "?";
}

static esp_err_t svx_get(httpd_req_t *r)
{
    svx_settings_t *s = malloc(sizeof *s);
    jb_t j = { .b = malloc(2048), .cap = 2048 };
    if (!s || !j.b) { free(s); free(j.b); return httpd_resp_send_500(r); }
    svx_settings_get(s);
    svx_state_t st;
    svx_state(&st);
    char call[32];
    strlcpy(call, s->call, sizeof call);
    for (char *c = call; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    const time_t now = time(NULL);
    pki_info_t pi;
    svx_pki_info(call, now, &pi);

    jb_raw(&j, "{");
    jb_str(&j, "call", s->call);
    jb_str(&j, "email", s->email);
    jb_str(&j, "loc", s->location);
    jb_str(&j, "lat", s->lat);
    jb_str(&j, "lon", s->lon);
    jb_str(&j, "sw", s->sw);
    jb_str(&j, "mon", s->mon);
    jb_raw(&j, "\"deftg\":%lu,\"lock\":%s,\"linger\":%u,\"idle\":%u,\"roger\":%s,"
               "\"agc\":%s,\"txto\":%u,\"feed\":%s,",
           (unsigned long)s->default_tg, s->lock_on_start ? "true" : "false",
           s->linger_s, s->idle_s, s->roger ? "true" : "false",
           s->agc ? "true" : "false", s->tx_timeout_s, s->feed ? "true" : "false");
    jb_str(&j, "phase", st.phase);
    jb_str(&j, "why", st.why);
    jb_str(&j, "server", st.server);
    jb_raw(&j, "\"nodes\":%d,\"up\":%s,\"clock\":%s,", st.nodes, st.up ? "true" : "false",
           svx_time_known(now) ? "true" : "false");
    jb_raw(&j, "\"cert\":{");
    jb_str(&j, "state", cert_state(pi.state));
    jb_str(&j, "cn", pi.cn);
    jb_str(&j, "issuer", pi.issuer);
    jb_raw(&j, "\"from\":%lld,\"until\":%lld,\"key\":%s,\"csr\":%s,\"ca\":%s,"
               "\"pending\":%s,\"requested\":%lld}}",
           (long long)pi.not_before, (long long)pi.not_after,
           pi.have_key ? "true" : "false", pi.have_csr ? "true" : "false",
           pi.have_ca ? "true" : "false", pi.pending ? "true" : "false",
           (long long)pi.requested);
    free(s);

    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(r, j.b, (ssize_t)j.n);
    free(j.b);
    return e;
}

/* ---------------------------------------------------------------- forms */

static char *read_body(httpd_req_t *r, size_t max)
{
    const int total = r->content_len;
    if (total < 0 || (size_t)total > max) return NULL;
    char *b = malloc((size_t)total + 1);
    if (!b) return NULL;
    int got = 0;
    while (got < total) {
        int k = httpd_req_recv(r, b + got, total - got);
        if (k <= 0) { free(b); return NULL; }
        got += k;
    }
    b[got] = 0;
    return b;
}

/* One field, URL-decoded; false if absent. */
static bool field(const char *body, const char *key, char *out, size_t cap)
{
    char *raw = malloc(800);
    if (!raw) return false;
    bool ok = httpd_query_key_value(body, key, raw, 800) == ESP_OK;
    if (ok) {
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
    }
    free(raw);
    return ok;
}

static bool field_ul(const char *body, const char *key, unsigned long *v)
{
    char b[16];
    if (!field(body, key, b, sizeof b) || !b[0]) return false;
    char *end;
    unsigned long n = strtoul(b, &end, 10);
    if (end == b) return false;
    *v = n;
    return true;
}

static bool field_bool(const char *body, const char *key, bool *v)
{
    char b[8];
    if (!field(body, key, b, sizeof b)) return false;
    *v = !strcmp(b, "1") || !strcmp(b, "true") || !strcmp(b, "on");
    return true;
}

static esp_err_t reply(httpd_req_t *r, esp_err_t e, const char *why)
{
    if (e == ESP_OK) return httpd_resp_sendstr(r, "ok");
    httpd_resp_set_status(r, "400 Bad Request");
    return httpd_resp_sendstr(r, why && why[0] ? why : esp_err_to_name(e));
}

static esp_err_t svx_post(httpd_req_t *r)
{
    char *body = read_body(r, 4096);
    svx_settings_t *s = malloc(sizeof *s);
    if (!body || !s) { free(body); free(s); return reply(r, ESP_ERR_INVALID_SIZE, "body"); }
    svx_settings_get(s);
    field(body, "call",  s->call,     sizeof s->call);
    field(body, "email", s->email,    sizeof s->email);
    field(body, "loc",   s->location, sizeof s->location);
    field(body, "lat",   s->lat,      sizeof s->lat);
    field(body, "lon",   s->lon,      sizeof s->lon);
    field(body, "sw",    s->sw,       sizeof s->sw);
    field(body, "mon",   s->mon,      sizeof s->mon);
    unsigned long v;
    if (field_ul(body, "deftg", &v))  s->default_tg   = (uint32_t)v;
    if (field_ul(body, "linger", &v)) s->linger_s     = (uint16_t)(v > 300 ? 300 : v < 10 ? 10 : v);
    if (field_ul(body, "idle", &v))   s->idle_s       = (uint16_t)(v > 3600 ? 3600 : v);
    if (field_ul(body, "txto", &v))   s->tx_timeout_s = (uint16_t)(v > 3600 ? 3600 : v);
    field_bool(body, "lock",  &s->lock_on_start);
    field_bool(body, "roger", &s->roger);
    field_bool(body, "agc",   &s->agc);
    field_bool(body, "feed",  &s->feed);
    free(body);
    const char *why = NULL;
    esp_err_t e = svx_settings_set(s, &why);
    free(s);
    return reply(r, e, why);
}

static esp_err_t svx_enroll_post(httpd_req_t *r)
{
    char *body = read_body(r, 256);
    bool cancel = false;
    if (body) field_bool(body, "cancel", &cancel);
    free(body);
    const char *why = NULL;
    return reply(r, svx_enroll(!cancel, &why), why);
}

static esp_err_t svx_forget_post(httpd_req_t *r)
{
    svx_forget();
    return httpd_resp_sendstr(r, "ok");
}

static esp_err_t svx_csr_get(httpd_req_t *r)
{
    char *csr = svx_pki_csr_pem();
    if (!csr) {
        httpd_resp_set_status(r, "404 Not Found");
        return httpd_resp_sendstr(r, "no certificate request yet");
    }
    httpd_resp_set_type(r, "application/x-pem-file");
    httpd_resp_set_hdr(r, "Content-Disposition", "attachment; filename=\"svxconnect.csr\"");
    esp_err_t e = httpd_resp_sendstr(r, csr);
    free(csr);
    return e;
}

const httpd_uri_t svx_web_uris[] = {
    { .uri = "/api/svx",        .method = HTTP_GET,  .handler = svx_get },
    { .uri = "/api/svx",        .method = HTTP_POST, .handler = svx_post },
    { .uri = "/api/svx/enroll", .method = HTTP_POST, .handler = svx_enroll_post },
    { .uri = "/api/svx/forget", .method = HTTP_POST, .handler = svx_forget_post },
    { .uri = "/api/svx/csr",    .method = HTTP_GET,  .handler = svx_csr_get },
};
const size_t svx_web_uris_n = sizeof svx_web_uris / sizeof svx_web_uris[0];
