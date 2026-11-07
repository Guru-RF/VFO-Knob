/* What the K4 client takes from ESP-IDF, on the PC: the log (kept, for
 * fh_logged()), the clock, tasks as threads, queues, ring buffers and a mutex,
 * the jack as a counter, and the password's SHA-384 from OpenSSL. The radio is
 * the mock's, by its address. Adapted from flexhost's. See stub/, and
 * uberhost's for the rest. */
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
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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
static volatile uint64_t s_frames;
uint64_t fh_frames(void) { return s_frames; }
static volatile bool s_mic;
bool fh_mic_active(void) { return s_mic; }
void audio_in_set_active(bool on) { s_mic = on; }
void audio_in_set_settle_ms(uint16_t ms) { (void)ms; }
bool audio_in_take(int16_t *out, size_t samples) { (void)out; (void)samples; usleep(10000); return false; }
bool audio_out_feed_pcm16(const int16_t *pcm, size_t frames, uint8_t channels) { (void)pcm; (void)channels; s_frames += frames; return true; }


/* ---- a mutex */
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutex_init(m, NULL);
    return m;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait)
{
    (void)wait;
    return pthread_mutex_lock(m) == 0 ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { return pthread_mutex_unlock(m) == 0 ? pdTRUE : pdFALSE; }

/* ---- the password's proof: OpenSSL's SHA-384, where the knob has mbedTLS's */
#include <openssl/sha.h>
#include "k4_parse.h"
#include "k4_sha.h"
void k4_sha384_hex(const char *password, char out[97])
{
    uint8_t d[SHA384_DIGEST_LENGTH];
    SHA384((const unsigned char *)password, strlen(password), d);
    k4_hex(out, d, sizeof d);
}

void audio_out_stats(audio_stats_t *st) { memset(st, 0, sizeof *st); st->frames = (uint32_t)s_frames; }

#include "esp_wifi.h"
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) { ap->rssi = -50; return ESP_OK; }
