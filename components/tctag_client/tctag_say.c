/* TailChaser Tag: each new over's callsign said out loud (tctag_say.h).
 *
 * ElevenLabs makes the voice: one HTTPS request per text, its answer raw
 * 16-bit PCM at the knob's own 24 kHz, kept on the card (VFO-TTS/, a file per
 * text, voice and model) so each callsign is asked for once. It plays as the
 * radio's audio does: the jack, and through the second chip a Bluetooth
 * speaker or headset (bt_link's tap on audio_out).
 *
 * The key is the owner's: on the card (kvstore "tagtts"), sent to ElevenLabs
 * alone, never logged, never sent back to the page -- only whether there is
 * one. What ElevenLabs is sent: the callsign as text, and the voice. */
#include "tctag_say.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_out.h"
#include "bt_link.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kvstore.h"
#include "sd_cache.h"
#include "tag.h"

static const char *TAG = "tctag_say";

#define NS         "tagtts"
#define MODEL      "eleven_flash_v2_5"         /* fast, and half the price of the others */
#define VOICE_DEF  "JBFqnCBsd6RMkjVDRZzb"      /* "George", one of ElevenLabs' own */
#define MAX_S      10                          /* the longest a callsign takes: 10 s */
#define PCM_CAP    (AUDIO_RATE_HZ * 2 * MAX_S)
#define CHUNK      256                         /* frames: audio_out_feed_pcm16's whole-or-nothing piece */
#define DIR        "VFO-TTS"

typedef struct {
    char call[TAG_CALL_LEN + 1];
    bool test;
} item_t;

static SemaphoreHandle_t s_mx;                 /* s_cfg, s_state and the counts */
static tctag_say_cfg_t   s_cfg;
static char              s_state[48] = "off";
static uint32_t          s_said, s_asked, s_cached;
static QueueHandle_t     s_q;
static uint8_t          *s_pcm;                /* PSRAM, PCM_CAP */

static void set_state(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_state(const char *fmt, ...)
{
    char b[sizeof s_state];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    strlcpy(s_state, b, sizeof s_state);
    xSemaphoreGive(s_mx);
}

/* ---- the settings, on the card ---- */

static void cfg_load(tctag_say_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    strlcpy(c->voice, VOICE_DEF, sizeof c->voice);
    kv_handle_t h;
    if (kv_open(NS, &h) != ESP_OK) return;
    uint8_t v;
    if (kv_get_u8(h, "on", &v) == ESP_OK) c->on = v != 0;
    if (kv_get_u8(h, "style", &v) == ESP_OK) c->phonetic = v != 0;
    size_t n = sizeof c->voice;
    char voice[sizeof c->voice];
    if (kv_get_str(h, "voice", voice, &n) == ESP_OK && voice[0]) strlcpy(c->voice, voice, sizeof c->voice);
    n = sizeof c->key;
    if (kv_get_str(h, "key", c->key, &n) != ESP_OK) c->key[0] = 0;
}

static void cfg_save(const tctag_say_cfg_t *c)
{
    kv_handle_t h;
    if (kv_open(NS, &h) != ESP_OK) return;
    kv_edit_begin(h);
    kv_set_u8(h, "on", c->on);
    kv_set_u8(h, "style", c->phonetic);
    kv_set_str(h, "voice", c->voice);
    if (c->key[0]) kv_set_str(h, "key", c->key);
    else kv_erase_key(h, "key");
    kv_edit_end(h);
    kv_commit(h);
}

/* ---- the card: one file a text, voice and model ---- */

static uint32_t fnv1a(uint32_t h, const char *s)
{
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static void clip_path(char *p, size_t n, const char *drive, const char *voice, const char *text)
{
    uint32_t h = fnv1a(2166136261u, voice);
    h = fnv1a(h, "|" MODEL "|");
    h = fnv1a(h, text);
    snprintf(p, n, "%s/" DIR "/%08" PRIX32 ".PCM", drive, h);
}

/* The clip for text from the card: its length in bytes, 0 when there is none. */
static size_t clip_load(const char *voice, const char *text)
{
    if (!sdc_mount()) return 0;
    char p[48];
    clip_path(p, sizeof p, sdc_drive(), voice, text);
    FIL f;
    size_t got = 0;
    if (f_open(&f, p, FA_READ) == FR_OK) {
        const FSIZE_t sz = f_size(&f);
        UINT n = 0;
        if (sz > 0 && sz <= PCM_CAP && f_read(&f, s_pcm, (UINT)sz, &n) == FR_OK && n == sz) got = n & ~(size_t)1;
        f_close(&f);
    }
    sdc_unmount();
    return got;
}

static void clip_save(const char *voice, const char *text, size_t len)
{
    if (!sdc_mount()) return;
    char d[24], p[48];
    snprintf(d, sizeof d, "%s/" DIR, sdc_drive());
    f_mkdir(d);                                          /* FR_EXIST after the first */
    clip_path(p, sizeof p, sdc_drive(), voice, text);
    FIL f;
    if (f_open(&f, p, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        UINT n = 0;
        const bool ok = f_write(&f, s_pcm, (UINT)len, &n) == FR_OK && n == len;
        f_close(&f);
        if (!ok) f_unlink(p);                            /* half a clip is none */
    }
    sdc_unmount();
}

/* ---- ElevenLabs ---- */

/* text into JSON's string: a callsign's words need no escaping but this. */
static void json_str(char *o, size_t n, const char *s)
{
    size_t k = 0;
    for (; *s && k + 2 < n; s++) {
        if (*s == '"' || *s == '\\') o[k++] = '\\';
        o[k++] = *s;
    }
    o[k] = 0;
}

/* The clip for text into s_pcm: its length, or 0 with the state set. */
static size_t fetch(const tctag_say_cfg_t *c, const char *text)
{
    char url[160], esc[200], body[300];
    snprintf(url, sizeof url, "https://api.elevenlabs.io/v1/text-to-speech/%s?output_format=pcm_%d", c->voice,
             AUDIO_RATE_HZ);
    json_str(esc, sizeof esc, text);
    const int bn = snprintf(body, sizeof body, "{\"text\":\"%s\",\"model_id\":\"" MODEL "\"}", esc);
    const esp_http_client_config_t hc = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .buffer_size       = 4096,
        .buffer_size_tx    = 1024,                   /* the request line and the key: past the default 512 */
    };
    esp_http_client_handle_t h = esp_http_client_init(&hc);
    if (!h) {
        set_state("no memory");
        return 0;
    }
    esp_http_client_set_header(h, "xi-api-key", c->key);
    esp_http_client_set_header(h, "Content-Type", "application/json");
    size_t got = 0;
    esp_err_t err = esp_http_client_open(h, bn);
    if (err == ESP_OK && esp_http_client_write(h, body, bn) != bn) err = ESP_FAIL;
    int status = -1;
    if (err == ESP_OK && esp_http_client_fetch_headers(h) >= 0) status = esp_http_client_get_status_code(h);
    if (status == 200) {
        for (;;) {
            if (got >= PCM_CAP) break;                   /* the rest of a long one: cut */
            const int n = esp_http_client_read(h, (char *)s_pcm + got, (int)(PCM_CAP - got));
            if (n <= 0) break;
            got += (size_t)n;
        }
        got &= ~(size_t)1;
        if (!got) set_state("ElevenLabs sent nothing");
    } else if (status > 0) {
        /* Their reason, without the key (it is never in an answer). */
        char why[160] = "";
        const int n = esp_http_client_read(h, why, sizeof why - 1);
        why[n > 0 ? n : 0] = 0;
        ESP_LOGW(TAG, "ElevenLabs: HTTP %d %.120s", status, why);
        set_state(status == 401 ? "key refused" : status == 429 ? "too many, or no credits left"
                  : status == 404 || status == 400 ? "no such voice" : "ElevenLabs: HTTP %d", status);
    } else {
        ESP_LOGW(TAG, "ElevenLabs: %s", esp_err_to_name(err));
        set_state("ElevenLabs did not answer");
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return status == 200 ? got : 0;
}

/* ---- playing ---- */

static void play(size_t bytes)
{
    const int16_t *pcm = (const int16_t *)s_pcm;
    const size_t frames = bytes / 2;
    for (size_t at = 0; at < frames;) {
        const size_t n = frames - at < CHUNK ? frames - at : CHUNK;
        if (!audio_out_feed_pcm16(pcm + at, n, 1)) {     /* the ring full: a piece is whole or not sent */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        at += n;
    }
    audio_out_kick();                                    /* a short one, under the pre-roll: played too */
}

static void say_task(void *arg)
{
    (void)arg;
    item_t it;
    for (;;) {
        if (xQueueReceive(s_q, &it, portMAX_DELAY) != pdTRUE) continue;
        tctag_say_cfg_t c;
        xSemaphoreTake(s_mx, portMAX_DELAY);
        c = s_cfg;
        xSemaphoreGive(s_mx);
        if (!c.on && !it.test) continue;
        if (!c.key[0]) {
            set_state("no ElevenLabs key");
            continue;
        }
        char text[160];
        if (!tag_say_text(it.call, c.phonetic, text, sizeof text)) continue;
        if (!s_pcm && !(s_pcm = heap_caps_malloc(PCM_CAP, MALLOC_CAP_SPIRAM))) {
            set_state("no memory");
            continue;
        }
        size_t len = clip_load(c.voice, text);
        const bool card = len != 0;
        if (card) {
            xSemaphoreTake(s_mx, portMAX_DELAY);
            s_cached++;
            xSemaphoreGive(s_mx);
        } else {
            set_state("asking ElevenLabs for %s", it.call);
            len = fetch(&c, text);
            if (!len) continue;
            xSemaphoreTake(s_mx, portMAX_DELAY);
            s_asked++;
            xSemaphoreGive(s_mx);
            clip_save(c.voice, text, len);
        }
        ESP_LOGI(TAG, "%s: %.1f s, %s", it.call, (double)len / 2 / AUDIO_RATE_HZ,
                 card ? "from the card" : "from ElevenLabs, now on the card");

        set_state("saying %s", it.call);
        play(len);
        xSemaphoreTake(s_mx, portMAX_DELAY);
        s_said++;
        snprintf(s_state, sizeof s_state, "said %s", it.call);
        xSemaphoreGive(s_mx);
    }
}

/* ---- the rest of the firmware's ---- */

void tctag_say_start(void)
{
    if (s_mx) return;
    s_mx = xSemaphoreCreateMutex();
    s_q = xQueueCreate(4, sizeof(item_t));
    if (!s_mx || !s_q) return;
    cfg_load(&s_cfg);
    strlcpy(s_state, s_cfg.on ? (s_cfg.key[0] ? "ready" : "no ElevenLabs key") : "off", sizeof s_state);
    /* A Bluetooth speaker or headset is wanted only while there is something
     * to say to it. */
    bt_link_want_audio(s_cfg.on);
    /* TLS in it: a task of its own, its stack in PSRAM as the Weather's. */
    if (xTaskCreatePinnedToCoreWithCaps(say_task, "tctag_say", 8192, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS)
        ESP_LOGE(TAG, "no room for its task");
    ESP_LOGI(TAG, "speech %s%s", s_cfg.on ? "on" : "off", s_cfg.on && !s_cfg.key[0] ? ", but no key" : "");
}

static void post(const char *call, bool test)
{
    if (!s_q || !tag_call_ok(call)) return;
    item_t it = { .test = test };
    strlcpy(it.call, call, sizeof it.call);
    xQueueSend(s_q, &it, 0);                             /* full: this one is not said */
}

void tctag_say_new(const char *call) { post(call, false); }

bool tctag_say_test(const char *call)
{
    if (!tag_call_ok(call)) return false;
    post(call, true);
    return true;
}

void tctag_say_info(tctag_say_info_t *o)
{
    memset(o, 0, sizeof *o);
    if (!s_mx) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    o->on = s_cfg.on;
    o->phonetic = s_cfg.phonetic;
    o->key = s_cfg.key[0] != 0;
    strlcpy(o->voice, s_cfg.voice, sizeof o->voice);
    strlcpy(o->state, s_state, sizeof o->state);
    o->said = s_said;
    o->asked = s_asked;
    o->cached = s_cached;
    xSemaphoreGive(s_mx);
}

const char *tctag_say_set(const tctag_say_set_t *s)
{
    if (!s_mx) return "not started";
    tctag_say_cfg_t c;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    c = s_cfg;
    xSemaphoreGive(s_mx);
    if (s->have_on) c.on = s->on;
    if (s->have_phonetic) c.phonetic = s->phonetic;
    if (s->voice) {
        if (!s->voice[0]) {
            strlcpy(c.voice, VOICE_DEF, sizeof c.voice);
        } else {
            if (strlen(s->voice) >= sizeof c.voice) return "the voice's ID is too long";
            for (const char *p = s->voice; *p; p++)
                if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')))
                    return "a voice's ID is letters and digits (ElevenLabs' Voices, the ID beside one)";
            strlcpy(c.voice, s->voice, sizeof c.voice);
        }
    }
    if (s->key && s->key[0]) {
        if (strlen(s->key) >= sizeof c.key) return "the key is too long";
        for (const char *p = s->key; *p; p++)
            if ((unsigned char)*p < 0x21 || (unsigned char)*p > 0x7E || *p == '"' || *p == '\\')
                return "the key has a character no ElevenLabs key has";
        strlcpy(c.key, s->key, sizeof c.key);
    }
    if (s->key_clear) c.key[0] = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_cfg = c;
    strlcpy(s_state, c.on ? (c.key[0] ? "ready" : "no ElevenLabs key") : "off", sizeof s_state);
    xSemaphoreGive(s_mx);
    cfg_save(&c);
    bt_link_want_audio(c.on);
    /* Never the key: only that there is one. */
    ESP_LOGI(TAG, "saved: speech %s, %s, voice %s, %s", c.on ? "on" : "off", c.phonetic ? "phonetic" : "letters",
             c.voice, c.key[0] ? "a key set" : "no key");
    return NULL;
}
