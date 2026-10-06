/* What the ubersdr client and the web SDR take from ESP-IDF, on the PC: the
 * log, the clock, timers and tasks as threads, NVS in memory, the jack as a
 * counter, and the PNG decoder as a check of what arrived (the decoder
 * itself is tried elsewhere, with the ROM's inflate) -- and from net_prov,
 * the knob's list of receivers, in memory. No TLS: esp_tls_init() counts
 * its calls and fails. Every run is a power-on. See stub/ for the headers. */
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "audio_out.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/task.h"
#include "net_prov.h"
#include "nvs.h"
#include "uber_png.h"
#include "uberhost.h"

int uh_verbose;
int uh_tls_inits;

/* ------------------------------------------------------------------ log */

static pthread_mutex_t s_log = PTHREAD_MUTEX_INITIALIZER;
static char  *s_said;                   /* everything logged, for uh_logged() */
static size_t s_said_n, s_said_cap;

void uh_log(char level, const char *tag, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(line, sizeof line, "%c (%lld) %s: ", level, (long long)(esp_timer_get_time() / 1000), tag);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&s_log);
    if (uh_verbose || (level != 'D' && level != 'V')) fprintf(stderr, "%s\n", line);
    const size_t l = strlen(line);
    if (s_said_n + l + 2 > s_said_cap) {
        s_said_cap = (s_said_n + l + 2) * 2;
        s_said = realloc(s_said, s_said_cap);
    }
    memcpy(s_said + s_said_n, line, l);
    s_said_n += l;
    s_said[s_said_n++] = '\n';
    s_said[s_said_n] = 0;
    pthread_mutex_unlock(&s_log);
}

bool uh_logged(const char *needle)
{
    pthread_mutex_lock(&s_log);
    const bool yes = s_said && strstr(s_said, needle);
    pthread_mutex_unlock(&s_log);
    return yes;
}

bool uh_logged_after(const char *first, const char *then)
{
    pthread_mutex_lock(&s_log);
    const char *at = s_said ? strstr(s_said, first) : NULL;
    const bool yes = at && strstr(at + strlen(first), then);
    pthread_mutex_unlock(&s_log);
    return yes;
}

int64_t uh_log_ms(const char *needle, int nth)
{
    int64_t ms = -1;
    pthread_mutex_lock(&s_log);
    const char *p = s_said;
    for (int k = 0; p && (p = strstr(p, needle)); p += strlen(needle), k++) {
        if (k < nth) continue;
        const char *line = p;
        while (line > s_said && line[-1] != '\n') line--;
        long long t;
        if (sscanf(line, "%*c (%lld)", &t) == 1) ms = t;
        break;
    }
    pthread_mutex_unlock(&s_log);
    return ms;
}

int uh_log_count(const char *needle)
{
    int n = 0;
    pthread_mutex_lock(&s_log);
    for (const char *p = s_said; p && (p = strstr(p, needle)); p += strlen(needle)) n++;
    pthread_mutex_unlock(&s_log);
    return n;
}

const char *esp_err_to_name(esp_err_t e)
{
    static char s[16];
    snprintf(s, sizeof s, "0x%x", e);
    return e == ESP_OK ? "ESP_OK" : e == ESP_FAIL ? "ESP_FAIL" : s;
}

/* ---------------------------------------------------------------- clock */

/* Time since a "boot" 100 s ago: the knob has been up a while by the time
 * a session runs, and the client's first-read timers count from boot. */
int64_t esp_timer_get_time(void)
{
    static int64_t t0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    const int64_t us = t.tv_sec * 1000000LL + t.tv_nsec / 1000;
    if (!t0) t0 = us - 100 * 1000000LL;
    return us - t0;
}

/* A timer: its callback on a thread of its own once its time has come,
 * unless stopped or started again meanwhile -- and one callback at a time,
 * as esp_timer's task runs them. */
struct uh_timer {
    esp_timer_create_args_t a;
    uint64_t gen;                       /* each start and each stop moves it on */
    bool     armed;                     /* started, and since then neither fired nor stopped */
};
static pthread_mutex_t s_timers = PTHREAD_MUTEX_INITIALIZER, s_timer_task = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    esp_timer_handle_t t;
    uint64_t gen, us;
} shot_t;

static void *shot(void *p)
{
    const shot_t s = *(shot_t *)p;
    free(p);
    const struct timespec d = { (time_t)(s.us / 1000000), (long)(s.us % 1000000) * 1000 };
    nanosleep(&d, NULL);
    pthread_mutex_lock(&s_timer_task);
    pthread_mutex_lock(&s_timers);
    const bool due = s.t->gen == s.gen;
    if (due) {                          /* fired: nothing left to stop */
        s.t->gen++;
        s.t->armed = false;
    }
    pthread_mutex_unlock(&s_timers);
    if (due) s.t->a.callback(s.t->a.arg);
    pthread_mutex_unlock(&s_timer_task);
    return NULL;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    *out = calloc(1, sizeof **out);
    (*out)->a = *args;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{
    shot_t *s = malloc(sizeof *s);
    pthread_mutex_lock(&s_timers);
    *s = (shot_t){ .t = t, .gen = ++t->gen, .us = us };
    t->armed = true;
    pthread_mutex_unlock(&s_timers);
    pthread_t th;
    if (pthread_create(&th, NULL, shot, s)) {
        pthread_mutex_lock(&s_timers);
        if (t->gen == s->gen) t->armed = false;
        pthread_mutex_unlock(&s_timers);
        free(s);
        return ESP_FAIL;
    }
    pthread_detach(th);
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    pthread_mutex_lock(&s_timers);
    t->gen++;
    t->armed = false;
    pthread_mutex_unlock(&s_timers);
    return ESP_OK;
}

bool esp_timer_is_active(esp_timer_handle_t t)
{
    pthread_mutex_lock(&s_timers);
    const bool on = t->armed;
    pthread_mutex_unlock(&s_timers);
    return on;
}

esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }

uint32_t esp_random(void)
{
    uint32_t r = 0;
    if (getrandom(&r, sizeof r, 0) != sizeof r) r = (uint32_t)rand();
    return r;
}

void esp_fill_random(void *buf, size_t len)
{
    if (getrandom(buf, len, 0) != (ssize_t)len)
        for (size_t i = 0; i < len; i++) ((uint8_t *)buf)[i] = (uint8_t)rand();
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t d = { .version = "v0.0.0-uberhost", .project_name = "vfo-knob-ubersdr" };
    return &d;
}

/* ----------------------------------------------------------- no names */

/* Addresses only, as numbers: a name is not found, as lwIP finds none that
 * no server knows -- never asked of the PC's resolver, which would ask the
 * network. */
int uh_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    struct addrinfo h = hints ? *hints : (struct addrinfo){ 0 };
    h.ai_flags |= AI_NUMERICHOST | AI_NUMERICSERV;
    return getaddrinfo(node, service, &h, res);
}

void uh_freeaddrinfo(struct addrinfo *ai) { freeaddrinfo(ai); }

/* -------------------------------------------------------------- no TLS */

esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }
esp_tls_t *esp_tls_init(void) { uh_tls_inits++; return NULL; }
int esp_tls_conn_new_sync(const char *h, int hl, int p, const esp_tls_cfg_t *c, esp_tls_t *t)
{ (void)h; (void)hl; (void)p; (void)c; (void)t; return -1; }
ssize_t esp_tls_conn_read(esp_tls_t *t, void *d, size_t n) { (void)t; (void)d; (void)n; return -1; }
ssize_t esp_tls_conn_write(esp_tls_t *t, const void *d, size_t n) { (void)t; (void)d; (void)n; return -1; }
int esp_tls_conn_destroy(esp_tls_t *t) { (void)t; return 0; }
esp_err_t esp_tls_get_error_handle(esp_tls_t *t, esp_tls_error_handle_t *eh) { (void)t; *eh = NULL; return ESP_FAIL; }
esp_err_t esp_tls_get_conn_sockfd(esp_tls_t *t, int *fd) { (void)t; *fd = -1; return ESP_FAIL; }
ssize_t esp_tls_get_bytes_avail(esp_tls_t *t) { (void)t; return 0; }

/* ---------------------------------------------------------------- tasks */

void vTaskDelay(TickType_t ticks)
{
    if (ticks) usleep((useconds_t)ticks * 1000);
    else sched_yield();
}

void vTaskDelete(TaskHandle_t t)
{
    (void)t;
    pthread_exit(NULL);
}

/* ...or deleted by another once it has nothing left to do, asleep for good
 * (sdr_test's): a thread left to its sleep. */
void vTaskDeleteWithCaps(TaskHandle_t t)
{
    if (!t || pthread_equal((pthread_t)t, pthread_self())) vTaskDelete(t);
}

/* A thread's stack is the PC's: no count of it here, 0. */
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t)
{
    (void)t;
    return 0;
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} start_t;

static void *trampoline(void *p)
{
    start_t s = *(start_t *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out, BaseType_t core,
                                           uint32_t caps)
{
    (void)name; (void)stack; (void)prio; (void)core; (void)caps;
    start_t *s = malloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    pthread_t th;
    if (pthread_create(&th, NULL, trampoline, s)) return pdFALSE;
    pthread_detach(th);
    if (out) *out = (TaskHandle_t)th;
    return pdPASS;
}

/* -------------------------------------------------------- the receivers */

/* The knob's list of receivers (net_prov's), in memory: the scenario's, set
 * with net_prov_radios_save() -- and the one in use, as the configuration
 * page reads it back, and how often the client chose it. The station is
 * always on its network. */
static pthread_mutex_t s_rl = PTHREAD_MUTEX_INITIALIZER;
static net_radio_t s_rlist[NET_PROV_RADIOS];
static int s_rn, s_rsel, s_ruses;

int net_prov_radio_count(void)
{
    pthread_mutex_lock(&s_rl);
    const int n = s_rn;
    pthread_mutex_unlock(&s_rl);
    return n;
}

int net_prov_radio_active(void)
{
    pthread_mutex_lock(&s_rl);
    const int i = s_rsel;
    pthread_mutex_unlock(&s_rl);
    return i;
}

bool net_prov_radio_get(int i, net_radio_t *out)
{
    pthread_mutex_lock(&s_rl);
    const bool ok = i >= 0 && i < s_rn && out;
    if (ok) *out = s_rlist[i];
    pthread_mutex_unlock(&s_rl);
    return ok;
}

esp_err_t net_prov_radios_save(const net_radio_t *list, int n, int active)
{
    if (!list || n < 1 || n > NET_PROV_RADIOS || active < 0 || active >= n) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_rl);
    memcpy(s_rlist, list, (size_t)n * sizeof *list);
    s_rn = n;
    s_rsel = active;
    pthread_mutex_unlock(&s_rl);
    return ESP_OK;
}

esp_err_t net_prov_radio_activate(int i)
{
    pthread_mutex_lock(&s_rl);
    const bool ok = i >= 0 && i < s_rn;
    if (ok) {
        s_rsel = i;
        s_ruses++;
    }
    pthread_mutex_unlock(&s_rl);
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}

bool net_prov_is_connected(void) { return true; }

int uh_radio_uses(void)
{
    pthread_mutex_lock(&s_rl);
    const int n = s_ruses;
    pthread_mutex_unlock(&s_rl);
    return n;
}

/* ------------------------------------------------------------------ NVS */

typedef struct {
    char    key[16];
    bool    str, blob;
    int64_t v;
    char   *s;                          /* a string's, or a blob's n bytes */
    size_t  n;
} kv_t;
static kv_t s_kv[64];
static int  s_nkv;
static pthread_mutex_t s_nvs = PTHREAD_MUTEX_INITIALIZER;

static kv_t *kv(const char *key, bool make)
{
    for (int i = 0; i < s_nkv; i++) if (!strcmp(s_kv[i].key, key)) return &s_kv[i];
    if (!make || s_nkv == 64) return NULL;
    kv_t *k = &s_kv[s_nkv++];
    memset(k, 0, sizeof *k);
    snprintf(k->key, sizeof k->key, "%s", key);
    return k;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) { (void)ns; (void)mode; *h = 1; return ESP_OK; }
void      nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    pthread_mutex_lock(&s_nvs);
    kv_t *k = kv(key, false);
    if (k) k->key[0] = 0x7F;                     /* never matched again */
    pthread_mutex_unlock(&s_nvs);
    return k ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v)
{
    (void)h;
    pthread_mutex_lock(&s_nvs);
    kv_t *k = kv(key, true);
    free(k->s);
    k->s = strdup(v);
    k->str = true;
    k->blob = false;
    pthread_mutex_unlock(&s_nvs);
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len)
{
    (void)h;
    pthread_mutex_lock(&s_nvs);
    kv_t *k = kv(key, true);
    free(k->s);
    k->s = malloc(len ? len : 1);
    memcpy(k->s, v, len);
    k->n = len;
    k->blob = true;
    k->str = false;
    pthread_mutex_unlock(&s_nvs);
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    esp_err_t e = ESP_ERR_NVS_NOT_FOUND;
    pthread_mutex_lock(&s_nvs);
    kv_t *k = kv(key, false);
    if (k && k->blob) {
        if (!out) {
            *len = k->n;
            e = ESP_OK;
        } else if (*len >= k->n) {
            memcpy(out, k->s, k->n);
            *len = k->n;
            e = ESP_OK;
        } else {
            e = ESP_ERR_INVALID_SIZE;
        }
    }
    pthread_mutex_unlock(&s_nvs);
    return e;
}

esp_err_t nvs_get_stats(const char *part_name, nvs_stats_t *st)
{
    (void)part_name;
    memset(st, 0, sizeof *st);
    st->total_entries = 6 * 126;
    st->available_entries = 5 * 126 - 40;
    st->free_entries = st->available_entries + 126;
    st->used_entries = st->total_entries - st->free_entries;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    (void)h;
    esp_err_t e = ESP_ERR_NVS_NOT_FOUND;
    pthread_mutex_lock(&s_nvs);
    kv_t *k = kv(key, false);
    if (k && k->str) {
        const size_t n = strlen(k->s) + 1;
        if (!out) {
            *len = n;
            e = ESP_OK;
        } else if (*len >= n) {
            memcpy(out, k->s, n);
            *len = n;
            e = ESP_OK;
        } else {
            e = ESP_ERR_INVALID_SIZE;
        }
    }
    pthread_mutex_unlock(&s_nvs);
    return e;
}

static esp_err_t set_int(const char *key, int64_t v)
{
    pthread_mutex_lock(&s_nvs);
    kv_t *k = kv(key, true);
    k->v = v;
    k->str = k->blob = false;
    pthread_mutex_unlock(&s_nvs);
    return ESP_OK;
}

static esp_err_t get_int(const char *key, int64_t *v)
{
    pthread_mutex_lock(&s_nvs);
    kv_t *k = kv(key, false);
    const bool ok = k && !k->str && !k->blob;
    if (ok) *v = k->v;
    pthread_mutex_unlock(&s_nvs);
    return ok ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_i64(nvs_handle_t h, const char *key, int64_t v) { (void)h; return set_int(key, v); }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v) { (void)h; return set_int(key, v); }
esp_err_t nvs_set_i8(nvs_handle_t h, const char *key, int8_t v)   { (void)h; return set_int(key, v); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v) { (void)h; return set_int(key, v); }

esp_err_t nvs_get_i64(nvs_handle_t h, const char *key, int64_t *v) { (void)h; return get_int(key, v); }

esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *v)
{
    (void)h;
    int64_t x;
    const esp_err_t e = get_int(key, &x);
    if (e == ESP_OK) *v = (int32_t)x;
    return e;
}

esp_err_t nvs_get_i8(nvs_handle_t h, const char *key, int8_t *v)
{
    (void)h;
    int64_t x;
    const esp_err_t e = get_int(key, &x);
    if (e == ESP_OK) *v = (int8_t)x;
    return e;
}

esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *v)
{
    (void)h;
    int64_t x;
    const esp_err_t e = get_int(key, &x);
    if (e == ESP_OK) *v = (uint32_t)x;
    return e;
}

/* --------------------------------------------------------------- the jack */

static pthread_mutex_t s_audio = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_radio_n, s_sdr_n;
static double   s_radio_sq, s_sdr_sq;

bool audio_out_feed_pcm16(const int16_t *pcm, size_t frames, uint8_t channels)
{
    pthread_mutex_lock(&s_audio);
    for (size_t i = 0; i < frames * channels; i++) s_radio_sq += (double)pcm[i] * pcm[i];
    s_radio_n += frames;
    pthread_mutex_unlock(&s_audio);
    return true;
}

void audio_out_flush(void) {}
void audio_out_sdr(bool on) { (void)on; }
void audio_out_set_balance(int8_t balance) { (void)balance; }

static uint32_t s_sdr_feeds;

bool audio_out_feed_sdr(const int16_t *pcm, size_t n)
{
    pthread_mutex_lock(&s_audio);
    for (size_t i = 0; i < n; i++) s_sdr_sq += (double)pcm[i] * pcm[i];
    s_sdr_n += n;
    s_sdr_feeds++;
    pthread_mutex_unlock(&s_audio);
    return true;
}

/* The SDR's ring, as the session keeps it (kiwi_sess.h's flow): played as
 * fed, nothing waiting in it -- every frame heard, none left out -- with the
 * knob's sizes, 40 KB of mono over a pre-roll of 12 KB that may grow to
 * 0.7 s. */
size_t audio_out_sdr_queued(void) { return 0; }
size_t audio_out_sdr_room(void) { return 40 * 1024 / 2; }
size_t audio_out_sdr_preroll(void) { return 12 * 1024 / 2; }
size_t audio_out_sdr_preroll_max(void) { return 24000 * 700 / 1000; }
void   audio_out_sdr_set_preroll(size_t n) { (void)n; }

void audio_out_sdr_stats(audio_stats_t *st)
{
    pthread_mutex_lock(&s_audio);
    memset(st, 0, sizeof *st);
    st->frames = s_sdr_feeds;
    st->sample_rate = 24000;
    st->channels = 1;
    pthread_mutex_unlock(&s_audio);
}

void uh_audio(uint64_t *radio, double *radio_rms, uint64_t *sdr, double *sdr_rms)
{
    pthread_mutex_lock(&s_audio);
    *radio = s_radio_n;
    *radio_rms = s_radio_n ? sqrt(s_radio_sq / s_radio_n) : 0;
    *sdr = s_sdr_n;
    *sdr_rms = s_sdr_n ? sqrt(s_sdr_sq / s_sdr_n) : 0;
    pthread_mutex_unlock(&s_audio);
}

/* ------------------------------------------------------------- pictures */

static size_t s_png_n;
static int    s_png_w, s_png_h;

esp_err_t upng_decode(uint8_t *png, size_t n, uint16_t *out, int box_w, int box_h,
                      int *ow, int *oh, char *why, size_t wn)
{
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (n < 33 || memcmp(png, SIG, 8) || memcmp(png + 12, "IHDR", 4)) {
        snprintf(why, wn, "not a PNG");
        return ESP_FAIL;
    }
    const int w = png[16] << 24 | png[17] << 16 | png[18] << 8 | png[19];
    const int h = png[20] << 24 | png[21] << 16 | png[22] << 8 | png[23];
    /* Fitted as the decoder fits it, its shape kept. */
    int W, H;
    if ((int64_t)w * box_h > (int64_t)h * box_w) {
        W = box_w;
        H = (int)((int64_t)h * box_w / w);
    } else {
        H = box_h;
        W = (int)((int64_t)w * box_h / h);
    }
    for (int i = 0; i < box_w * box_h; i++) out[i] = 0x8410;
    s_png_n = n;
    s_png_w = w;
    s_png_h = h;
    *ow = W;
    *oh = H;
    return ESP_OK;
}

void uh_png_last(size_t *n, int *w, int *h)
{
    *n = s_png_n;
    *w = s_png_w;
    *h = s_png_h;
}
