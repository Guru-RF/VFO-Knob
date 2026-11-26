/* TailChaser Tag on the configuration page, over HTTP. Registered by webcfg,
 * behind its login (radio_web_endpoints() in tctag_client.c).
 *
 *   GET  /api/tctag     how the listening goes -- the levels, the start's
 *                       match, the counts -- and who was heard, the newest
 *                       first, as JSON
 *   POST /api/tctag     form-encoded: act=clear, the list emptied, here and
 *                       on the card; act=speech, the callsigns said out loud
 *                       -- on=0/1, phonetic=0/1, voice, key (kept unless one
 *                       is sent), keyclear=1; act=say&call=, said now
 *
 * The ElevenLabs key goes one way: the page sends it, the knob keeps it on
 * its card and tells the page only whether there is one.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "tctag_client.h"
#include "tctag_say.h"

static const char *TAG = "tctag_web";

#define JSON_CAP (TAG_HIST_N * 160 + 1536)
#define BODY_MAX 512

typedef struct {
    char  *b;
    size_t n, o;
} out_t;

static void put(out_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(out_t *j, const char *fmt, ...)
{
    if (j->o >= j->n) return;
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(j->b + j->o, j->n - j->o, fmt, ap);
    va_end(ap);
    if (k > 0) j->o += (size_t)k;
}

static esp_err_t tg_get(httpd_req_t *r)
{
    tag_view_t *v = heap_caps_malloc(sizeof *v, MALLOC_CAP_SPIRAM);
    char *b = heap_caps_malloc(JSON_CAP, MALLOC_CAP_SPIRAM);
    if (!v || !b) {
        free(v);
        free(b);
        return httpd_resp_send_500(r);
    }
    tctag_view(v);
    const tag_stats_t *s = &v->st;
    out_t j = { b, JSON_CAP, 0 };
    /* A call is upper case, digits, '-' and '/' alone (tag_call_ok): nothing
     * in it to escape. */
    put(&j, "{\"listening\":%s,\"band\":%.1f,\"floor\":%.1f,\"all\":%.1f,\"sync_db\":%.1f,"
            "\"sync_now\":%.3f,\"sync_avg\":%.3f,\"holding\":%s,\"tries\":%" PRIu32 ",\"read\":%" PRIu32
            ",\"crc_bad\":%" PRIu32 ",\"heard_s\":%" PRIu64 ",\"cpu\":%u,\"lost\":%" PRIu32 ",\"now\":%" PRId64
            ",\"try_db\":%.1f,\"try_avg\":%.3f,\"sure_avg\":%.3f,\"format\":%d,\"list\":[",
        v->listening ? "true" : "false", (double)s->band_dbfs, (double)s->floor_dbfs, (double)s->all_dbfs,
        (double)s->sync_db, (double)s->sync_now, (double)s->sync_avg, s->holding ? "true" : "false", s->tries,
        s->readings, s->crc_bad, s->heard / TAG_FS, (unsigned)v->cpu, v->lost, v->now_unix, (double)TAG_TRY_DB,
        (double)TAG_TRY_AVG, (double)TAG_SURE_AVG, TAG_FORMAT);
    for (int i = 0; i < tag_hist_count(&v->h); i++) {
        const tag_entry_t *e = tag_hist_at(&v->h, i);
        /* This start's: how long ago, and how long, from the knob's own clock
         * -- known before NTP is. */
        put(&j, "%s{\"call\":\"%s\",\"local\":%s,\"first\":%" PRId64 ",\"last\":%" PRId64 ",\"reads\":%u", i ? "," : "",
            e->call, e->local ? "true" : "false", e->first_unix, e->last_unix, (unsigned)e->reads);
        if (e->now_start)
            put(&j, ",\"ago_s\":%" PRId64 ",\"len_s\":%" PRId64, (v->now_ms - e->last_ms) / 1000,
                (e->last_ms - e->first_ms) / 1000);
        put(&j, "}");
    }
    tctag_say_info_t sy;
    tctag_say_info(&sy);
    /* The voice's ID is letters and digits (tctag_say_set), the state the
     * knob's own words and a callsign. */
    put(&j, "],\"say\":{\"on\":%s,\"phonetic\":%s,\"key\":%s,\"voice\":\"%s\",\"state\":\"%s\",\"said\":%" PRIu32
            ",\"asked\":%" PRIu32 ",\"cached\":%" PRIu32 "}}",
        sy.on ? "true" : "false", sy.phonetic ? "true" : "false", sy.key ? "true" : "false", sy.voice, sy.state,
        sy.said, sy.asked, sy.cached);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(r, b, (ssize_t)j.o);
    free(v);
    free(b);
    return err;
}

static esp_err_t refuse(httpd_req_t *r, const char *why)
{
    httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, why);
    return ESP_FAIL;
}

/* A form field into out: ESP_ERR_NOT_FOUND when absent, an error when too long. */
static esp_err_t field(const char *body, const char *key, char *out, size_t n)
{
    out[0] = 0;
    const esp_err_t e = httpd_query_key_value(body, key, out, n);
    if (e == ESP_OK) {
        /* %xx and + as the browser sends them, in place. */
        char *w = out;
        for (const char *p = out; *p; p++) {
            if (*p == '+') *w++ = ' ';
            else if (*p == '%' && p[1] && p[2]) {
                const char h[3] = { p[1], p[2], 0 };
                *w++ = (char)strtol(h, NULL, 16);
                p += 2;
            } else *w++ = *p;
        }
        *w = 0;
    }
    return e;
}

static esp_err_t tg_post(httpd_req_t *r)
{
    const int total = r->content_len;
    if (total <= 0 || total >= BODY_MAX) return refuse(r, "body size");
    char body[BODY_MAX];
    int got = 0;
    while (got < total) {
        const int k = httpd_req_recv(r, body + got, total - got);
        if (k <= 0) return ESP_FAIL;
        got += k;
    }
    body[got] = 0;
    char act[16] = "";
    httpd_query_key_value(body, "act", act, sizeof act);
    if (!strcmp(act, "clear")) {
        tctag_clear();
        ESP_LOGI(TAG, "the page empties the list");
        return httpd_resp_sendstr(r, "ok");
    }
    if (!strcmp(act, "say")) {
        char call[16];
        field(body, "call", call, sizeof call);
        for (char *p = call; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
        if (!tctag_say_test(call)) return refuse(r, "a callsign: up to 9 of A-Z, 0-9, / and -");
        return httpd_resp_sendstr(r, "ok");
    }
    if (!strcmp(act, "speech")) {
        char on[4], ph[4], kc[4], voice[40], key[160];
        tctag_say_set_t s = { 0 };
        if (field(body, "on", on, sizeof on) == ESP_OK) { s.have_on = true; s.on = on[0] == '1'; }
        if (field(body, "phonetic", ph, sizeof ph) == ESP_OK) { s.have_phonetic = true; s.phonetic = ph[0] == '1'; }
        if (field(body, "keyclear", kc, sizeof kc) == ESP_OK) s.key_clear = kc[0] == '1';
        esp_err_t e = field(body, "voice", voice, sizeof voice);
        if (e == ESP_OK) s.voice = voice;
        else if (e != ESP_ERR_NOT_FOUND) return refuse(r, "the voice's ID is too long");
        e = field(body, "key", key, sizeof key);
        if (e == ESP_OK) s.key = key;
        else if (e != ESP_ERR_NOT_FOUND) return refuse(r, "the key is too long");
        const char *why = tctag_say_set(&s);
        memset(key, 0, sizeof key);
        memset(body, 0, sizeof body);                        /* the key: not left on the stack */
        if (why) return refuse(r, why);
        return httpd_resp_sendstr(r, "ok");
    }
    return refuse(r, "act: clear, speech or say");
}

const httpd_uri_t tctag_web_uris[] = {
    { .uri = "/api/tctag", .method = HTTP_GET,  .handler = tg_get },
    { .uri = "/api/tctag", .method = HTTP_POST, .handler = tg_post },
};
const size_t tctag_web_uris_n = sizeof tctag_web_uris / sizeof tctag_web_uris[0];
