/* Google Contacts: the starred ones, as the telephone's favourites.
 *
 * Signing in happens in the browser, from the configuration page: Google's
 * device flow does not offer the contacts, and Google returns only to an
 * https address -- never to a knob's on the LAN. So it returns to a small
 * static page (docs/google.html, wherever it is served) that hands the code
 * to the knob whose address came along in `state`. Here the code becomes a
 * refresh token, kept in NVS, and the starred contacts -- People API, their
 * names and numbers -- become the favourites: at start, every six hours, and
 * whenever the page asks.
 *
 * The OAuth client is the user's own (a Google Cloud project with the People
 * API, its id and secret given on the page): nothing of Google's is built in.
 * Everything runs on a task of its own, created for the work and gone after:
 * TLS wants the stack, and the settings are written to flash. */
#include "contacts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nvs.h"

#include "phone_priv.h"

static const char *TAG = "contacts";

#define NVS_NS      "phone"
#define SYNC_MS     (6 * 3600 * 1000)
#define RETRY_MS    (10 * 60 * 1000)    /* after a sync that failed */
#define BODY_CAP    (96 * 1024)    /* in PSRAM; a batch of people is ~30 KB */
#define BATCH       20
#define SCOPE       "https://www.googleapis.com/auth/contacts.readonly"
#define DEFAULT_RELAY "https://vfoknob.com/google.html"     /* docs/google.html */

static struct {
    char cid[96], csec[64], relay[128];
    char rtok[256];
    char code[256];             /* from the browser, to be exchanged */
    char nonce[12];             /* this sign-in's, in `state` and back */
    volatile bool busy, due;
    char state[16], why[64];
    int  count;
    uint64_t t_last;            /* the last sync that worked */
    uint64_t t_try;             /* the last one begun */
    uint64_t hold_until;        /* no task to be had: not before this */
    bool last_ok;
} G;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static inline uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void set_state(const char *st, const char *why)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(G.state, st, sizeof G.state);
    strlcpy(G.why, why ? why : "", sizeof G.why);
    taskEXIT_CRITICAL(&s_lock);
}

static void nvs_str(nvs_handle_t h, const char *k, char *out, size_t cap)
{
    size_t n = cap;
    if (nvs_get_str(h, k, out, &n) != ESP_OK) out[0] = 0;
}

/* cJSON in PSRAM. A reply of Google's parses into thousands of small nodes,
 * which malloc would put in internal RAM -- the WiFi's -- until none was left.
 * The hooks are cJSON's only, global: every cJSON of this firmware's in PSRAM. */
static void *ext_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

void contacts_load(void)
{
    nvs_handle_t h;
    cJSON_InitHooks(&(cJSON_Hooks){ .malloc_fn = ext_malloc, .free_fn = free });
    strlcpy(G.relay, DEFAULT_RELAY, sizeof G.relay);
    strlcpy(G.state, "idle", sizeof G.state);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    nvs_str(h, "gcid", G.cid, sizeof G.cid);
    nvs_str(h, "gcsec", G.csec, sizeof G.csec);
    nvs_str(h, "grtok", G.rtok, sizeof G.rtok);
    char relay[128];
    nvs_str(h, "grelay", relay, sizeof relay);
    if (relay[0]) strlcpy(G.relay, relay, sizeof G.relay);
    nvs_close(h);
    if (G.rtok[0]) G.due = true;                  /* synced once registered */
}

static esp_err_t save(const char *k, const char *v)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = v[0] ? nvs_set_str(h, k, v) : nvs_erase_key(h, k);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* What the page gave, without what would break the JSON or a URL: no
 * quotes, backslashes, spaces or control characters. */
static void clean(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s && o + 1 < cap; s++)
        if (*s > ' ' && *s < 0x7f && *s != '"' && *s != '\\') out[o++] = *s;
    out[o] = 0;
}

esp_err_t contacts_set_client(const char *cid, const char *csec, const char *relay)
{
    if (cid) clean(cid, G.cid, sizeof G.cid);
    if (csec && csec[0]) clean(csec, G.csec, sizeof G.csec);
    if (relay && relay[0]) {
        char r[sizeof G.relay];
        clean(relay, r, sizeof r);
        if (strncmp(r, "https://", 8)) return ESP_ERR_INVALID_ARG;
        strlcpy(G.relay, r, sizeof G.relay);
    }
    esp_err_t e = save("gcid", G.cid);
    if (e == ESP_OK) e = save("gcsec", G.csec);
    if (e == ESP_OK) e = save("grelay", strcmp(G.relay, DEFAULT_RELAY) ? G.relay : "");
    return e;
}

void contacts_forget(void)
{
    G.rtok[0] = 0;
    save("grtok", "");
    set_state("idle", "signed out");
    ESP_LOGI(TAG, "signed out of Google");
}

/* ---------------------------------------------------------------- flash, later */

/* The sync runs on a PSRAM stack -- TLS wants 8 kB of one, and internal RAM
 * is the WiFi's -- and a task on a PSRAM stack must not write flash. What
 * it saves, a short task with an internal stack writes. */
typedef struct {
    void (*fn)(void *);
    void  *arg;
} later_t;

static void later_task(void *p)
{
    const later_t l = *(later_t *)p;
    free(p);
    l.fn(l.arg);
    vTaskDelete(NULL);
}

static bool on_internal(void (*fn)(void *), void *arg)
{
    later_t *l = malloc(sizeof *l);
    if (!l) return false;
    l->fn  = fn;
    l->arg = arg;
    if (xTaskCreatePinnedToCore(later_task, "cflash", 3584, l, 3, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "no memory to save just now");
        free(l);
        return false;
    }
    return true;
}

static void save_token(void *arg)
{
    (void)arg;
    save("grtok", G.rtok);
}

typedef struct {
    phone_fav_t *f;
    int          n;
} favs_job_t;

static void save_favs(void *arg)
{
    favs_job_t *j = arg;
    phone_favs_set(j->f, j->n);
    free(j->f);
    free(j);
}

/* ---------------------------------------------------------------- HTTPS */

typedef struct {
    char  *buf;
    size_t n;
    bool   cut;                 /* longer than BODY_CAP: not to be read */
} body_t;

static esp_err_t on_data(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b && b->buf) {
        const size_t k = e->data_len < BODY_CAP - 1 - b->n ? (size_t)e->data_len : BODY_CAP - 1 - b->n;
        if (k < (size_t)e->data_len) b->cut = true;
        memcpy(b->buf + b->n, e->data, k);
        b->n += k;
        b->buf[b->n] = 0;
    }
    return ESP_OK;
}

/* One connection a host for a whole sync: a TLS handshake here is seconds of
 * software ECC and kilobytes of the WiFi's internal RAM, so the batches of
 * people all go down the one the group list opened. */
static esp_http_client_handle_t s_conn;
static char                     s_conn_host[64];

static void conn_close(void)
{
    if (s_conn) esp_http_client_cleanup(s_conn);
    s_conn = NULL;
    s_conn_host[0] = 0;
}

static void host_of(const char *url, char *out, size_t cap)
{
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t n = strcspn(h, "/?");
    if (n >= cap) n = cap - 1;
    memcpy(out, h, n);
    out[n] = 0;
}

/* A request; the body back in `b`, the status returned (-1: no answer). */
static int request(const char *url, const char *form, const char *bearer, body_t *b)
{
    char host[64];
    host_of(url, host, sizeof host);
    if (s_conn && strcmp(host, s_conn_host)) conn_close();
    if (!s_conn) {
        esp_http_client_config_t c = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 15000,
            .event_handler = on_data,
            .buffer_size = 2048,
            /* The request line and headers in one: a batch of twenty people
             * is a 1 kB address, and Google's token rides along. */
            .buffer_size_tx = 2048,
        };
        s_conn = esp_http_client_init(&c);
        if (!s_conn) return -1;
        strlcpy(s_conn_host, host, sizeof s_conn_host);
    } else {
        esp_http_client_set_url(s_conn, url);
    }
    esp_http_client_handle_t h = s_conn;
    esp_http_client_set_user_data(h, b);
    b->n = 0;
    b->buf[0] = 0;
    b->cut = false;
    esp_http_client_set_method(h, form ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(h, "User-Agent", "VFO-Knob");
    if (bearer) {
        char auth[1200];
        snprintf(auth, sizeof auth, "Bearer %s", bearer);
        esp_http_client_set_header(h, "Authorization", auth);
    } else {
        esp_http_client_delete_header(h, "Authorization");
    }
    if (form) esp_http_client_set_header(h, "Content-Type", "application/x-www-form-urlencoded");
    else      esp_http_client_delete_header(h, "Content-Type");
    esp_http_client_set_post_field(h, form, form ? (int)strlen(form) : 0);
    esp_err_t e = ESP_FAIL;
    /* A connection that fails -- a TLS handshake Google gave up on -- is
     * tried twice more before it counts. */
    for (int i = 0; i < 3; i++) {
        e = esp_http_client_perform(h);
        if (e != ESP_ERR_HTTP_CONNECT) break;
        ESP_LOGW(TAG, "%s not reached (%s); free internal %u, largest %u: again", host,
                 esp_err_to_name(e), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        vTaskDelay(pdMS_TO_TICKS(1500));
        b->n = 0;
        b->buf[0] = 0;
        b->cut = false;
    }
    const int status = e == ESP_OK ? esp_http_client_get_status_code(h) : -1;
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", host, esp_err_to_name(e));
        conn_close();
    } else if (status != 200) {
        ESP_LOGW(TAG, "%s: HTTP %d, %u bytes: %.80s", host, status, (unsigned)b->n, b->buf);
    }
    if (b->cut) {
        ESP_LOGW(TAG, "a reply longer than %u bytes", (unsigned)BODY_CAP);
        return -2;
    }
    return status;
}

static void url_enc(const char *s, char *out, size_t cap)
{
    static const char H[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < cap; s++) {
        const unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = H[c >> 4];
            out[o++] = H[c & 15];
        }
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------- tokens */

static bool exchange_code(body_t *b, char *access, size_t cap)
{
    char *form = malloc(1400), cid[160], sec[128], code[400], relay[256];
    if (!form) return false;
    url_enc(G.cid, cid, sizeof cid);
    url_enc(G.csec, sec, sizeof sec);
    url_enc(G.code, code, sizeof code);
    url_enc(G.relay, relay, sizeof relay);
    snprintf(form, 1400, "code=%s&client_id=%s&client_secret=%s&redirect_uri=%s&grant_type=authorization_code",
             code, cid, sec, relay);
    G.code[0] = 0;
    const int st = request("https://oauth2.googleapis.com/token", form, NULL, b);
    free(form);
    cJSON *j = cJSON_Parse(b->buf);
    const char *rt = cJSON_GetStringValue(cJSON_GetObjectItem(j, "refresh_token"));
    const char *at = cJSON_GetStringValue(cJSON_GetObjectItem(j, "access_token"));
    const char *er = cJSON_GetStringValue(cJSON_GetObjectItem(j, "error_description"));
    bool ok = st == 200 && rt && at;
    if (ok) {
        strlcpy(G.rtok, rt, sizeof G.rtok);
        on_internal(save_token, NULL);
        strlcpy(access, at, cap);
        ESP_LOGI(TAG, "signed in to Google");
    } else if (st < 0) {
        ESP_LOGW(TAG, "sign-in: Google not reached");
        set_state("failed", "Google not reached: sign in again");
    } else {
        ESP_LOGW(TAG, "sign-in refused: %d %s", st, er ? er : "");
        set_state("failed", er ? er : "Google refused the sign-in");
    }
    cJSON_Delete(j);
    return ok;
}

static bool refresh_access(body_t *b, char *access, size_t cap)
{
    char *form = malloc(800), cid[160], sec[128], rt[400];
    if (!form) return false;
    url_enc(G.cid, cid, sizeof cid);
    url_enc(G.csec, sec, sizeof sec);
    url_enc(G.rtok, rt, sizeof rt);
    snprintf(form, 800, "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token", cid, sec, rt);
    const int st = request("https://oauth2.googleapis.com/token", form, NULL, b);
    free(form);
    cJSON *j = cJSON_Parse(b->buf);
    const char *at = cJSON_GetStringValue(cJSON_GetObjectItem(j, "access_token"));
    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(j, "error"));
    const bool ok = st == 200 && at;
    if (ok) {
        strlcpy(access, at, cap);
    } else if (err && !strcmp(err, "invalid_grant")) {
        /* Revoked, or expired: a Google Cloud app still in Testing gives
         * refresh tokens a week. Signed in again from the page. */
        G.rtok[0] = 0;
        on_internal(save_token, NULL);
        set_state("failed", "Google's sign-in expired: sign in again");
    } else {
        set_state("failed", st < 0 ? "Google not reached" : "Google refused the token");
    }
    cJSON_Delete(j);
    return ok;
}

/* ---------------------------------------------------------------- the sync */

static int cmp_fav(const void *a, const void *b)
{
    return strcasecmp(((const phone_fav_t *)a)->name, ((const phone_fav_t *)b)->name);
}

static void number_of(const cJSON *pn, char *out, size_t cap)
{
    const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(pn, "canonicalForm"));
    const char *v = c ? c : cJSON_GetStringValue(cJSON_GetObjectItem(pn, "value"));
    size_t o = 0;
    for (; v && *v && o + 1 < cap; v++)
        if ((*v >= '0' && *v <= '9') || (*v == '+' && o == 0)) out[o++] = *v;
    out[o] = 0;
}

static int sync_starred(body_t *b, const char *access)
{
    int st = request("https://people.googleapis.com/v1/contactGroups/starred?maxMembers=200", NULL, access, b);
    if (st != 200) {
        set_state("failed", st < 0 ? "Google not reached" : "the starred contacts were refused");
        return -1;
    }
    cJSON *g = cJSON_Parse(b->buf);
    const cJSON *members = cJSON_GetObjectItem(g, "memberResourceNames");
    const int n = cJSON_GetArraySize(members);
    phone_fav_t *f = calloc(PHONE_FAV_MAX, sizeof *f);
    char *url = malloc(4096);
    int k = 0;
    for (int at = 0; f && url && at < n && k < PHONE_FAV_MAX; at += BATCH) {
        size_t u = (size_t)snprintf(url, 4096, "https://people.googleapis.com/v1/people:batchGet?personFields=names,phoneNumbers");
        for (int i = at; i < n && i < at + BATCH && u < 4000; i++) {
            const char *rn = cJSON_GetStringValue(cJSON_GetArrayItem(members, i));
            if (rn) u += (size_t)snprintf(url + u, 4096 - u, "&resourceNames=%s", rn);
        }
        st = request(url, NULL, access, b);
        if (st != 200 && st != 401 && st != 403) {
            /* Once more, on a fresh connection: a batch that failed is
             * likelier the line's fault than the list's. */
            conn_close();
            vTaskDelay(pdMS_TO_TICKS(1000));
            st = request(url, NULL, access, b);
        }
        if (st != 200) break;
        cJSON *r = cJSON_Parse(b->buf);
        if (!r) {                       /* half a list is no list */
            ESP_LOGW(TAG, "a batch of people unreadable: %u bytes", (unsigned)b->n);
            st = -1;
            break;
        }
        const cJSON *resp;
        cJSON_ArrayForEach(resp, cJSON_GetObjectItem(r, "responses")) {
            const cJSON *person = cJSON_GetObjectItem(resp, "person");
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(
                cJSON_GetArrayItem(cJSON_GetObjectItem(person, "names"), 0), "displayName"));
            const cJSON *nums = cJSON_GetObjectItem(person, "phoneNumbers");
            const int nn = cJSON_GetArraySize(nums);
            for (int j = 0; j < nn && k < PHONE_FAV_MAX; j++) {
                const cJSON *pn = cJSON_GetArrayItem(nums, j);
                number_of(pn, f[k].number, sizeof f[k].number);
                if (!f[k].number[0]) continue;
                const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(pn, "formattedType"));
                /* More than one number: each by its kind, after the name. */
                if (nn > 1 && type) snprintf(f[k].name, sizeof f[k].name, "%.14s %.8s", name ? name : f[k].number, type);
                else                strlcpy(f[k].name, name ? name : f[k].number, sizeof f[k].name);
                k++;
            }
        }
        cJSON_Delete(r);
    }
    cJSON_Delete(g);
    free(url);
    if (!f || st != 200) {
        free(f);
        set_state("failed", "the contacts could not be read");
        return -1;
    }
    qsort(f, (size_t)k, sizeof *f, cmp_fav);
    favs_job_t *j = malloc(sizeof *j);
    if (j) {
        j->f = f;                               /* saved, then freed, there */
        j->n = k;
        if (!on_internal(save_favs, j)) {
            free(f);
            free(j);
        }
    } else {
        free(f);
    }
    if (n && !k) ESP_LOGW(TAG, "%d starred contacts, none with a number", n);
    return k;
}

static void sync_task(void *arg)
{
    (void)arg;
    body_t b = { .buf = malloc(BODY_CAP) };
    char *access = malloc(1100);
    ESP_LOGI(TAG, "sync: free internal %u, largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (b.buf && access) {
        set_state("syncing", "");
        bool have = false;
        if (G.code[0]) have = exchange_code(&b, access, 1100);
        else if (G.rtok[0]) have = refresh_access(&b, access, 1100);
        else set_state("idle", "not signed in");
        if (have) {
            const int k = sync_starred(&b, access);
            if (k >= 0) {
                G.count = k;
                G.t_last = now_ms();
                G.last_ok = true;
                char why[48];
                snprintf(why, sizeof why, "%d favourite%s from Google", k, k == 1 ? "" : "s");
                set_state("ok", why);
                ESP_LOGI(TAG, "%s", why);
            }
        }
    }
    conn_close();
    free(b.buf);
    free(access);
    G.busy = false;
    vTaskDeleteWithCaps(NULL);
}

void contacts_sync(void) { G.due = true; }

bool contacts_set_code(const char *code, const char *state, const char *error)
{
    /* Only the sign-in this knob began comes back here: its nonce, once. */
    const char *slash = state ? strrchr(state, '/') : NULL;
    if (!G.nonce[0] || !slash || strcmp(slash + 1, G.nonce)) {
        ESP_LOGW(TAG, "a sign-in this knob did not begin, ignored");
        set_state("failed", "not this knob's sign-in: sign in again");
        return false;
    }
    G.nonce[0] = 0;
    if (error && error[0]) {
        ESP_LOGW(TAG, "sign-in: %s", error);
        set_state("failed", !strcmp(error, "access_denied") ? "the sign-in was cancelled" : error);
        return false;
    }
    strlcpy(G.code, code, sizeof G.code);
    G.due = true;
    set_state("syncing", "signed in, reading the contacts");
    return true;
}

/* From the phone task, often: a sync when one is due -- asked for, or six
 * hours on -- and the network is up. */
void contacts_tick(bool online)
{
    if (!online || G.busy || now_ms() < G.hold_until) return;
    /* Six hours after a sync that worked; ten minutes after one that did
     * not -- not straight away, again and again, while Google is away. */
    if (G.rtok[0] && G.t_try && now_ms() - G.t_try >= (G.last_ok ? SYNC_MS : RETRY_MS))
        G.due = true;
    if (!G.due || !G.cid[0] || !G.csec[0]) return;
    if (!G.code[0] && !G.rtok[0]) { G.due = false; return; }
    G.due = false;
    G.busy = true;
    G.t_try = now_ms();
    G.last_ok = false;
    /* Its stack in PSRAM: TLS wants 8 kB, which internal RAM -- the WiFi's
     * -- can spare only barely. Flash is written by on_internal(). */
    if (xTaskCreatePinnedToCoreWithCaps(sync_task, "contacts", 8192, NULL, 3, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        G.busy = false;
        G.due = true;                           /* again in half a minute */
        G.hold_until = now_ms() + 30000;
        set_state("failed", "no memory for the sync just now");
    }
}

size_t contacts_state_json(char *b, size_t cap)
{
    char st[16], why[64];
    taskENTER_CRITICAL(&s_lock);
    strlcpy(st, G.state, sizeof st);
    strlcpy(why, G.why, sizeof why);
    taskEXIT_CRITICAL(&s_lock);
    /* The client id is no secret -- every sign-in link carries it -- but the
     * client secret never leaves the knob. */
    const char *cid = G.cid;
    const int n = snprintf(b, cap,
        "{\"client\":%s,\"client_id\":\"%s\",\"secret\":%s,\"signed_in\":%s,\"relay\":\"%s\","
        "\"state\":\"%s\",\"why\":\"%s\",\"count\":%d,\"minutes_ago\":%ld}",
        G.cid[0] ? "true" : "false", cid, G.csec[0] ? "true" : "false",
        G.rtok[0] ? "true" : "false", G.relay, st, why, G.count,
        G.t_last ? (long)((now_ms() - G.t_last) / 60000) : -1L);
    return n > 0 ? (size_t)n : 0;
}

bool contacts_signin_url(const char *knob, char *out, size_t cap)
{
    if (!G.cid[0] || !G.csec[0]) return false;
    char cid[160], relay[256], scope[96], st[96], state[160];
    url_enc(G.cid, cid, sizeof cid);
    url_enc(G.relay, relay, sizeof relay);
    url_enc(SCOPE, scope, sizeof scope);
    /* The knob's address for the relay page, then this sign-in's nonce. */
    snprintf(G.nonce, sizeof G.nonce, "%08lx", (unsigned long)esp_random());
    snprintf(st, sizeof st, "%.64s/%s", knob, G.nonce);
    url_enc(st, state, sizeof state);
    snprintf(out, cap, "https://accounts.google.com/o/oauth2/v2/auth?client_id=%s&redirect_uri=%s"
             "&response_type=code&scope=%s&access_type=offline&prompt=consent&state=%s",
             cid, relay, scope, state);
    return true;
}
