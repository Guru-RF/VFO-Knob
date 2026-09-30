/* SmartLink: see smartlink.h. Written from the protocol as AetherSDR speaks
 * it (src/core/backends/flex/SmartLinkClient.cpp and WanConnection.cpp,
 * github.com/aethersdr/AetherSDR), itself from FlexLib:
 *
 *   login   POST https://frtest.auth0.com/oauth/token, JSON: the password-
 *           realm grant with FlexLib's client id, scope offline_access for a
 *           refresh token -- kept -- and afterwards the refresh_token grant.
 *           Either gives an id_token, which is what the server takes.
 *   server  TLS smartlink.flexradio.com:443, a line each way:
 *             -> application register name=VFO-Knob platform=ESP32 token=<id>
 *             <- application info public_ip=...    application user_settings
 *                callsign=...    radio list <radio>|<radio>|..., each
 *                key=value: serial radio_name model status public_ip
 *                public_tls_port public_udp_port upnp_supported
 *                public_upnp_tls_port public_upnp_udp_port
 *             -> application connect serial=<s> hole_punch_port=<our UDP port>
 *             <- radio connect_ready handle=<h> serial=<s>
 *   radio   TLS <public_ip>:<tls port>. Its certificate is its own, self-
 *           signed: pinned on first use, as AetherSDR does. Then command 1,
 *           `wan validate handle=<h>`, and the API as on the LAN (V, H ...).
 *           UDP goes to its public UDP port: see flex_client.c.
 *
 * The TLS runs on a worker of its own, its stack in PSRAM: a handshake wants
 * more stack than the web server's or the flex task's, and neither may lose
 * it to internal RAM. The worker touches no flash; its callers save what it
 * brings back. */
#include "smartlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "nvs.h"
#include <fcntl.h>

static const char *TAG = "smartlink";

#define AUTH0_URL     "https://frtest.auth0.com/oauth/token"
#define AUTH0_CLIENT  "4Y9fEIIsVYyQo5u6jr7yBWc4lV5ugC2m"   /* FlexLib's */
#define SERVER_HOST   "smartlink.flexradio.com"
#define SERVER_PORT   443
#define NVS_NS        "sl"
#define RT_CAP        512            /* a refresh token, with room */
#define IDT_CAP       4096           /* an id_token: a JWT, 1-2 kB */
#define LINE_CAP      4096           /* the radio list is one line */
#define NET_MS        10000
#define LIST_MS       8000           /* registered: the radio list within */
#define READY_MS      12000          /* asked to connect: connect_ready within */
#define JOB_MS        45000
#define SRV_PING_MS   10000          /* FlexLib's keepalive to the server */

/* -------------------------------------------------------------- the state */

typedef struct {
    bool       on;
    char       email[64], callsign[16];
    char       rt[RT_CAP];                      /* "" = logged out */
    sl_radio_t list[SL_MAX];
    int        n;
    char       active[24];
    struct { char serial[24], fp[65]; } pin[SL_MAX * 2];
} sl_state_t;
EXT_RAM_BSS_ATTR static sl_state_t s;   /* internal RAM is the scarce one */
static SemaphoreHandle_t s_mx;                  /* s, between the tasks */

static void lock(void)
{
    if (!s_mx) sl_init();
    xSemaphoreTake(s_mx, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_mx); }

/* The list as one string for NVS: serial \t name \t model \t status \t ip
 * \t tls \t udp, a line each. */
static void list_to_nvs(nvs_handle_t h)
{
    char *b = heap_caps_calloc(1, SL_MAX * 200 + 1, MALLOC_CAP_SPIRAM);
    if (!b) return;
    size_t o = 0;
    for (int i = 0; i < s.n; i++) {
        const sl_radio_t *r = &s.list[i];
        o += snprintf(b + o, SL_MAX * 200 + 1 - o, "%s\t%s\t%s\t%s\t%s\t%u\t%u\n", r->serial,
                      r->name, r->model, r->status, r->ip, (unsigned)r->tls_port,
                      (unsigned)r->udp_port);
    }
    nvs_set_str(h, "list", b);
    free(b);
}

static void list_from(const char *b)
{
    s.n = 0;
    for (const char *p = b; p && *p && s.n < SL_MAX; ) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[200];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        char *f[7] = { line, "", "", "", "", "", "" };
        int k = 1;
        for (char *q = line; *q && k < 7; q++)
            if (*q == '\t') { *q = 0; f[k++] = q + 1; }
        if (f[0][0]) {
            sl_radio_t *r = &s.list[s.n++];
            memset(r, 0, sizeof *r);
            strlcpy(r->serial, f[0], sizeof r->serial);
            strlcpy(r->name, f[1], sizeof r->name);
            strlcpy(r->model, f[2], sizeof r->model);
            strlcpy(r->status, f[3], sizeof r->status);
            strlcpy(r->ip, f[4], sizeof r->ip);
            r->tls_port = (uint16_t)atoi(f[5]);
            r->udp_port = (uint16_t)atoi(f[6]);
        }
        p = eol ? eol + 1 : NULL;
    }
}

static void pins_to_nvs(nvs_handle_t h)
{
    const size_t cap = SL_MAX * 2 * 92 + 1;
    char *b = heap_caps_calloc(1, cap, MALLOC_CAP_SPIRAM);   /* off the flex task's stack */
    if (!b) return;
    size_t o = 0;
    for (size_t i = 0; i < sizeof s.pin / sizeof s.pin[0]; i++)
        if (s.pin[i].serial[0])
            o += snprintf(b + o, cap - o, "%s %s\n", s.pin[i].serial, s.pin[i].fp);
    nvs_set_str(h, "pins", b);
    free(b);
}

static const char *pin_of(const char *serial)
{
    for (size_t i = 0; i < sizeof s.pin / sizeof s.pin[0]; i++)
        if (s.pin[i].serial[0] && !strcmp(s.pin[i].serial, serial)) return s.pin[i].fp;
    return "";
}

static void pin_set(const char *serial, const char *fp)
{
    size_t free_i = SIZE_MAX;
    for (size_t i = 0; i < sizeof s.pin / sizeof s.pin[0]; i++) {
        if (!strcmp(s.pin[i].serial, serial)) { strlcpy(s.pin[i].fp, fp, sizeof s.pin[i].fp); return; }
        if (!s.pin[i].serial[0] && free_i == SIZE_MAX) free_i = i;
    }
    if (free_i == SIZE_MAX) free_i = 0;         /* full: the oldest goes */
    strlcpy(s.pin[free_i].serial, serial, sizeof s.pin[free_i].serial);
    strlcpy(s.pin[free_i].fp, fp, sizeof s.pin[free_i].fp);
}

void sl_init(void)
{
    if (s_mx) return;
    s_mx = xSemaphoreCreateMutex();
    memset(&s, 0, sizeof s);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t on = 0;
    nvs_get_u8(h, "on", &on);
    s.on = on;
    size_t len = sizeof s.email;    nvs_get_str(h, "email", s.email, &len);
    len = sizeof s.callsign;        nvs_get_str(h, "call", s.callsign, &len);
    len = sizeof s.rt;              nvs_get_str(h, "rt", s.rt, &len);
    len = sizeof s.active;          nvs_get_str(h, "active", s.active, &len);
    size_t n = 0;
    if (nvs_get_str(h, "list", NULL, &n) == ESP_OK && n > 1) {
        char *b = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (b && nvs_get_str(h, "list", b, &n) == ESP_OK) list_from(b);
        free(b);
    }
    char pins[SL_MAX * 2 * 92 + 1];
    len = sizeof pins;
    if (nvs_get_str(h, "pins", pins, &len) == ESP_OK) {
        size_t i = 0;
        for (char *p = strtok(pins, "\n"); p && i < sizeof s.pin / sizeof s.pin[0];
             p = strtok(NULL, "\n")) {
            char *sp = strchr(p, ' ');
            if (!sp) continue;
            *sp = 0;
            strlcpy(s.pin[i].serial, p, sizeof s.pin[i].serial);
            strlcpy(s.pin[i].fp, sp + 1, sizeof s.pin[i].fp);
            i++;
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%s; %d radio%s known%s%s", s.rt[0] ? "logged in" : "not logged in",
             s.n, s.n == 1 ? "" : "s", s.on ? ", offered on the dial" : "",
             s.active[0] ? ", one in use" : "");
}

bool sl_logged_in(void) { return s.rt[0] != 0; }
bool sl_enabled(void)   { return s.on && s.rt[0]; }
int  sl_count(void)     { return s.n; }

bool sl_get(int i, sl_radio_t *out)
{
    if (!s_mx || !out) return false;
    lock();
    const bool ok = i >= 0 && i < s.n;
    if (ok) *out = s.list[i];
    unlock();
    return ok;
}

void sl_active(char *serial, size_t cap)
{
    if (!s_mx) { if (cap) serial[0] = 0; return; }
    lock();
    strlcpy(serial, s.active, cap);
    unlock();
}

esp_err_t sl_set_active(const char *serial)
{
    lock();
    strlcpy(s.active, serial ? serial : "", sizeof s.active);
    unlock();
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_str(h, "active", serial ? serial : "");
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* ------------------------------------------------------------------ jobs */

typedef enum { J_LOGIN, J_LIST, J_OPEN } jkind_t;

typedef struct {
    jkind_t  kind;
    char     email[64], pass[64];
    char     rt[RT_CAP];                  /* in: the refresh token */
    char     serial[24];                  /* J_OPEN */
    uint16_t udp_port;
    char     pin[65];
    /* out */
    esp_err_t  err;
    char       why[96];
    char       rt_new[RT_CAP];            /* a newer refresh token, or "" */
    char       callsign[16];
    sl_radio_t list[SL_MAX];
    int        n;
    esp_tls_t *tls;
    char       fp[65];
    sl_radio_t radio;
    bool       finished, abandoned;
    SemaphoreHandle_t done;
} job_t;

static QueueHandle_t s_jobs;
static portMUX_TYPE  s_jmux = portMUX_INITIALIZER_UNLOCKED;

/* The server, kept open while a radio is ours through it -- the worker's
 * alone. Closing it straight after `connect_ready` lost the radio: it then
 * refused `wan validate` (500000B1) and hung up. AetherSDR keeps it too,
 * pinging it every 10 s. */
static esp_tls_t *s_srv;
static int64_t    s_srv_ping;

static void srv_close(void)
{
    if (s_srv) esp_tls_conn_destroy(s_srv);
    s_srv = NULL;
}

/* What the server sends meanwhile is read and let go; a ping every 10 s. */
static void srv_tick(void)
{
    if (!s_srv) return;
    char b[256];
    for (int i = 0; i < 16; i++) {
        const ssize_t k = esp_tls_conn_read(s_srv, b, sizeof b);
        if (k == ESP_TLS_ERR_SSL_WANT_READ || k == ESP_TLS_ERR_SSL_WANT_WRITE) break;
        if (k <= 0) {
            ESP_LOGW(TAG, "the SmartLink server hung up");
            srv_close();
            return;
        }
    }
    const int64_t now = esp_timer_get_time();
    if (now - s_srv_ping >= (int64_t)SRV_PING_MS * 1000) {
        s_srv_ping = now;
        static const char P[] = "ping from client\n";
        if (esp_tls_conn_write(s_srv, P, sizeof P - 1) < 0) {
            ESP_LOGW(TAG, "the SmartLink server hung up");
            srv_close();
        }
    }
}

static void fail(job_t *j, esp_err_t e, const char *why)
{
    j->err = e;
    strlcpy(j->why, why, sizeof j->why);
    ESP_LOGW(TAG, "%s", why);
}

/* The login service: an id_token, and a refresh token when it gives one. */
static bool auth0(job_t *j, char *idt)
{
    cJSON *req = cJSON_CreateObject();
    if (j->kind == J_LOGIN) {
        cJSON_AddStringToObject(req, "grant_type", "http://auth0.com/oauth/grant-type/password-realm");
        cJSON_AddStringToObject(req, "realm", "Username-Password-Authentication");
        cJSON_AddStringToObject(req, "username", j->email);
        cJSON_AddStringToObject(req, "password", j->pass);
        cJSON_AddStringToObject(req, "scope",
                                "openid email given_name family_name profile picture offline_access");
    } else {
        cJSON_AddStringToObject(req, "grant_type", "refresh_token");
        cJSON_AddStringToObject(req, "refresh_token", j->rt);
    }
    cJSON_AddStringToObject(req, "client_id", AUTH0_CLIENT);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    memset(j->pass, 0, sizeof j->pass);         /* not a moment longer */
    const size_t body_len = body ? strlen(body) : 0;
    if (!body) { fail(j, ESP_ERR_NO_MEM, "no memory for the login"); return false; }

    const esp_http_client_config_t hc = {
        .url = AUTH0_URL, .method = HTTP_METHOD_POST, .timeout_ms = NET_MS,
        .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t h = esp_http_client_init(&hc);
    char *resp = heap_caps_calloc(1, 8192, MALLOC_CAP_SPIRAM);
    bool ok = false;
    int status = 0, got = 0;
    if (h && resp) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
        const int len = (int)strlen(body);
        if (esp_http_client_open(h, len) == ESP_OK && esp_http_client_write(h, body, len) == len) {
            esp_http_client_fetch_headers(h);
            status = esp_http_client_get_status_code(h);
            int k;
            while (got < 8191 && (k = esp_http_client_read(h, resp + got, 8191 - got)) > 0) got += k;
            resp[got] = 0;
            ok = true;
        }
    }
    if (h) esp_http_client_cleanup(h);
    memset(body, 0, body_len);                  /* the password was in it */
    cJSON_free(body);
    if (!ok) {
        fail(j, ESP_FAIL, "cannot reach FlexRadio's login service");
        free(resp);
        return false;
    }
    cJSON *r = cJSON_Parse(resp);
    free(resp);
    const cJSON *id = r ? cJSON_GetObjectItem(r, "id_token") : NULL;
    const cJSON *rt = r ? cJSON_GetObjectItem(r, "refresh_token") : NULL;
    const cJSON *ed = r ? cJSON_GetObjectItem(r, "error_description") : NULL;
    if (status != 200 || !cJSON_IsString(id)) {
        char w[96];
        snprintf(w, sizeof w, "login refused: %s",
                 cJSON_IsString(ed) ? ed->valuestring : status ? "no token" : "no answer");
        fail(j, ESP_ERR_INVALID_RESPONSE, w);
        cJSON_Delete(r);
        return false;
    }
    strlcpy(idt, id->valuestring, IDT_CAP);
    if (cJSON_IsString(rt)) strlcpy(j->rt_new, rt->valuestring, sizeof j->rt_new);
    cJSON_Delete(r);
    return true;
}

static bool tls_write(esp_tls_t *t, const char *b, size_t n)
{
    while (n) {
        const ssize_t k = esp_tls_conn_write(t, b, n);
        if (k == ESP_TLS_ERR_SSL_WANT_WRITE || k == ESP_TLS_ERR_SSL_WANT_READ) continue;
        if (k <= 0) return false;
        b += k;
        n -= (size_t)k;
    }
    return true;
}

/* One line from the server, up to `ms`: 1 a line, 0 none in time, -1 gone. */
static int tls_line(esp_tls_t *t, char *line, size_t cap, size_t *have, int ms)
{
    const int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    for (;;) {
        char *nl = memchr(line, '\n', *have);
        if (nl) return 1;
        if (esp_timer_get_time() > until) return 0;
        if (*have >= cap - 1) *have = 0;         /* longer than we keep: dropped */
        const ssize_t k = esp_tls_conn_read(t, line + *have, cap - 1 - *have);
        if (k == ESP_TLS_ERR_SSL_WANT_READ || k == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
        if (k <= 0) return -1;
        *have += (size_t)k;
    }
}

/* A value up to the next space only: connect_ready's handle holds a '|'
 * of its own ("xxxxx|<24 hex>_<uuid>"), which kvs() would cut it at -- and a
 * radio sent the first five characters refuses `wan validate` (500000B1). */
static bool kv_word(const char *s, const char *key, char *out, size_t cap)
{
    const size_t kl = strlen(key);
    for (const char *p = s; (p = strstr(p, key)); p += kl) {
        if ((p != s && p[-1] != ' ') || p[kl] != '=') continue;
        const char *v = p + kl + 1;
        size_t n = strcspn(v, " \r\n");
        if (n >= cap) return false;              /* cut short is no handle at all */
        memcpy(out, v, n);
        out[n] = 0;
        return true;
    }
    if (cap) out[0] = 0;
    return false;
}

/* "key=value" out of a space-separated list; the API's 0x7F is a space. */
static bool kvs(const char *s, const char *key, char *out, size_t cap)
{
    const size_t kl = strlen(key);
    for (const char *p = s; (p = strstr(p, key)); p += kl) {
        if ((p != s && p[-1] != ' ' && p[-1] != '|') || p[kl] != '=') continue;
        const char *v = p + kl + 1;
        size_t n = strcspn(v, " |\r\n");
        if (n >= cap) n = cap - 1;
        for (size_t i = 0; i < n; i++) out[i] = v[i] == 0x7F ? ' ' : v[i];
        out[n] = 0;
        return true;
    }
    if (cap) out[0] = 0;
    return false;
}

/* A server line for the log (debug), a handle's or a token's value as its
 * length, the account's personal lines left out. */
static void log_line(const char *line)
{
    if (!strncmp(line, "application user_settings", 25)) {
        ESP_LOGD(TAG, "server: application user_settings (personal, omitted)");
        return;
    }
    char b[400];
    size_t o = 0;
    for (const char *p = line; *p && o + 12 < sizeof b; ) {
        if (!strncmp(p, "handle=", 7) || !strncmp(p, "token=", 6)) {
            const size_t kl = p[0] == 'h' ? 7 : 6;
            memcpy(b + o, p, kl);
            o += kl;
            p += kl;
            size_t n = strcspn(p, " ");
            o += (size_t)snprintf(b + o, sizeof b - o, "<%u chars>", (unsigned)n);
            p += n;
        } else {
            b[o++] = *p++;
        }
    }
    b[o] = 0;
    ESP_LOGD(TAG, "server: %s", b);
}

static void parse_list(job_t *j, char *body)
{
    j->n = 0;
    for (char *r = strtok(body, "|"); r && j->n < SL_MAX; r = strtok(NULL, "|")) {
        sl_radio_t *x = &j->list[j->n];
        memset(x, 0, sizeof *x);
        char v[16];
        if (!kvs(r, "serial", x->serial, sizeof x->serial)) continue;
        if (!kvs(r, "radio_name", x->name, sizeof x->name)) kvs(r, "name", x->name, sizeof x->name);
        kvs(r, "model", x->model, sizeof x->model);
        kvs(r, "status", x->status, sizeof x->status);
        kvs(r, "public_ip", x->ip, sizeof x->ip);
        /* A port forwarded by hand first, then UPnP's, as FlexLib picks. */
        long tls = kvs(r, "public_tls_port", v, sizeof v) ? atol(v) : -1;
        long udp = kvs(r, "public_udp_port", v, sizeof v) ? atol(v) : -1;
        const bool upnp = kvs(r, "upnp_supported", v, sizeof v) && !strcmp(v, "1");
        if (!(tls > 0 && udp > 0) && upnp) {
            tls = kvs(r, "public_upnp_tls_port", v, sizeof v) ? atol(v) : -1;
            udp = kvs(r, "public_upnp_udp_port", v, sizeof v) ? atol(v) : -1;
        }
        if (tls > 0 && tls < 65536 && udp > 0 && udp < 65536) {
            x->tls_port = (uint16_t)tls;
            x->udp_port = (uint16_t)udp;
        }
        j->n++;
    }
}

static void run(job_t *j)
{
    char *idt  = heap_caps_calloc(1, IDT_CAP, MALLOC_CAP_SPIRAM);
    char *line = heap_caps_calloc(1, LINE_CAP + IDT_CAP, MALLOC_CAP_SPIRAM);
    esp_tls_t *srv = NULL;
    if (!idt || !line) { fail(j, ESP_ERR_NO_MEM, "no memory for SmartLink"); goto out; }
    if (!auth0(j, idt)) goto out;

    /* The server: register, and its list of the account's radios. */
    srv = esp_tls_init();
    const esp_tls_cfg_t sc = { .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = NET_MS };
    if (!srv || esp_tls_conn_new_sync(SERVER_HOST, strlen(SERVER_HOST), SERVER_PORT, &sc, srv) != 1) {
        fail(j, ESP_FAIL, "cannot reach the SmartLink server");
        goto out;
    }
    int n = snprintf(line, LINE_CAP + IDT_CAP, "application register name=VFO-Knob platform=ESP32 token=%s\n", idt);
    if (!tls_write(srv, line, (size_t)n)) { fail(j, ESP_FAIL, "the SmartLink server hung up"); goto out; }
    size_t have = 0;
    bool listed = false;
    const int64_t until = esp_timer_get_time() + (int64_t)LIST_MS * 1000;
    while (!listed && esp_timer_get_time() < until) {
        const int r = tls_line(srv, line, LINE_CAP, &have, LIST_MS);
        if (r < 0) { fail(j, ESP_FAIL, "the SmartLink server hung up"); goto out; }
        if (r == 0) break;
        char *nl = memchr(line, '\n', have);
        *nl = 0;
        log_line(line);
        if (!strncmp(line, "radio list", 10)) {
            parse_list(j, line + 10);
            listed = true;
        } else if (!strncmp(line, "application user_settings", 25)) {
            kvs(line, "callsign", j->callsign, sizeof j->callsign);
        } else if (!strncmp(line, "application registration_invalid", 32)) {
            fail(j, ESP_ERR_INVALID_STATE, "SmartLink refused the login: log in again");
            goto out;
        }
        const size_t used = (size_t)(nl - line) + 1;
        memmove(line, line + used, have - used);
        have -= used;
    }
    if (!listed) { fail(j, ESP_ERR_TIMEOUT, "no radio list from SmartLink"); goto out; }
    ESP_LOGI(TAG, "%d radio%s on the account", j->n, j->n == 1 ? "" : "s");
    if (j->kind != J_OPEN) { j->err = ESP_OK; goto out; }

    /* Connect: the radio told our UDP port, and a handle to show it. */
    const sl_radio_t *x = NULL;
    for (int i = 0; i < j->n; i++)
        if (!strcmp(j->list[i].serial, j->serial)) x = &j->list[i];
    if (!x) { fail(j, ESP_ERR_NOT_FOUND, "that radio is no longer on the account"); goto out; }
    j->radio = *x;
    if (!x->tls_port) {
        fail(j, ESP_ERR_NOT_SUPPORTED, "the radio is reachable only by hole punching, "
                                       "which the knob does not do yet");
        goto out;
    }
    n = snprintf(line, LINE_CAP, "application connect serial=%s hole_punch_port=%u\n",
                 j->serial, (unsigned)j->udp_port);
    have = 0;
    if (!tls_write(srv, line, (size_t)n)) { fail(j, ESP_FAIL, "the SmartLink server hung up"); goto out; }
    char handle[160] = "";
    const int64_t until2 = esp_timer_get_time() + (int64_t)READY_MS * 1000;
    while (!handle[0] && esp_timer_get_time() < until2) {
        const int r = tls_line(srv, line, LINE_CAP, &have, READY_MS);
        if (r <= 0) break;
        char *nl = memchr(line, '\n', have);
        *nl = 0;
        log_line(line);
        if (!strncmp(line, "radio connect_ready", 19)) kv_word(line, "handle", handle, sizeof handle);
        const size_t used = (size_t)(nl - line) + 1;
        memmove(line, line + used, have - used);
        have -= used;
    }
    if (!handle[0]) { fail(j, ESP_ERR_TIMEOUT, "the radio did not answer through SmartLink"); goto out; }

    /* The radio: its own certificate, pinned the first time. */
    esp_tls_t *t = esp_tls_init();
    const esp_tls_cfg_t rc = { .timeout_ms = NET_MS, .skip_common_name = true };
    if (!t || esp_tls_conn_new_sync(x->ip, strlen(x->ip), x->tls_port, &rc, t) != 1) {
        if (t) esp_tls_conn_destroy(t);
        fail(j, ESP_FAIL, "cannot reach the radio's SmartLink port");
        goto out;
    }
    const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(esp_tls_get_ssl_context(t));
    if (crt) {
        uint8_t d[32];
        mbedtls_sha256(crt->raw.p, crt->raw.len, d, 0);
        for (int i = 0; i < 32; i++) snprintf(j->fp + 2 * i, 3, "%02x", d[i]);
    }
    if (j->pin[0] && strcmp(j->pin, j->fp)) {
        esp_tls_conn_destroy(t);
        fail(j, ESP_ERR_INVALID_STATE, "the radio's certificate is not the one it had: "
                                       "log in to SmartLink again to accept it");
        goto out;
    }
    n = snprintf(line, LINE_CAP, "C1|wan validate handle=%s\n", handle);
    if (!tls_write(t, line, (size_t)n)) {
        esp_tls_conn_destroy(t);
        fail(j, ESP_FAIL, "the radio hung up");
        goto out;
    }
    j->tls = t;
    j->err = ESP_OK;
    ESP_LOGI(TAG, "connected to %s through SmartLink (%s:%u)", x->name, x->ip,
             (unsigned)x->tls_port);
    /* The server stays: see s_srv. Read without waiting from here on. */
    int sfd = -1;
    esp_tls_get_conn_sockfd(srv, &sfd);
    if (sfd >= 0) fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL, 0) | O_NONBLOCK);
    s_srv = srv;
    s_srv_ping = esp_timer_get_time();
    srv = NULL;
out:
    if (srv) esp_tls_conn_destroy(srv);
    free(idt);
    free(line);
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        job_t *j;
        if (xQueueReceive(s_jobs, &j, pdMS_TO_TICKS(1000)) != pdTRUE) {
            srv_tick();
            continue;
        }
        j->err = ESP_FAIL;
        if (j->kind == J_LIST && s_srv) {
            /* Connected through it now: a second session could cost the
             * first. The list the knob has stands. */
            j->err = ESP_OK;
        } else {
            srv_close();                         /* one session at a time */
            run(j);
        }
        taskENTER_CRITICAL(&s_jmux);
        const bool gone = j->abandoned;
        j->finished = true;
        taskEXIT_CRITICAL(&s_jmux);
        if (gone) {                              /* its caller gave up */
            if (j->tls) esp_tls_conn_destroy(j->tls);
            vSemaphoreDelete(j->done);
            free(j);
        } else {
            xSemaphoreGive(j->done);
        }
    }
}

/* A job done by the worker: the job back, or NULL when it did not finish in
 * time (the worker then frees it). */
static job_t *job_run(job_t *j)
{
    if (!s_jobs) {
        s_jobs = xQueueCreate(2, sizeof(job_t *));
        /* Core 0, low: a handshake's arithmetic must not cost the dial or
         * the audio (core 1) a frame. */
        if (!s_jobs || xTaskCreatePinnedToCoreWithCaps(worker, "slink", 12288, NULL, 3, NULL, 0,
                                                        MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGE(TAG, "no SmartLink worker");
            return j;                            /* err is ESP_FAIL */
        }
    }
    j->done = xSemaphoreCreateBinary();
    if (!j->done || xQueueSend(s_jobs, &j, 0) != pdTRUE) {
        strlcpy(j->why, "SmartLink is busy", sizeof j->why);
        return j;
    }
    if (xSemaphoreTake(j->done, pdMS_TO_TICKS(JOB_MS)) == pdTRUE) {
        vSemaphoreDelete(j->done);
        return j;
    }
    taskENTER_CRITICAL(&s_jmux);
    const bool fin = j->finished;
    if (!fin) j->abandoned = true;
    taskEXIT_CRITICAL(&s_jmux);
    if (!fin) return NULL;
    xSemaphoreTake(j->done, portMAX_DELAY);      /* finished just now */
    vSemaphoreDelete(j->done);
    return j;
}

static job_t *job_new(jkind_t kind)
{
    job_t *j = heap_caps_calloc(1, sizeof *j, MALLOC_CAP_SPIRAM);
    if (j) {
        j->kind = kind;
        j->err = ESP_FAIL;
        lock();
        strlcpy(j->rt, s.rt, sizeof j->rt);
        unlock();
    }
    return j;
}

/* What a job brought back that is worth keeping: the refresh token, who is
 * logged in, the list. From a task with an internal stack. */
static void keep(const job_t *j, const char *email)
{
    lock();
    if (j->rt_new[0]) strlcpy(s.rt, j->rt_new, sizeof s.rt);
    if (email) strlcpy(s.email, email, sizeof s.email);
    if (j->callsign[0]) strlcpy(s.callsign, j->callsign, sizeof s.callsign);
    if (j->n) {
        memcpy(s.list, j->list, sizeof s.list);
        s.n = j->n;
    }
    unlock();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "rt", s.rt);
    nvs_set_str(h, "email", s.email);
    nvs_set_str(h, "call", s.callsign);
    list_to_nvs(h);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t sl_open(uint16_t udp_port, sl_link_t *out, char *why, size_t cap)
{
    memset(out, 0, sizeof *out);
    job_t *j = job_new(J_OPEN);
    if (!j) { strlcpy(why, "no memory", cap); return ESP_ERR_NO_MEM; }
    lock();
    strlcpy(j->serial, s.active, sizeof j->serial);
    strlcpy(j->pin, pin_of(s.active), sizeof j->pin);
    unlock();
    j->udp_port = udp_port;
    if (!j->rt[0] || !j->serial[0]) {
        free(j);
        strlcpy(why, "not logged in to SmartLink", cap);
        return ESP_ERR_INVALID_STATE;
    }
    j = job_run(j);
    if (!j) { strlcpy(why, "SmartLink took too long", cap); return ESP_ERR_TIMEOUT; }
    const esp_err_t e = j->err;
    strlcpy(why, j->why, cap);
    if (e == ESP_OK || j->rt_new[0] || j->n) keep(j, NULL);
    if (e == ESP_OK) {
        if (!j->pin[0] && j->fp[0]) {            /* the first time: pinned */
            lock();
            pin_set(j->serial, j->fp);
            unlock();
            nvs_handle_t h;
            if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                pins_to_nvs(h);
                nvs_commit(h);
                nvs_close(h);
            }
            ESP_LOGI(TAG, "pinned %s's certificate (sha256 %.16s...)", j->serial, j->fp);
        }
        out->tls = j->tls;
        out->udp.sin_family = AF_INET;
        out->udp.sin_port = htons(j->radio.udp_port);
        inet_aton(j->radio.ip, &out->udp.sin_addr);
        strlcpy(out->name, j->radio.name, sizeof out->name);
    }
    free(j);
    return e;
}

/* ------------------------------------------------------------------ web */

static bool body_of(httpd_req_t *r, char *b, size_t cap)
{
    const int total = r->content_len;
    if (total < 0 || total >= (int)cap) return false;
    int got = 0;
    while (got < total) {
        const int k = httpd_req_recv(r, b + got, total - got);
        if (k <= 0) return false;
        got += k;
    }
    b[got] = 0;
    return true;
}

/* A form field, URL-decoded; false when absent. */
static bool field(const char *body, const char *key, char *out, size_t cap)
{
    char raw[200];
    if (httpd_query_key_value(body, key, raw, sizeof raw) != ESP_OK) return false;
    size_t o = 0;
    for (size_t i = 0; raw[i] && o + 1 < cap; i++) {
        if (raw[i] == '+') out[o++] = ' ';
        else if (raw[i] == '%' && raw[i + 1] && raw[i + 2]) {
            const char h[3] = { raw[i + 1], raw[i + 2], 0 };
            out[o++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else out[o++] = raw[i];
    }
    out[o] = 0;
    return true;
}

static void esc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 2 < cap; in++) {
        if ((unsigned char)*in < 0x20) continue;
        if (*in == '"' || *in == '\\') out[o++] = '\\';
        out[o++] = *in;
    }
    out[o] = 0;
}

static esp_err_t send_state(httpd_req_t *r, esp_err_t e, const char *why)
{
    char *j = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!j) return httpd_resp_send_500(r);
    char em[96], call[32], w[160];
    lock();
    esc(s.email, em, sizeof em);
    esc(s.callsign, call, sizeof call);
    esc(why ? why : "", w, sizeof w);
    int o = snprintf(j, 2048, "{\"ok\":%s,\"error\":\"%s\",\"logged_in\":%s,\"email\":\"%s\","
                     "\"callsign\":\"%s\",\"enabled\":%s,\"active\":\"%s\",\"radios\":[",
                     e == ESP_OK ? "true" : "false", w, s.rt[0] ? "true" : "false", em, call,
                     s.on ? "true" : "false", s.active);
    for (int i = 0; i < s.n && o < 1800; i++) {
        char nm[52], md[36], st[36];
        esc(s.list[i].name, nm, sizeof nm);
        esc(s.list[i].model, md, sizeof md);
        esc(s.list[i].status, st, sizeof st);
        o += snprintf(j + o, 2048 - o, "%s{\"serial\":\"%s\",\"name\":\"%s\",\"model\":\"%s\","
                      "\"status\":\"%s\",\"reachable\":%s}", i ? "," : "", s.list[i].serial,
                      nm, md, st, s.list[i].tls_port ? "true" : "false");
    }
    unlock();
    snprintf(j + o, 2048 - o, "]}");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    const esp_err_t ret = httpd_resp_sendstr(r, j);
    free(j);
    return ret;
}

static esp_err_t web_get(httpd_req_t *r) { return send_state(r, ESP_OK, NULL); }

/* Log in: the password goes to FlexRadio's login service and is forgotten;
 * the refresh token it gives is what the knob keeps. */
static esp_err_t web_login(httpd_req_t *r)
{
    char body[600];
    job_t *j = job_new(J_LOGIN);
    if (!j) return httpd_resp_send_500(r);
    if (!body_of(r, body, sizeof body) || !field(body, "email", j->email, sizeof j->email) ||
        !field(body, "password", j->pass, sizeof j->pass) || !j->email[0] || !j->pass[0]) {
        memset(body, 0, sizeof body);
        free(j);
        return send_state(r, ESP_ERR_INVALID_ARG, "an email address and a password");
    }
    memset(body, 0, sizeof body);
    char email[64];
    strlcpy(email, j->email, sizeof email);
    ESP_LOGI(TAG, "logging in");
    j = job_run(j);
    if (!j) return send_state(r, ESP_ERR_TIMEOUT, "SmartLink took too long");
    const esp_err_t e = j->err;
    char why[96];
    strlcpy(why, j->why, sizeof why);
    if (e == ESP_OK) {
        keep(j, email);
        /* A new login is the way past a certificate that changed: the
         * radios' are pinned afresh on their next connection. */
        lock();
        memset(s.pin, 0, sizeof s.pin);
        unlock();
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            pins_to_nvs(h);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "logged in; %d radio%s", j->n, j->n == 1 ? "" : "s");
    }
    free(j);
    return send_state(r, e, e == ESP_OK ? NULL : why);
}

static esp_err_t web_logout(httpd_req_t *r)
{
    lock();
    memset(s.rt, 0, sizeof s.rt);
    s.email[0] = s.callsign[0] = 0;
    s.n = 0;
    s.active[0] = 0;
    memset(s.pin, 0, sizeof s.pin);
    unlock();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_set_u8(h, "on", s.on);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "logged out");
    return send_state(r, ESP_OK, NULL);
}

/* enabled=0/1: offered on the dial or not; refresh=1: the list asked again. */
static esp_err_t web_post(httpd_req_t *r)
{
    char body[80], v[8];
    if (!body_of(r, body, sizeof body)) return send_state(r, ESP_ERR_INVALID_ARG, "body size");
    if (field(body, "enabled", v, sizeof v)) {
        s.on = v[0] == '1';
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "on", s.on);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    if (field(body, "refresh", v, sizeof v) && v[0] == '1') {
        job_t *j = job_new(J_LIST);
        if (!j) return httpd_resp_send_500(r);
        if (!j->rt[0]) { free(j); return send_state(r, ESP_ERR_INVALID_STATE, "not logged in"); }
        j = job_run(j);
        if (!j) return send_state(r, ESP_ERR_TIMEOUT, "SmartLink took too long");
        const esp_err_t e = j->err;
        char why[96];
        strlcpy(why, j->why, sizeof why);
        if (e == ESP_OK || j->rt_new[0]) keep(j, NULL);
        free(j);
        return send_state(r, e, e == ESP_OK ? NULL : why);
    }
    return send_state(r, ESP_OK, NULL);
}

size_t sl_web_endpoints(const httpd_uri_t **out)
{
    static const httpd_uri_t U[] = {
        { .uri = "/api/smartlink",        .method = HTTP_GET,  .handler = web_get },
        { .uri = "/api/smartlink",        .method = HTTP_POST, .handler = web_post },
        { .uri = "/api/smartlink/login",  .method = HTTP_POST, .handler = web_login },
        { .uri = "/api/smartlink/logout", .method = HTTP_POST, .handler = web_logout },
    };
    *out = U;
    return sizeof U / sizeof U[0];
}
