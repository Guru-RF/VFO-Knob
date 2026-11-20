/* TailChaser's audio, from the repeater in focus. See tc_audio.h. */
#include "tc_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_out.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tc_http.h"

static const char *TAG = "tc_audio";

#define WS_CAP      (16 * 1024)     /* a message is 3.2 kB                    */
#define CHUNK       1600            /* samples a message while audio flows    */
#define RECV_MS     100
#define QUIET_MS    300             /* nothing this long: the over has ended  */
#define OFF_RETRY_MS  30000         /* its listening off: asked again         */
#define BUSY_RETRY_MS 10000         /* four listeners there                   */
static const uint16_t BACKOFF_S[] = { 1, 2, 4, 8, 15 };

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    tc_addr_t a;
    uint32_t  id;
    bool      set, monitor;
} s_focus;
static volatile bool     s_muted, s_started;
static volatile uint8_t  s_state;
static volatile uint32_t s_chunks;
static float    s_level = -120.0f;
static uint64_t s_level_t;
/* The over heard now, for its line in the log once it ends. */
static struct { uint64_t t0; uint32_t samples; float peak; } s_over;

static inline uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

void tc_audio_focus(const tc_addr_t *a, uint32_t id, bool monitor)
{
    taskENTER_CRITICAL(&s_mux);
    s_focus.set = a != NULL;
    if (a) s_focus.a = *a;
    s_focus.id      = id;
    s_focus.monitor = monitor;
    taskEXIT_CRITICAL(&s_mux);
}

void tc_audio_set_mute(bool on)
{
    /* Silent at once: what is buffered is not played out first. */
    if (on && !s_muted) audio_out_flush();
    s_muted = on;
}

bool tc_audio_muted(void) { return s_muted; }

float tc_audio_level(void)
{
    taskENTER_CRITICAL(&s_mux);
    const float v = s_level;
    const uint64_t t = s_level_t;
    taskEXIT_CRITICAL(&s_mux);
    return t && now_ms() - t < QUIET_MS ? v : -120.0f;
}

tca_state_t tc_audio_state(void) { return (tca_state_t)s_state; }
uint32_t tc_audio_chunks(void) { return s_chunks; }

const char *tc_audio_state_word(tca_state_t s)
{
    switch (s) {
    case TCA_CONNECTING: return "connecting";
    case TCA_LISTENING:  return "listening";
    case TCA_NO_MONITOR: return "off on the repeater";
    case TCA_BUSY:       return "four listeners there";
    case TCA_FAILED:     return "no answer";
    default:             return "off";
    }
}

static void b64(const uint8_t *in, size_t n, char *out)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0u) |
                           (i + 2 < n ? (uint32_t)in[i + 2] : 0u);
        out[o++] = A[v >> 18 & 63];
        out[o++] = A[v >> 12 & 63];
        out[o++] = i + 1 < n ? A[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? A[v & 63] : '=';
    }
    out[o] = 0;
}

static void mask_of(uint8_t m[4])
{
    const uint32_t r = esp_random();
    memcpy(m, &r, 4);
}

/* The websocket opened: the handshake asked and answered. The bytes that
 * came after the answer -- the first frames -- left at the buffer's start,
 * *have of them. No API key: listening needs none. */
static tc_conn_t *ws_open(const tc_addr_t *a, uint8_t *buf, size_t cap, size_t *have, int *code, tc_http_t *r)
{
    *have = 0;
    *code = 0;
    tc_conn_t *c = tc_conn_open(a, r);
    if (!c) return NULL;
    uint8_t rnd[16];
    for (int i = 0; i < 16; i += 4) mask_of(rnd + i);
    char key[25];
    b64(rnd, sizeof rnd, key);
    const int len = snprintf((char *)buf, cap,
                             "GET /monitor/tx HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\n"
                             "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
                             "User-Agent: vfo-knob\r\n\r\n",
                             a->host, (unsigned)a->port, key);
    if (len <= 0 || (size_t)len >= cap || tc_conn_send(c, buf, (size_t)len) != len) {
        snprintf(r->why, sizeof r->why, "could not send");
        tc_conn_close(c);
        return NULL;
    }
    size_t n = 0;
    const uint64_t until = now_ms() + 5000;
    for (;;) {
        const int k = tc_conn_recv(c, buf + n, cap - 1 - n, 500);
        if (k < 0) {
            snprintf(r->why, sizeof r->why, "closed");
            break;
        }
        n += (size_t)k;
        const size_t head = tc_ws_answer((const char *)buf, n, code);
        if (head) {
            if (*code != 101) {
                snprintf(r->why, sizeof r->why, "answered %d", *code);
                break;
            }
            memmove(buf, buf + head, n - head);
            *have = n - head;
            return c;
        }
        if (n + 1 >= cap || now_ms() >= until) {
            snprintf(r->why, sizeof r->why, "no answer");
            break;
        }
    }
    tc_conn_close(c);
    return NULL;
}

/* A close, as the protocol asks, then the socket. */
static void drop(tc_conn_t **c)
{
    if (!*c) return;
    uint8_t out[8], m[4];
    mask_of(m);
    static const uint8_t normal[2] = { 0x03, 0xE8 };      /* 1000 */
    const size_t k = tc_ws_client_frame(8, normal, 2, m, out, sizeof out);
    tc_conn_send(*c, out, k);
    tc_conn_close(*c);
    *c = NULL;
}

/* One frame: audio played (unless muted) and measured; a ping answered. */
static void take(tc_conn_t *c, uint8_t *buf, const tc_ws_frame_t *f, bool *queued, uint64_t *t_audio)
{
    uint8_t *p = buf + f->off;
    if (f->op == 2 || f->op == 0) {
        const size_t ns = f->len / 2;
        if (!ns) return;
        /* Little-endian, as the node sends it, at an even offset in an
         * aligned buffer (headers of 2, 4 or 10 bytes). */
        const int16_t *pcm = (const int16_t *)(void *)p;
        const float db = tc_pcm_peak_db(pcm, ns);
        const uint64_t t = now_ms();
        taskENTER_CRITICAL(&s_mux);
        s_level   = db;
        s_level_t = t;
        taskEXIT_CRITICAL(&s_mux);
        s_chunks++;
        *t_audio = t;
        if (!s_over.samples) {
            s_over.t0   = t;
            s_over.peak = -120.0f;
        }
        s_over.samples += (uint32_t)ns;
        if (db > s_over.peak) s_over.peak = db;
        if (!s_muted) {
            audio_out_feed_pcm16(pcm, ns, 1);
            *queued = true;
            /* The tail of an over, shorter: played now, not left in the
             * pre-roll for the start of the next. */
            if (ns < CHUNK) {
                audio_out_kick();
                *queued = false;
            }
        }
    } else if (f->op == 9) {
        uint8_t out[6 + 125], m[4];
        mask_of(m);
        const size_t k = tc_ws_client_frame(10, p, f->len, m, out, sizeof out);
        if (k) tc_conn_send(c, out, k);
    }
}

static void audio_task(void *arg)
{
    uint8_t *buf = arg;
    tc_conn_t *c = NULL;
    size_t n = 0;
    uint32_t cur = 0;
    bool cur_set = false, queued = false;
    uint64_t t_retry = 0, t_audio = 0;
    int fails = 0, said = 0;        /* the refusal last logged: once each */
    for (;;) {
        tc_addr_t a;
        uint32_t id;
        bool set, mon;
        taskENTER_CRITICAL(&s_mux);
        a   = s_focus.a;
        id  = s_focus.id;
        set = s_focus.set;
        mon = s_focus.monitor;
        taskEXIT_CRITICAL(&s_mux);

        /* Another repeater in focus, or none: the one before let go, and
         * what of it is still buffered. */
        if (set != cur_set || (set && id != cur)) {
            if (c) {
                drop(&c);
                audio_out_flush();
            }
            s_over.samples = 0;
            cur = id;
            cur_set = set;
            fails = 0;
            said = 0;
            t_retry = 0;
            n = 0;
            queued = false;
        }
        const uint64_t t = now_ms();
        if (!set) {
            s_state = TCA_OFF;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (c && !mon) {
            drop(&c);           /* its listening turned off: its status says so */
            n = 0;
        }
        if (!c) {
            if (!mon) {
                s_state = TCA_NO_MONITOR;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (t < t_retry) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            s_state = TCA_CONNECTING;
            tc_http_t r;
            int code;
            c = ws_open(&a, buf, WS_CAP, &n, &code, &r);
            if (!c) {
                if (code == 404) {
                    if (said != 404) ESP_LOGW(TAG, "%s: its listening is off ([status] monitor)", a.host);
                    said = 404;
                    s_state = TCA_NO_MONITOR;
                    t_retry = now_ms() + OFF_RETRY_MS;
                } else if (code == 503) {
                    if (said != 503) ESP_LOGW(TAG, "%s: four listeners there already", a.host);
                    said = 503;
                    s_state = TCA_BUSY;
                    t_retry = now_ms() + BUSY_RETRY_MS;
                } else {
                    const int k = fails < (int)(sizeof BACKOFF_S / sizeof BACKOFF_S[0]) ? fails
                                                                                          : (int)(sizeof BACKOFF_S / sizeof BACKOFF_S[0]) - 1;
                    if (!fails || fails == 3) ESP_LOGW(TAG, "%s: %s", a.host, r.why);
                    fails++;
                    s_state = TCA_FAILED;
                    t_retry = now_ms() + BACKOFF_S[k] * 1000u;
                }
                n = 0;
                continue;
            }
            ESP_LOGI(TAG, "%s: listening%s", a.host, r.tailnet ? ", through the tailnet" : "");
            s_state = TCA_LISTENING;
            fails = 0;
            said = 0;
            queued = false;
        }

        bool bad = false;
        for (;;) {
            tc_ws_frame_t f;
            const long k = tc_ws_frame(buf, n, &f);
            if (k < 0) {
                bad = true;
                break;
            }
            if (k == 0) break;
            take(c, buf, &f, &queued, &t_audio);
            memmove(buf, buf + k, n - (size_t)k);
            n -= (size_t)k;
            if (f.op == 8) {
                bad = true;     /* the node closed it */
                break;
            }
        }
        if (!bad && n == WS_CAP) bad = true;    /* bigger than any message of the node's */
        if (!bad) {
            const int k = tc_conn_recv(c, buf + n, WS_CAP - n, RECV_MS);
            if (k < 0) bad = true;
            else n += (size_t)k;
        }
        if (queued && now_ms() - t_audio >= QUIET_MS) {
            audio_out_kick();
            queued = false;
        }
        if (s_over.samples && now_ms() - t_audio >= QUIET_MS) {
            ESP_LOGI(TAG, "%s: an over, %.1f s, peak %.0f dBFS%s", a.host, s_over.samples / 16000.0,
                     (double)s_over.peak, s_muted ? ", muted" : "");
            s_over.samples = 0;
        }
        if (bad) {
            ESP_LOGW(TAG, "%s: the stream ended", a.host);
            drop(&c);
            n = 0;
            s_state = TCA_FAILED;
            t_retry = now_ms() + 1000;
        }
    }
}

bool tc_audio_start(void)
{
    if (s_started) return true;
    uint8_t *buf = heap_caps_malloc(WS_CAP, MALLOC_CAP_SPIRAM);
    if (!buf) return false;
    /* Core 0 beside lwIP and the readers, above them: a message waiting is
     * sound waiting. Its stack in PSRAM: it never touches flash. */
    if (xTaskCreatePinnedToCoreWithCaps(audio_task, "tc_audio", 8192, buf, 6, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        free(buf);
        return false;
    }
    s_started = true;
    return true;
}
