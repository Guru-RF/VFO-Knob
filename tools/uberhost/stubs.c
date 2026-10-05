/* What the ubersdr client and the web SDR take from ESP-IDF, on the PC: the
 * log, the clock, tasks as threads, NVS in memory, the jack as a counter,
 * and the PNG decoder as a check of what arrived (the decoder itself is
 * tried elsewhere, with the ROM's inflate). No TLS: esp_tls_init() counts
 * its calls and fails. See stub/ for the headers. */
#include <math.h>
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
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/task.h"
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

struct uh_timer { esp_timer_create_args_t a; };

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    *out = calloc(1, sizeof **out);
    (*out)->a = *args;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) { (void)t; (void)us; return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t t) { (void)t; return ESP_OK; }

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

/* ------------------------------------------------------------------ NVS */

typedef struct {
    char    key[16];
    bool    str;
    int64_t v;
    char   *s;
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
    pthread_mutex_unlock(&s_nvs);
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
    k->str = false;
    pthread_mutex_unlock(&s_nvs);
    return ESP_OK;
}

static esp_err_t get_int(const char *key, int64_t *v)
{
    pthread_mutex_lock(&s_nvs);
    kv_t *k = kv(key, false);
    if (k && !k->str) *v = k->v;
    pthread_mutex_unlock(&s_nvs);
    return k && !k->str ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_i64(nvs_handle_t h, const char *key, int64_t v) { (void)h; return set_int(key, v); }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v) { (void)h; return set_int(key, v); }
esp_err_t nvs_set_i8(nvs_handle_t h, const char *key, int8_t v)   { (void)h; return set_int(key, v); }

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

bool audio_out_feed_sdr(const int16_t *pcm, size_t n)
{
    pthread_mutex_lock(&s_audio);
    for (size_t i = 0; i < n; i++) s_sdr_sq += (double)pcm[i] * pcm[i];
    s_sdr_n += n;
    pthread_mutex_unlock(&s_audio);
    return true;
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
