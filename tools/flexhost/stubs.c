/* What the FlexRadio client takes from ESP-IDF, on the PC: the log (kept, for
 * fh_logged()), the clock, tasks as threads, queues and ring buffers under a
 * mutex, NVS in memory, the jack as a counter. No TLS -- esp_tls_init()
 * counts its calls and fails -- no SmartLink and no discovery: the radio is
 * the mock's, by its address. See stub/, and uberhost's for the rest. */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "audio_in.h"
#include "audio_out.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "nvs.h"
#include "smartlink.h"
#include "discovery.h"

int uh_verbose = 0;
int uh_tls_inits = 0;

static pthread_mutex_t s_log = PTHREAD_MUTEX_INITIALIZER;
static char  *s_said;
static size_t s_said_n, s_said_cap;

void uh_log(char level, const char *tag, const char *fmt, ...)
{
    char line[600];
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

bool fh_logged(const char *needle)
{
    pthread_mutex_lock(&s_log);
    const bool yes = s_said && strstr(s_said, needle);
    pthread_mutex_unlock(&s_log);
    return yes;
}

int fh_log_count(const char *needle)
{
    int k = 0;
    pthread_mutex_lock(&s_log);
    for (const char *p = s_said; p && (p = strstr(p, needle)); p++) k++;
    pthread_mutex_unlock(&s_log);
    return k;
}

const char *esp_err_to_name(esp_err_t e)
{
    static char s[16];
    snprintf(s, sizeof s, "0x%x", e);
    return e == ESP_OK ? "ESP_OK" : e == ESP_FAIL ? "ESP_FAIL" : s;
}

int64_t esp_timer_get_time(void)
{
    static int64_t t0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    if (!t0) t0 = us - 100 * 1000000LL;
    return us - t0;
}

uint32_t esp_random(void)
{
    uint32_t v;
    if (getrandom(&v, sizeof v, 0) != sizeof v) v = (uint32_t)rand();
    return v;
}

void esp_fill_random(void *buf, size_t len)
{
    if (getrandom(buf, len, 0) != (ssize_t)len) memset(buf, 0x5A, len);
}

esp_tls_t *esp_tls_init(void) { uh_tls_inits++; return NULL; }
int esp_tls_conn_new_sync(const char *h, int hl, int p, const esp_tls_cfg_t *c, esp_tls_t *t)
{ (void)h; (void)hl; (void)p; (void)c; (void)t; return -1; }
ssize_t esp_tls_conn_read(esp_tls_t *t, void *d, size_t n) { (void)t; (void)d; (void)n; return -1; }
ssize_t esp_tls_conn_write(esp_tls_t *t, const void *d, size_t n) { (void)t; (void)d; (void)n; return -1; }
int esp_tls_conn_destroy(esp_tls_t *t) { (void)t; return 0; }
esp_err_t esp_tls_get_error_handle(esp_tls_t *t, esp_tls_error_handle_t *eh) { (void)t; *eh = NULL; return ESP_FAIL; }
esp_err_t esp_tls_get_conn_sockfd(esp_tls_t *t, int *fd) { (void)t; *fd = -1; return ESP_FAIL; }
ssize_t esp_tls_get_bytes_avail(esp_tls_t *t) { (void)t; return 0; }

void vTaskDelay(TickType_t ticks) { usleep((useconds_t)(ticks ? ticks : 1) * 1000); }
void vTaskDelete(TaskHandle_t t) { (void)t; pthread_exit(NULL); }

typedef struct { void (*fn)(void *); void *arg; } tramp_t;
static void *trampoline(void *p)
{
    tramp_t t = *(tramp_t *)p;
    free(p);
    t.fn(t.arg);
    return NULL;
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out, BaseType_t core, uint32_t caps)
{
    (void)name; (void)stack; (void)prio; (void)core; (void)caps;
    tramp_t *t = malloc(sizeof *t);
    t->fn = fn; t->arg = arg;
    pthread_t th;
    if (pthread_create(&th, NULL, trampoline, t)) return 0;
    pthread_detach(th);
    if (out) *out = (TaskHandle_t)th;
    return pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    return xTaskCreatePinnedToCoreWithCaps(fn, name, stack, arg, prio, out, core, 0);
}

/* ---- NVS in memory */
typedef struct { char key[16]; char s[128]; int64_t i; bool is_str; } kv_t;
static kv_t s_kv[32];
static int s_nkv;
static kv_t *kv(const char *key, bool make)
{
    for (int i = 0; i < s_nkv; i++) if (!strcmp(s_kv[i].key, key)) return &s_kv[i];
    if (!make || s_nkv >= 32) return NULL;
    kv_t *k = &s_kv[s_nkv++];
    memset(k, 0, sizeof *k);
    snprintf(k->key, sizeof k->key, "%s", key);
    return k;
}
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) { (void)ns; (void)mode; *h = 1; return ESP_OK; }
void      nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) { (void)h; kv_t *k = kv(key, false); if (k) k->key[0] = 1; return ESP_OK; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v) { (void)h; kv_t *k = kv(key, true); snprintf(k->s, sizeof k->s, "%s", v); k->is_str = true; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    (void)h;
    kv_t *k = kv(key, false);
    if (!k || !k->is_str) return ESP_ERR_NVS_NOT_FOUND;
    const size_t n = strlen(k->s) + 1;
    if (!out) { *len = n; return ESP_OK; }
    if (*len < n) return ESP_FAIL;
    memcpy(out, k->s, n);
    *len = n;
    return ESP_OK;
}
esp_err_t nvs_set_i64(nvs_handle_t h, const char *key, int64_t v) { (void)h; kv(key, true)->i = v; return ESP_OK; }
esp_err_t nvs_get_i64(nvs_handle_t h, const char *key, int64_t *v) { (void)h; kv_t *k = kv(key, false); if (!k) return ESP_ERR_NVS_NOT_FOUND; *v = k->i; return ESP_OK; }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v) { (void)h; kv(key, true)->i = v; return ESP_OK; }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *v) { (void)h; kv_t *k = kv(key, false); if (!k) return ESP_ERR_NVS_NOT_FOUND; *v = (int32_t)k->i; return ESP_OK; }
esp_err_t nvs_set_i8(nvs_handle_t h, const char *key, int8_t v) { (void)h; kv(key, true)->i = v; return ESP_OK; }
esp_err_t nvs_get_i8(nvs_handle_t h, const char *key, int8_t *v) { (void)h; kv_t *k = kv(key, false); if (!k) return ESP_ERR_NVS_NOT_FOUND; *v = (int8_t)k->i; return ESP_OK; }

/* ---- heap */
size_t heap_caps_get_minimum_free_size(uint32_t caps) { (void)caps; return 100000; }
void heap_caps_monitor_local_minimum_free_size_start(void) {}
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 100000; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 50000; }
int heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t cb) { (void)cb; return 0; }

void esp_restart(void) { fprintf(stderr, "esp_restart()\n"); exit(3); }
esp_err_t esp_register_shutdown_handler(shutdown_handler_t h) { (void)h; return ESP_OK; }

/* ---- a queue of fixed-size items */
struct fh_queue { pthread_mutex_t m; pthread_cond_t c; size_t item, n, head, count; uint8_t *buf; };
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t item)
{
    struct fh_queue *q = calloc(1, sizeof *q);
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->c, NULL);
    q->item = item; q->n = n;
    q->buf = calloc(n, item);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)wait;
    pthread_mutex_lock(&q->m);
    if (q->count == q->n) { pthread_mutex_unlock(&q->m); return pdFALSE; }
    memcpy(q->buf + ((q->head + q->count) % q->n) * q->item, item, q->item);
    q->count++;
    pthread_cond_signal(&q->c);
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    pthread_mutex_lock(&q->m);
    if (!q->count && wait) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)wait * 1000000L;
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&q->c, &q->m, &ts);
    }
    if (!q->count) { pthread_mutex_unlock(&q->m); return pdFALSE; }
    memcpy(item, q->buf + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->n;
    q->count--;
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}

/* ---- a ring buffer of whole items */
typedef struct rb_item { struct rb_item *next; size_t n; uint8_t d[]; } rb_item_t;
struct fh_ring { pthread_mutex_t m; pthread_cond_t c; size_t cap, used; rb_item_t *head, *tail; };
RingbufHandle_t xRingbufferCreateWithCaps(size_t n, RingbufferType_t t, uint32_t caps)
{
    (void)t; (void)caps;
    struct fh_ring *r = calloc(1, sizeof *r);
    pthread_mutex_init(&r->m, NULL);
    pthread_cond_init(&r->c, NULL);
    r->cap = n;
    return r;
}
BaseType_t xRingbufferSend(RingbufHandle_t r, const void *item, size_t n, TickType_t wait)
{
    (void)wait;
    pthread_mutex_lock(&r->m);
    if (r->used + n + 8 > r->cap) { pthread_mutex_unlock(&r->m); return pdFALSE; }
    rb_item_t *it = malloc(sizeof *it + n);
    it->next = NULL; it->n = n;
    memcpy(it->d, item, n);
    if (r->tail) r->tail->next = it; else r->head = it;
    r->tail = it;
    r->used += n + 8;
    pthread_cond_signal(&r->c);
    pthread_mutex_unlock(&r->m);
    return pdTRUE;
}
void *xRingbufferReceive(RingbufHandle_t r, size_t *n, TickType_t wait)
{
    pthread_mutex_lock(&r->m);
    if (!r->head && wait) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)wait * 1000000L;
        ts.tv_sec += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        pthread_cond_timedwait(&r->c, &r->m, &ts);
    }
    rb_item_t *it = r->head;
    if (!it) { pthread_mutex_unlock(&r->m); return NULL; }
    r->head = it->next;
    if (!r->head) r->tail = NULL;
    r->used -= it->n + 8;
    pthread_mutex_unlock(&r->m);
    *n = it->n;
    return it->d;
}
void vRingbufferReturnItem(RingbufHandle_t r, void *item)
{
    (void)r;
    free((uint8_t *)item - offsetof(rb_item_t, d));
}

/* ---- audio: counted, never heard */
static uint64_t s_frames;
void audio_in_set_active(bool on) { (void)on; }
bool audio_in_take(int16_t *out, size_t samples) { (void)out; (void)samples; usleep(10000); return false; }
bool audio_out_feed_pcm16(const int16_t *pcm, size_t frames, uint8_t channels) { (void)pcm; (void)channels; s_frames += frames; return true; }

/* ---- SmartLink: not logged in */
void sl_init(void) {}
bool sl_logged_in(void) { return false; }
bool sl_enabled(void) { return false; }
int sl_count(void) { return 0; }
bool sl_get(int i, sl_radio_t *out) { (void)i; (void)out; return false; }
void sl_active(char *serial, size_t cap) { if (cap) serial[0] = 0; }
esp_err_t sl_set_active(const char *serial) { (void)serial; return ESP_OK; }
esp_err_t sl_open(uint16_t udp_port, sl_link_t *out, char *why, size_t cap) { (void)udp_port; (void)out; snprintf(why, cap, "no"); return ESP_FAIL; }
size_t sl_web_endpoints(const httpd_uri_t **out) { *out = NULL; return 0; }

/* ---- discovery: tried separately */
void disc_start(void) {}
int disc_lan_count(void) { return 0; }
bool disc_lan_get(int i, flex_disc_t *out) { (void)i; (void)out; return false; }
bool disc_lan_name(int i, char *name, size_t cap) { (void)i; if (cap) name[0] = 0; return false; }
esp_err_t disc_lan_use(int i) { (void)i; return ESP_FAIL; }
void disc_news(void) {}
size_t disc_web_endpoints(const httpd_uri_t **out) { *out = NULL; return 0; }
