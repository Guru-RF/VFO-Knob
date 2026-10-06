/* Host shim: the few ESP-IDF services sdr_rx.c and kiwi_mark.c use, on Linux.
 *
 *   NVS        a file (SHIM_NVS), written through on every set, as flash is:
 *              a restart of the process is a restart of the knob; with
 *              SHIM_NVS_FULL set, full -- every write fails
 *   RTC memory a file too (SHIM_RTC), mapped: what a crash leaves as it was
 *   the reset  why this boot began (SHIM_RESET: poweron, sw, panic, brownout)
 *   esp_timer  one dispatcher thread, as the esp_timer task
 *   tasks      threads; critical sections one recursive mutex
 *   audio_out  its rings, played at 24 kHz by the PC's clock: what is fed,
 *              dropped and run dry is counted as audio_out.c counts it
 *
 * Sockets, getaddrinfo and select are the host's own: lwIP's names for them
 * are POSIX's. */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "audio_out.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "shim.h"

/* ------------------------------------------------------------- basics */

static pthread_mutex_t s_crit;
static pthread_once_t  s_once = PTHREAD_ONCE_INIT;
static int64_t         s_t0;

static int64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void once(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_crit, &a);
    s_t0 = mono_us();
}

void shim_enter(portMUX_TYPE *m) { (void)m; pthread_once(&s_once, once); pthread_mutex_lock(&s_crit); }
void shim_exit(portMUX_TYPE *m)  { (void)m; pthread_mutex_unlock(&s_crit); }

/* The knob's clock starts at its boot: the process's start. */
int64_t esp_timer_get_time(void) { pthread_once(&s_once, once); return mono_us() - s_t0; }

static pthread_mutex_t s_log = PTHREAD_MUTEX_INITIALIZER;

void shim_log(char lv, const char *tag, const char *fmt, ...)
{
    if (lv == 'D' && !getenv("SHIM_DEBUG")) return;
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&s_log);
    printf("%c (%7.3f) %s: ", lv, (double)esp_timer_get_time() / 1e6, tag);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    pthread_mutex_unlock(&s_log);
    va_end(ap);
}

void shim_say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&s_log);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    pthread_mutex_unlock(&s_log);
    va_end(ap);
}

const char *esp_err_to_name(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE: return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
    default: return "ESP_FAIL";
    }
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

/* -------------------------------------------------------------- tasks */

typedef struct { TaskFunction_t fn; void *arg; } start_t;

static void *task_main(void *p)
{
    start_t s = *(start_t *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           int prio, TaskHandle_t *out, int core, uint32_t caps)
{
    (void)name; (void)stack; (void)prio; (void)core; (void)caps;
    start_t *s = malloc(sizeof *s);
    if (!s) return pdFALSE;
    s->fn  = fn;
    s->arg = arg;
    pthread_t t;
    if (pthread_create(&t, NULL, task_main, s) != 0) { free(s); return pdFALSE; }
    pthread_detach(t);
    if (out) *out = (TaskHandle_t)t;
    return pdPASS;
}

void vTaskDelay(TickType_t ticks) { usleep((useconds_t)ticks * 1000); }

/* A task deleting itself; or deleted by another once it has nothing left
 * to do, asleep for good (sdr_test's): a thread left to its sleep. */
void vTaskDelete(TaskHandle_t t) { (void)t; pthread_exit(NULL); }
void vTaskDeleteWithCaps(TaskHandle_t t)
{
    if (!t || pthread_equal((pthread_t)t, pthread_self())) pthread_exit(NULL);
}

/* -------------------------------------------------------- the boot */

esp_reset_reason_t esp_reset_reason(void)
{
    const char *r = getenv("SHIM_RESET");
    if (!r || !strcmp(r, "poweron")) return ESP_RST_POWERON;
    if (!strcmp(r, "sw"))       return ESP_RST_SW;
    if (!strcmp(r, "panic"))    return ESP_RST_PANIC;
    if (!strcmp(r, "brownout")) return ESP_RST_BROWNOUT;
    if (!strcmp(r, "wdt"))      return ESP_RST_TASK_WDT;
    return ESP_RST_UNKNOWN;
}

/* RTC memory: a file, mapped -- written as the memory is, so a run killed
 * mid-flight ("off", a crash) leaves it as it was for the next. One region,
 * whoever asks first sizes it. */
void *shim_rtc(size_t n)
{
    static void *mem;
    if (mem) return mem;
    const char *p = getenv("SHIM_RTC");
    int fd = open(p ? p : "rtc.bin", O_RDWR | O_CREAT, 0644);
    if (fd >= 0 && ftruncate(fd, (off_t)(n < 4096 ? 4096 : n)) == 0)
        mem = mmap(NULL, n < 4096 ? 4096 : n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fd >= 0) close(fd);
    if (!mem || mem == MAP_FAILED) mem = calloc(1, n);
    return mem;
}

/* -------------------------------------------------------------- timers */

struct shim_timer {
    esp_timer_cb_t     cb;
    void              *arg;
    int64_t            due;          /* 0: not running */
    struct shim_timer *next;
};

static pthread_mutex_t     s_tm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t      s_tc;
static struct shim_timer  *s_timers;
static pthread_t           s_tthread;
static bool                s_tstarted;

static void *timer_main(void *p)
{
    (void)p;
    pthread_mutex_lock(&s_tm);
    for (;;) {
        struct shim_timer *first = NULL;
        for (struct shim_timer *t = s_timers; t; t = t->next)
            if (t->due && (!first || t->due < first->due)) first = t;
        if (!first) { pthread_cond_wait(&s_tc, &s_tm); continue; }
        const int64_t now = esp_timer_get_time();
        if (first->due > now) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const int64_t wait = first->due - now;
            ts.tv_sec  += wait / 1000000;
            ts.tv_nsec += (wait % 1000000) * 1000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&s_tc, &s_tm, &ts);
            continue;
        }
        first->due = 0;                      /* one shot: off while it runs */
        pthread_mutex_unlock(&s_tm);
        first->cb(first->arg);
        pthread_mutex_lock(&s_tm);
    }
    return NULL;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out)
{
    struct shim_timer *t = calloc(1, sizeof *t);
    if (!t) return ESP_ERR_NO_MEM;
    t->cb  = a->callback;
    t->arg = a->arg;
    pthread_mutex_lock(&s_tm);
    if (!s_tstarted) {
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&s_tc, &ca);
        pthread_create(&s_tthread, NULL, timer_main, NULL);
        pthread_detach(s_tthread);
        s_tstarted = true;
    }
    t->next  = s_timers;
    s_timers = t;
    pthread_mutex_unlock(&s_tm);
    *out = t;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{
    esp_err_t e = ESP_OK;
    pthread_mutex_lock(&s_tm);
    if (t->due) e = ESP_ERR_INVALID_STATE;
    else {
        t->due = esp_timer_get_time() + (int64_t)us;
        if (!t->due) t->due = 1;
        pthread_cond_signal(&s_tc);
    }
    pthread_mutex_unlock(&s_tm);
    return e;
}

esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    esp_err_t e = ESP_OK;
    pthread_mutex_lock(&s_tm);
    if (!t->due) e = ESP_ERR_INVALID_STATE;
    t->due = 0;
    pthread_mutex_unlock(&s_tm);
    return e;
}

bool esp_timer_is_active(esp_timer_handle_t t)
{
    pthread_mutex_lock(&s_tm);
    const bool a = t->due != 0;
    pthread_mutex_unlock(&s_tm);
    return a;
}

bool shim_timers_idle(void)
{
    bool idle = true;
    pthread_mutex_lock(&s_tm);
    for (struct shim_timer *t = s_timers; t; t = t->next) if (t->due) idle = false;
    pthread_mutex_unlock(&s_tm);
    return idle;
}

/* ----------------------------------------------------------------- NVS */

typedef struct nv {
    char       ns[16], key[16], type;    /* 's' str, 'b' blob, '1' i8, 'u' u8, '4' u32, 'i' i32, '8' i64 */
    uint8_t   *v;
    size_t     n;
    struct nv *next;
} nv_t;

static pthread_mutex_t s_nm = PTHREAD_MUTEX_INITIALIZER;
static nv_t           *s_nv;
static bool            s_nv_loaded;
static char            s_ns[8][16];       /* a handle is an index into this */
static int             s_nns;
uint32_t               shim_nvs_writes;

static const char *nvs_path(void) { const char *p = getenv("SHIM_NVS"); return p ? p : "nvs.txt"; }

static void nv_load(void)
{
    if (s_nv_loaded) return;
    s_nv_loaded = true;
    FILE *f = fopen(nvs_path(), "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char ns[16], key[16], type, hex[4000];
        if (sscanf(line, "%15s %15s %c %3999s", ns, key, &type, hex) != 4) continue;
        nv_t *e = calloc(1, sizeof *e);
        strcpy(e->ns, ns);
        strcpy(e->key, key);
        e->type = type;
        e->n = strlen(hex) / 2;
        e->v = malloc(e->n + 1);
        for (size_t i = 0; i < e->n; i++) { unsigned b; sscanf(hex + 2 * i, "%2x", &b); e->v[i] = (uint8_t)b; }
        e->v[e->n] = 0;
        e->next = s_nv;
        s_nv = e;
    }
    fclose(f);
}

/* Through to the file, as flash is written at once: a process killed right
 * after keeps it, one killed right before does not. */
static void nv_store(void)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", nvs_path());
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (nv_t *e = s_nv; e; e = e->next) {
        fprintf(f, "%s %s %c ", e->ns, e->key, e->type);
        if (!e->n) fprintf(f, "-");
        for (size_t i = 0; i < e->n; i++) fprintf(f, "%02x", e->v[i]);
        fprintf(f, "\n");
    }
    fclose(f);
    rename(tmp, nvs_path());
    shim_nvs_writes++;
}

static nv_t *nv_find(nvs_handle_t h, const char *key)
{
    for (nv_t *e = s_nv; e; e = e->next)
        if (!strcmp(e->ns, s_ns[h]) && !strcmp(e->key, key)) return e;
    return NULL;
}

static esp_err_t nv_get(nvs_handle_t h, const char *key, char type, void *out, size_t *len, size_t fixed)
{
    pthread_mutex_lock(&s_nm);
    nv_load();
    nv_t *e = nv_find(h, key);
    esp_err_t r = ESP_OK;
    if (!e || e->type != type) r = ESP_ERR_NVS_NOT_FOUND;
    else if (fixed) memcpy(out, e->v, fixed);
    else {
        const size_t need = e->n + (type == 's');
        if (!out) *len = need;
        else if (*len < need) r = ESP_ERR_INVALID_ARG;
        else { memcpy(out, e->v, e->n); if (type == 's') ((char *)out)[e->n] = 0; *len = need; }
    }
    pthread_mutex_unlock(&s_nm);
    return r;
}

static bool nv_full(void) { return getenv("SHIM_NVS_FULL") != NULL; }

static esp_err_t nv_set(nvs_handle_t h, const char *key, char type, const void *v, size_t n)
{
    if (nv_full()) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    pthread_mutex_lock(&s_nm);
    nv_load();
    nv_t *e = nv_find(h, key);
    if (!e) {
        e = calloc(1, sizeof *e);
        snprintf(e->ns, sizeof e->ns, "%s", s_ns[h]);
        snprintf(e->key, sizeof e->key, "%s", key);
        e->next = s_nv;
        s_nv = e;
    }
    free(e->v);
    e->type = type;
    e->n = n;
    e->v = malloc(n + 1);
    memcpy(e->v, v, n);
    nv_store();
    pthread_mutex_unlock(&s_nm);
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    (void)mode;
    pthread_mutex_lock(&s_nm);
    int i = 0;
    while (i < s_nns && strcmp(s_ns[i], ns)) i++;
    if (i == s_nns && s_nns < 8) snprintf(s_ns[s_nns++], sizeof s_ns[0], "%s", ns);
    pthread_mutex_unlock(&s_nm);
    *h = (nvs_handle_t)i;
    return ESP_OK;
}

void      nvs_close(nvs_handle_t h)  { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *o, size_t *n)   { return nv_get(h, k, 's', o, n, 0); }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)        { return nv_set(h, k, 's', v, strlen(v)); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *o, size_t *n)  { return nv_get(h, k, 'b', o, n, 0); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t n) { return nv_set(h, k, 'b', v, n); }
esp_err_t nvs_get_i8(nvs_handle_t h, const char *k, int8_t *o)             { return nv_get(h, k, '1', o, NULL, 1); }
esp_err_t nvs_set_i8(nvs_handle_t h, const char *k, int8_t v)              { return nv_set(h, k, '1', &v, 1); }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *o)          { return nv_get(h, k, '4', o, NULL, 4); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v)           { return nv_set(h, k, '4', &v, 4); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *o)            { return nv_get(h, k, 'u', o, NULL, 1); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v)             { return nv_set(h, k, 'u', &v, 1); }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *o)           { return nv_get(h, k, 'i', o, NULL, 4); }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v)            { return nv_set(h, k, 'i', &v, 4); }
esp_err_t nvs_get_i64(nvs_handle_t h, const char *k, int64_t *o)           { return nv_get(h, k, '8', o, NULL, 8); }
esp_err_t nvs_set_i64(nvs_handle_t h, const char *k, int64_t v)            { return nv_set(h, k, '8', &v, 8); }

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    pthread_mutex_lock(&s_nm);
    nv_load();
    esp_err_t r = ESP_ERR_NVS_NOT_FOUND;
    for (nv_t **pp = &s_nv; *pp; pp = &(*pp)->next) {
        if (!strcmp((*pp)->ns, s_ns[h]) && !strcmp((*pp)->key, key)) {
            nv_t *e = *pp;
            *pp = e->next;
            free(e->v);
            free(e);
            nv_store();
            r = ESP_OK;
            break;
        }
    }
    pthread_mutex_unlock(&s_nm);
    return r;
}

esp_err_t nvs_get_stats(const char *part_name, nvs_stats_t *st)
{
    (void)part_name;
    memset(st, 0, sizeof *st);
    st->total_entries = 6 * 126;
    st->available_entries = nv_full() ? 0 : 5 * 126 - 40;
    st->free_entries = st->available_entries + (nv_full() ? 0 : 126);
    st->used_entries = st->total_entries - st->free_entries;
    return ESP_OK;
}

/* ---------------------------------------------------------- audio_out */

/* audio_out's rings, as the kiwi firmware has them (audio_out.c): 64 KB of
 * stereo frames over a pre-roll of 24 KB, which may grow to 0.7 s, the ring
 * holding as much more; fed 1 KB at a time, a full ring letting the rest of
 * a feed go (dropped); and the second receiver's, 40 KB of mono over a
 * pre-roll of 12 KB, growing alike, a feed taken whole or not at all. Both
 * played at 24 kHz by the PC's clock. Alone, a ring that stays dry 60 ms
 * while it plays goes back to its pre-roll -- an underrun; with the second
 * receiver on, the two are mixed a 10 ms block at a time (mix_block), and a
 * ring short of a whole block runs dry there and then, with no grace. */
#define ROOM_N    (64 * 1024 / 4)
#define PREROLL_N (24 * 1024 / 4)
#define GROWN_N   (AUDIO_RATE_HZ * 700 / 1000)
#define RING_N    (ROOM_N + GROWN_N - PREROLL_N)
#define SDR_ROOM  (40 * 1024 / 2)
#define SDR_PRE   (12 * 1024 / 2)
#define SDR_N     (SDR_ROOM + GROWN_N - SDR_PRE)
#define PIECE     256
#define DRY_N     (AUDIO_RATE_HZ * 60 / 1000)
#define BLOCK_N   (AUDIO_RATE_HZ / 100)          /* the mixer's block, 10 ms */

typedef struct { size_t level, dry; bool playing; } ring_t;

static pthread_mutex_t s_am = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  s_play_once = PTHREAD_ONCE_INIT;
static ring_t          s_main, s_sdr;
static audio_stats_t   s_ast;
static uint32_t        s_sdr_frames, s_sdr_dropped, s_sdr_underruns;
static volatile size_t s_pre = PREROLL_N, s_sdr_pre = SDR_PRE;

volatile uint64_t shim_sdr_samples;
volatile bool     shim_sdr_on;
volatile uint64_t shim_samples;          /* the receiver's own, as a radio's */
volatile uint32_t shim_flushes;

static void play(ring_t *r, size_t due, size_t pre, uint32_t *underruns)
{
    if (!r->playing) {
        if (r->level < pre) return;
        r->playing = true;
    }
    if (r->level >= due) {
        r->level -= due;
        r->dry = 0;
        return;
    }
    r->dry += due - r->level;
    r->level = 0;
    if (r->dry >= DRY_N) {
        r->playing = false;
        r->dry = 0;
        (*underruns)++;
    }
}

/* A block out of a ring that plays, as mix_block takes it: what is there,
 * and short of a whole block, an underrun -- the pre-roll waited for again. */
static void mix_take(ring_t *r, uint32_t *underruns)
{
    if (!r->playing) return;
    r->dry = 0;
    if (r->level >= BLOCK_N) {
        r->level -= BLOCK_N;
        return;
    }
    r->level = 0;
    r->playing = false;
    (*underruns)++;
}

/* The two mixed, a block at a time: each ring starts once it holds its
 * pre-roll, and while neither plays the mixer waits. Under s_am. */
static void mix(size_t due)
{
    static size_t owed;                     /* the mixer's time not played yet */
    for (owed += due; owed >= BLOCK_N; owed -= BLOCK_N) {
        if (!s_main.playing && s_main.level >= s_pre) s_main.playing = true;
        if (!s_sdr.playing && s_sdr.level >= s_sdr_pre) s_sdr.playing = true;
        if (!s_main.playing && !s_sdr.playing) continue;
        mix_take(&s_main, &s_ast.underruns);
        mix_take(&s_sdr, &s_sdr_underruns);
    }
}

static void *player(void *p)
{
    (void)p;
    const int64_t t0 = mono_us();
    uint64_t done = 0;
    for (;;) {
        usleep(2000);
        const uint64_t due = (uint64_t)(mono_us() - t0) * AUDIO_RATE_HZ / 1000000 - done;
        done += due;
        pthread_mutex_lock(&s_am);
        if (shim_sdr_on) mix((size_t)due);
        else             play(&s_main, (size_t)due, s_pre, &s_ast.underruns);
        pthread_mutex_unlock(&s_am);
    }
    return NULL;
}

static void player_start(void)
{
    pthread_t t;
    pthread_create(&t, NULL, player, NULL);
    pthread_detach(t);
}

#define PLAYER() pthread_once(&s_play_once, player_start)

/* The pitch of what is fed, measured as it comes: an upward crossing of
 * zero, with some room either side so the quantising cannot count twice --
 * the knob's own audio, and the second receiver's apart. */
typedef struct { uint64_t n, up; int side; } pitch_t;
static pthread_mutex_t s_pm = PTHREAD_MUTEX_INITIALIZER;
static pitch_t         s_p = { .side = -1 }, s_sp = { .side = -1 };

static void pitch_count(pitch_t *q, const int16_t *p, size_t n)
{
    pthread_mutex_lock(&s_pm);
    for (size_t i = 0; i < n; i++) {
        if (p[i] > 300 && q->side < 0) { q->side = 1; q->up++; }
        else if (p[i] < -300) q->side = -1;
    }
    q->n += n;
    pthread_mutex_unlock(&s_pm);
}

static double pitch_of(pitch_t *q, uint64_t *samples)
{
    pthread_mutex_lock(&s_pm);
    const double hz = q->n ? (double)q->up * AUDIO_RATE_HZ / (double)q->n : 0;
    if (samples) *samples = q->n;
    pthread_mutex_unlock(&s_pm);
    return hz;
}

static void pitch_zero(pitch_t *q)
{
    pthread_mutex_lock(&s_pm);
    q->n = q->up = 0;
    pthread_mutex_unlock(&s_pm);
}

void shim_pitch_reset(void)               { pitch_zero(&s_p); }
double shim_pitch(uint64_t *samples)      { return pitch_of(&s_p, samples); }
void shim_sdr_pitch_reset(void)           { pitch_zero(&s_sp); }
double shim_sdr_pitch(uint64_t *samples)  { return pitch_of(&s_sp, samples); }

void audio_out_sdr(bool on)
{
    PLAYER();
    pthread_mutex_lock(&s_am);
    shim_sdr_on = on;
    if (!on) s_sdr = (ring_t){ 0 };          /* what is left is the session's just ended */
    pthread_mutex_unlock(&s_am);
}

bool audio_out_feed_sdr(const int16_t *p, size_t n)
{
    PLAYER();
    if (!shim_sdr_on || !p || !n) return false;
    pitch_count(&s_sp, p, n);
    shim_sdr_samples += n;
    pthread_mutex_lock(&s_am);
    const bool ok = SDR_N - s_sdr.level >= n;
    if (ok) { s_sdr.level += n; s_sdr_frames++; }
    else    s_sdr_dropped++;
    pthread_mutex_unlock(&s_am);
    return ok;
}

void audio_out_set_balance(int8_t b) { (void)b; }

bool audio_out_feed_pcm16(const int16_t *p, size_t n, uint8_t ch)
{
    (void)ch;
    PLAYER();
    if (!p || !n) return false;
    shim_samples += n;
    pitch_count(&s_p, p, n);
    bool ok = true;
    pthread_mutex_lock(&s_am);
    for (size_t done = 0; done < n;) {
        const size_t k = n - done < PIECE ? n - done : PIECE;
        if (RING_N - s_main.level < k) {
            s_ast.dropped++;
            ok = false;
            break;
        }
        s_main.level += k;
        done += k;
    }
    if (ok) s_ast.frames++;
    pthread_mutex_unlock(&s_am);
    return ok;
}

void audio_out_flush(void)
{
    PLAYER();
    pthread_mutex_lock(&s_am);
    s_main = (ring_t){ 0 };
    shim_flushes++;
    pthread_mutex_unlock(&s_am);
}

void audio_out_trim(size_t keep)
{
    pthread_mutex_lock(&s_am);
    if (s_main.level > keep) s_main.level = keep;
    pthread_mutex_unlock(&s_am);
}

size_t audio_out_queued(void)
{
    pthread_mutex_lock(&s_am);
    const size_t q = s_main.level;
    pthread_mutex_unlock(&s_am);
    return q;
}

size_t audio_out_sdr_queued(void)
{
    pthread_mutex_lock(&s_am);
    const size_t q = s_sdr.level;
    pthread_mutex_unlock(&s_am);
    return q;
}

size_t audio_out_room(void)            { return ROOM_N; }
size_t audio_out_preroll(void)         { return PREROLL_N; }
size_t audio_out_preroll_max(void)     { return GROWN_N; }
size_t audio_out_sdr_room(void)        { return SDR_ROOM; }
size_t audio_out_sdr_preroll(void)     { return SDR_PRE; }
size_t audio_out_sdr_preroll_max(void) { return GROWN_N; }

static size_t clampz(size_t v, size_t lo, size_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void audio_out_set_preroll(size_t n)     { s_pre = clampz(n, PREROLL_N, GROWN_N); }
void audio_out_sdr_set_preroll(size_t n) { s_sdr_pre = clampz(n, SDR_PRE, GROWN_N); }

void audio_out_stats(audio_stats_t *st)
{
    pthread_mutex_lock(&s_am);
    *st = s_ast;
    st->sample_rate = AUDIO_RATE_HZ;
    st->channels = 1;
    pthread_mutex_unlock(&s_am);
}

void audio_out_sdr_stats(audio_stats_t *st)
{
    pthread_mutex_lock(&s_am);
    memset(st, 0, sizeof *st);
    st->frames = s_sdr_frames;
    st->dropped = s_sdr_dropped;
    st->underruns = s_sdr_underruns;
    st->sample_rate = AUDIO_RATE_HZ;
    st->channels = 1;
    pthread_mutex_unlock(&s_am);
}

void shim_sdr_audio(size_t *level, uint32_t *dropped, uint32_t *underruns)
{
    pthread_mutex_lock(&s_am);
    *level = s_sdr.level;
    *dropped = s_sdr_dropped;
    *underruns = s_sdr_underruns;
    pthread_mutex_unlock(&s_am);
}

double shim_now(void) { return (double)esp_timer_get_time() / 1e6; }
