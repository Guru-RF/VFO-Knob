/* A WebSocket client over a radio's Bluetooth serial link: ws_bt.h. */
#include "ws_bt.h"

/* Not the icombt firmware's: its radio speaks CI-V, not TCI. */
#if VFO_BT_RADIO && !VFO_RADIO_ICOM

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "bt_link.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"

static const char *TAG = "ws_bt";

#define RB_SIZE     4096             /* read from the link, not given out yet */
#define REOPEN_US   1000000          /* an OPEN the second chip has not taken: asked again */
#define TICK_MS     20               /* the task's wait, with nothing to wake it */
#define PAYLOAD_MAX (1u << 20)       /* a frame longer than this makes no sense here */
#define GUID        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

struct ws_bt {
    ws_bt_config_t    cfg;
    char              host[64], path[64], ua[64];
    ws_bt_handler_t   cb;
    void             *cb_arg;
    TaskHandle_t      task;           /* compared with, never used: it deletes itself */
    bool              running;        /* started, and not stopped since */
    SemaphoreHandle_t done;           /* given by the task as it ends */
    SemaphoreHandle_t tx;             /* one frame at a time onto the link */
    volatile bool     stop_req, close_req;
    volatile TickType_t close_tmo;
    volatile uint8_t  conn;           /* the link's, while it is up */
    volatile bool     connected;      /* upgraded: frames may go */
    uint8_t          *rb;
    size_t            rb_len;
    /* the frame being read */
    bool              in_payload, masked;
    uint8_t           op, mask[4];
    uint64_t          pl_len, pl_off;
    char              why[96];
};

/* The task the link's bytes wake: the running client's. Under s_wmx -- set
 * and cleared by the task itself, read by the link's task and the API -- so
 * nothing notifies a task that has gone. */
static SemaphoreHandle_t s_wmx;
static TaskHandle_t      s_wake;

static void poke(void)
{
    if (!s_wmx) return;
    xSemaphoreTake(s_wmx, portMAX_DELAY);
    if (s_wake) xTaskNotifyGive(s_wake);
    xSemaphoreGive(s_wmx);
}

static void set_wake(TaskHandle_t t)
{
    xSemaphoreTake(s_wmx, portMAX_DELAY);
    s_wake = t;
    xSemaphoreGive(s_wmx);
}

static bool on_task(const struct ws_bt *c) { return c->task && xTaskGetCurrentTaskHandle() == c->task; }

static void emit(struct ws_bt *c, int32_t id, ws_bt_event_data_t *d)
{
    if (c->cb) c->cb(c->cb_arg, NULL, id, d);
}

static void say(struct ws_bt *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(struct ws_bt *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->why, sizeof c->why, fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s", c->why);
}

/* ---- onto the link --------------------------------------------------- */

/* Waits, up to timeout from t0, until the link takes n bytes whole. Under
 * c->tx: only this writes to the link, so the room only grows meanwhile. */
static bool room_for(struct ws_bt *c, uint8_t conn, size_t n, TickType_t t0, TickType_t timeout)
{
    while (bt_link_radio_room(conn) < n) {
        if (c->conn != conn || xTaskGetTickCount() - t0 >= timeout) return false;
        vTaskDelay(1);
    }
    return true;
}

static bool write_all(uint8_t conn, const void *p, size_t n)
{
    return bt_link_radio_write(conn, p, n) == n;
}

/* One frame, masked as a client's are, whole or not at all. */
static int send_frame(struct ws_bt *c, uint8_t op, const void *data, size_t len, TickType_t timeout)
{
    const uint8_t conn = c->conn;
    if (!conn) return -1;
    uint8_t h[14];
    size_t  hl = 0;
    h[hl++] = 0x80 | op;
    if (len < 126) {
        h[hl++] = 0x80 | (uint8_t)len;
    } else if (len < 65536) {
        h[hl++] = 0x80 | 126;
        h[hl++] = (uint8_t)(len >> 8);
        h[hl++] = (uint8_t)len;
    } else {
        h[hl++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) h[hl++] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    const uint32_t m  = esp_random();
    uint8_t       *mk = h + hl;
    memcpy(mk, &m, 4);
    hl += 4;

    const TickType_t t0 = xTaskGetTickCount();
    if (xSemaphoreTake(c->tx, timeout) != pdTRUE) return -1;
    bool ok = room_for(c, conn, hl + len, t0, timeout) && write_all(conn, h, hl);
    uint8_t chunk[256];
    for (size_t i = 0; ok && i < len;) {
        const size_t n = len - i < sizeof chunk ? len - i : sizeof chunk;
        for (size_t k = 0; k < n; k++) chunk[k] = ((const uint8_t *)data)[i + k] ^ mk[(i + k) & 3];
        ok = write_all(conn, chunk, n);
        i += n;
    }
    xSemaphoreGive(c->tx);
    return ok ? (int)len : -1;
}

int ws_bt_send_text(ws_bt_handle_t c, const char *data, int len, TickType_t timeout)
{
    if (!c || !c->connected || len < 0) return -1;
    return send_frame(c, 0x1, data, (size_t)len, timeout);
}

int ws_bt_send_bin(ws_bt_handle_t c, const char *data, int len, TickType_t timeout)
{
    if (!c || !c->connected || len < 0) return -1;
    return send_frame(c, 0x2, data, (size_t)len, timeout);
}

bool ws_bt_has_room(ws_bt_handle_t c, size_t len)
{
    const uint8_t conn = c ? c->conn : 0;
    return conn && c->connected && bt_link_radio_room(conn) >= len + 14;
}

bool ws_bt_is_connected(ws_bt_handle_t c) { return c && c->connected; }

const char *ws_bt_why(ws_bt_handle_t c) { return c ? c->why : ""; }

/* ---- the link -------------------------------------------------------- */

static const char *why_str(uint8_t why)
{
    switch (why) {
    case BTL_RSWHY_KNOB:       return "closed from here";
    case BTL_RSWHY_REMOTE:     return "the radio closed it, or it went out of range";
    case BTL_RSWHY_NO_SERVICE: return "the radio has no TCI over Bluetooth: TCI off, or OpenX6X00 too old";
    case BTL_RSWHY_SDP:        return "the radio did not answer";
    case BTL_RSWHY_REFUSED:    return "refused, or not there: pair it in the radio's pairing window";
    case BTL_RSWHY_OVERFLOW:   return "the second chip could not keep up";
    case BTL_RSWHY_BUSY:       return "the second chip was busy";
    default:                   return "the second chip went quiet, or restarted";
    }
}

/* An RFCOMM link to the radio, through the second chip: true when it is
 * open, c->conn its number. */
static bool link_up(struct ws_bt *c, int64_t until)
{
    if (!bt_link_radio_can()) {
        say(c, "the second chip cannot carry a radio's link: its firmware is too old, or it does not answer");
        return false;
    }
    uint32_t base    = bt_link_radio_open(c->cfg.bda, c->cfg.scn);
    int64_t  t_asked = esp_timer_get_time();
    bool     taken   = false;
    uint8_t  last    = 0xFF;
    int64_t  stale   = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
        if (c->stop_req || c->close_req) return false;
        btl_radio_state_t st;
        uint32_t          reports;
        uint8_t           conn;
        bt_link_radio_state(&st, &reports, &conn);
        const bool ours = !memcmp(st.bda, c->cfg.bda, 6);
        if (base && (int32_t)(reports - base) >= 0) {
            if (st.state == BTL_RS_OPEN && ours && conn && st.conn == conn) {
                c->conn = conn;
                ESP_LOGI(TAG, "link %u open, channel %u", conn, st.scn);
                return true;
            }
            /* Open, but a link this end closed: its CLOSE lost on the wire.
             * Closed again, and asked afresh; what is said meanwhile -- the
             * close -- is no answer. */
            if (st.state == BTL_RS_OPEN && ours && !conn) {
                const int64_t now = esp_timer_get_time();
                if (!stale) stale = now;
                if (now - stale > REOPEN_US) {
                    ESP_LOGW(TAG, "link %u still open from before: closing it", st.conn);
                    bt_link_radio_close();
                    stale   = 0;
                    taken   = false;
                    base    = reports + 1;
                    t_asked = now - REOPEN_US / 2;   /* asked again in half the time */
                    continue;
                }
            }
            if (st.state != BTL_RS_IDLE && ours) {
                taken = true;
                if (st.state != last) {
                    last = st.state;
                    ESP_LOGI(TAG, "%s", st.state == BTL_RS_LOOKING ? "looking for the radio's TCI" : "connecting");
                }
            } else if (taken && st.state == BTL_RS_IDLE) {
                say(c, "no link: %s", why_str(st.why));
                return false;
            }
        }
        const int64_t now = esp_timer_get_time();
        if (now > until) {
            say(c, "no link within %d s%s", c->cfg.connect_timeout_ms / 1000,
                taken ? "" : ": the second chip did not take the request");
            return false;
        }
        if (!taken && now - t_asked > REOPEN_US) {
            base    = bt_link_radio_open(c->cfg.bda, c->cfg.scn);
            t_asked = now;
        }
    }
}

static const uint8_t *find_crlf2(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 4 <= n; i++)
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') return p + i;
    return NULL;
}

/* A header's value in the answer, its line's start p: true and copied when
 * it is this one. */
static bool header(const char *p, const char *name, char *out, size_t max)
{
    const size_t nl = strlen(name);
    if (strncasecmp(p, name, nl) || p[nl] != ':') return false;
    p += nl + 1;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    while (p[n] && p[n] != '\r' && p[n] != '\n' && n + 1 < max) n++;
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

/* The HTTP upgrade, over the open link: true when the radio answers 101 with
 * the right Sec-WebSocket-Accept. What follows its answer stays in c->rb. */
static bool upgrade(struct ws_bt *c, int64_t until)
{
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof rnd);
    char   key[32];
    size_t kl = 0;
    mbedtls_base64_encode((unsigned char *)key, sizeof key - 1, &kl, rnd, sizeof rnd);
    key[kl] = 0;

    char req[384];
    const int n = snprintf(req, sizeof req,
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Key: %s\r\n"
                           "Sec-WebSocket-Version: 13\r\n"
                           "%s%s%s"
                           "\r\n",
                           c->path, c->host, key, c->ua[0] ? "User-Agent: " : "", c->ua, c->ua[0] ? "\r\n" : "");
    if (n <= 0 || n >= (int)sizeof req) {
        say(c, "the upgrade's request is too long");
        return false;
    }
    const uint8_t conn = c->conn;
    xSemaphoreTake(c->tx, portMAX_DELAY);
    const bool sent = room_for(c, conn, (size_t)n, xTaskGetTickCount(), pdMS_TO_TICKS(2000)) &&
                      write_all(conn, req, (size_t)n);
    xSemaphoreGive(c->tx);
    if (!sent) {
        say(c, "the upgrade could not be sent");
        return false;
    }

    char    cat[sizeof key + sizeof GUID];
    uint8_t sha[20];
    char    want[32];
    size_t  wl = 0;
    snprintf(cat, sizeof cat, "%s%s", key, GUID);
    mbedtls_sha1((const unsigned char *)cat, strlen(cat), sha);
    mbedtls_base64_encode((unsigned char *)want, sizeof want - 1, &wl, sha, sizeof sha);
    want[wl] = 0;

    c->rb_len = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
        if (c->stop_req || c->close_req) return false;
        uint8_t cn;
        bt_link_radio_state(NULL, NULL, &cn);
        if (cn != conn) {
            say(c, "the link closed before the upgrade was answered");
            return false;
        }
        c->rb_len += bt_link_radio_read(conn, c->rb + c->rb_len, RB_SIZE - 1 - c->rb_len);
        const uint8_t *end = find_crlf2(c->rb, c->rb_len);
        if (end) {
            const size_t head = (size_t)(end - c->rb) + 4;
            const char   saved = (char)c->rb[head - 1];
            c->rb[head - 1]    = 0;                 /* the head, a string for now */
            const char *s      = (const char *)c->rb;
            bool        ok     = !strncmp(s, "HTTP/1.", 7) && !strncmp(s + 8, " 101", 4);
            if (!ok) {
                const char *eol = strchr(s, '\r');
                say(c, "the radio answered %.*s", eol ? (int)(eol - s) : 40, s);
            } else {
                char got[40] = "";
                bool have    = false;
                for (const char *l = strchr(s, '\n'); l && !have; l = strchr(l, '\n'))
                    have = header(++l, "Sec-WebSocket-Accept", got, sizeof got);
                ok = have && !strcmp(got, want);
                if (!ok) say(c, "the radio's upgrade answer has %s", have ? "the wrong accept key" : "no accept key");
            }
            c->rb[head - 1] = (uint8_t)saved;
            memmove(c->rb, c->rb + head, c->rb_len - head);
            c->rb_len -= head;
            return ok;
        }
        if (c->rb_len >= RB_SIZE - 1) {
            say(c, "the radio's upgrade answer is too long");
            return false;
        }
        if (esp_timer_get_time() > until) {
            say(c, "the radio did not answer the upgrade within %d s", c->cfg.connect_timeout_ms / 1000);
            return false;
        }
    }
}

/* ---- frames from the link -------------------------------------------- */

enum { GO_ON, LOST, CLOSED };

/* What c->rb holds, given out as DATA: a frame's payload in pieces as it
 * comes, a control frame whole. */
static int frames(struct ws_bt *c, bool *pong)
{
    size_t i = 0;
    int    r = GO_ON;
    while (i < c->rb_len && r == GO_ON) {
        uint8_t *p = c->rb + i;
        size_t   n = c->rb_len - i;
        if (!c->in_payload) {
            if (n < 2) break;
            const uint8_t op     = p[0] & 0x0F;
            const bool    masked = p[1] & 0x80;
            uint64_t      len    = p[1] & 0x7F;
            size_t        hl     = 2;
            if (len == 126) hl += 2;
            else if (len == 127) hl += 8;
            if (masked) hl += 4;
            if (n < hl) break;
            if (len == 126) len = (uint64_t)p[2] << 8 | p[3];
            else if (len == 127) {
                len = 0;
                for (int k = 0; k < 8; k++) len = len << 8 | p[2 + k];
            }
            if ((p[0] & 0x70) || len > PAYLOAD_MAX || (op >= 0x8 && len > 125) || (op > 0x2 && op < 0x8) || op > 0xA) {
                say(c, "a frame that makes no sense (%02x %02x)", p[0], p[1]);
                return LOST;
            }
            if (masked) memcpy(c->mask, p + hl - 4, 4);
            if (op >= 0x8) {
                /* Control: ping, pong, close -- whole, at most 125 bytes. */
                if (n < hl + len) break;
                uint8_t *d = p + hl;
                if (masked)
                    for (size_t k = 0; k < len; k++) d[k] ^= c->mask[k & 3];
                i += hl + (size_t)len;
                if (op == 0x9) {
                    send_frame(c, 0xA, d, (size_t)len, pdMS_TO_TICKS(200));
                    continue;
                }
                ws_bt_event_data_t e = { .data_ptr = (const char *)d, .data_len = (int)len, .op_code = op,
                                         .payload_len = (int)len, .payload_offset = 0 };
                emit(c, WS_BT_EVENT_DATA, &e);
                if (op == 0xA) *pong = true;
                if (op == 0x8) {
                    /* The radio's close: answered, if it was not ours. */
                    if (!c->close_req) send_frame(c, 0x8, d, len >= 2 ? 2 : 0, pdMS_TO_TICKS(200));
                    say(c, "the radio closed the WebSocket%s%.*s", len > 2 ? ": " : "", len > 2 ? (int)len - 2 : 0,
                        (const char *)d + 2);
                    r = CLOSED;
                }
                continue;
            }
            c->in_payload = true;
            c->masked     = masked;
            c->op         = op;
            c->pl_len     = len;
            c->pl_off     = 0;
            i += hl;
            if (len) continue;
            /* An empty one: given out as it is. */
            p = c->rb + i;
            n = 0;
        }
        size_t take = n;
        if (take > c->pl_len - c->pl_off) take = (size_t)(c->pl_len - c->pl_off);
        if (c->masked)
            for (size_t k = 0; k < take; k++) p[k] ^= c->mask[(c->pl_off + k) & 3];
        ws_bt_event_data_t e = { .data_ptr = (const char *)p, .data_len = (int)take, .op_code = c->op,
                                 .payload_len = (int)c->pl_len, .payload_offset = (int)c->pl_off };
        emit(c, WS_BT_EVENT_DATA, &e);
        c->pl_off += take;
        i += take;
        if (c->pl_off >= c->pl_len) c->in_payload = false;
    }
    memmove(c->rb, c->rb + i, c->rb_len - i);
    c->rb_len -= i;
    return r;
}

/* Connected: frames each way until the link ends. */
static int run(struct ws_bt *c)
{
    const int64_t ping_us = (int64_t)c->cfg.ping_interval_sec * 1000000;
    const int64_t pong_us = (int64_t)c->cfg.pingpong_timeout_sec * 1000000;
    int64_t       t_ping = esp_timer_get_time(), t_pong_wait = 0, close_until = 0;
    bool          pong_wait = false, close_sent = false;
    c->in_payload = false;
    for (;;) {
        bool pong = false;
        const int r = frames(c, &pong);   /* what came with the upgrade's answer, then each pass */
        if (r != GO_ON) return r;
        if (pong) pong_wait = false;
        if (c->stop_req) return -1;
        const int64_t now = esp_timer_get_time();
        if (c->close_req && !close_sent) {
            close_sent = true;
            const uint8_t code[2] = { 0x03, 0xE8 };   /* 1000, a normal close */
            send_frame(c, 0x8, code, sizeof code, pdMS_TO_TICKS(200));
            close_until = now + (int64_t)c->close_tmo * portTICK_PERIOD_MS * 1000;
        }
        if (close_sent && now > close_until) return -1;   /* not answered: as a stop */
        if (ping_us && now - t_ping >= ping_us && !close_sent) {
            t_ping = now;
            send_frame(c, 0x9, NULL, 0, pdMS_TO_TICKS(100));
            if (!pong_wait && pong_us) {
                pong_wait   = true;
                t_pong_wait = now;
            }
        }
        if (pong_wait && now - t_pong_wait > pong_us) {
            say(c, "no pong for %d s", c->cfg.pingpong_timeout_sec);
            return LOST;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
        uint8_t conn;
        bt_link_radio_state(NULL, NULL, &conn);
        if (conn != c->conn) {
            if (close_sent) return CLOSED;            /* the radio ends a close by closing the link */
            btl_radio_state_t st;
            bt_link_radio_state(&st, NULL, NULL);
            say(c, "the link closed: %s", why_str(st.why));
            return LOST;
        }
        c->rb_len += bt_link_radio_read(c->conn, c->rb + c->rb_len, RB_SIZE - c->rb_len);
    }
}

static void ws_task(void *arg)
{
    struct ws_bt *c = arg;
    set_wake(xTaskGetCurrentTaskHandle());
    const int64_t until = esp_timer_get_time() + (int64_t)c->cfg.connect_timeout_ms * 1000;
    int end = -1;
    if (link_up(c, until)) {
        if (upgrade(c, until)) {
            c->connected = true;
            c->why[0]    = 0;
            ESP_LOGI(TAG, "connected");
            emit(c, WS_BT_EVENT_CONNECTED, NULL);
            end = run(c);
        } else if (!c->stop_req && !c->close_req) {
            end = LOST;
        }
    } else if (!c->stop_req && !c->close_req) {
        end = LOST;
    }
    c->connected = false;
    c->conn      = 0;
    bt_link_radio_close();
    uint32_t resends, gaps, dups;
    bt_link_radio_stats(&resends, &gaps, &dups);
    ESP_LOGI(TAG, "ended%s%s (the chips' link so far: %u resent, %u gaps, %u twice)", c->why[0] ? ": " : "", c->why,
             (unsigned)resends, (unsigned)gaps, (unsigned)dups);
    if (end == LOST) emit(c, WS_BT_EVENT_DISCONNECTED, NULL);
    else if (end == CLOSED) emit(c, WS_BT_EVENT_CLOSED, NULL);
    emit(c, WS_BT_EVENT_FINISH, NULL);
    set_wake(NULL);
    xSemaphoreGive(c->done);                          /* the last of c touched */
    vTaskDelete(NULL);
}

/* ---- the API --------------------------------------------------------- */

ws_bt_handle_t ws_bt_init(const ws_bt_config_t *cfg)
{
    if (!s_wmx) {
        s_wmx = xSemaphoreCreateMutex();
        if (!s_wmx) return NULL;
        bt_link_radio_set_cb(poke);
    }
    struct ws_bt *c = heap_caps_calloc(1, sizeof *c, MALLOC_CAP_SPIRAM);
    if (!c) return NULL;
    c->cfg  = *cfg;
    c->rb   = heap_caps_malloc(RB_SIZE, MALLOC_CAP_SPIRAM);
    c->done = xSemaphoreCreateBinary();
    c->tx   = xSemaphoreCreateMutex();
    if (!c->rb || !c->done || !c->tx) {
        ws_bt_destroy(c);
        return NULL;
    }
    strlcpy(c->host, cfg->host ? cfg->host : "x6100.local", sizeof c->host);
    strlcpy(c->path, cfg->path ? cfg->path : "/", sizeof c->path);
    strlcpy(c->ua, cfg->user_agent ? cfg->user_agent : "", sizeof c->ua);
    if (c->cfg.connect_timeout_ms <= 0) c->cfg.connect_timeout_ms = 20000;
    if (c->cfg.task_stack <= 0) c->cfg.task_stack = 6144;
    if (c->cfg.task_prio <= 0) c->cfg.task_prio = 5;
    return c;
}

esp_err_t ws_bt_register_events(ws_bt_handle_t c, ws_bt_handler_t cb, void *arg)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    c->cb     = cb;
    c->cb_arg = arg;
    return ESP_OK;
}

esp_err_t ws_bt_start(ws_bt_handle_t c)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    if (c->running) {
        /* Ended by itself -- the link lost -- and not stopped since: done. */
        if (xSemaphoreTake(c->done, 0) != pdTRUE) return ESP_FAIL;
        c->running = false;
    }
    c->stop_req = c->close_req = false;
    c->connected = false;
    c->conn      = 0;
    c->rb_len    = 0;
    c->task      = NULL;
    if (xTaskCreatePinnedToCore(ws_task, "ws_bt", c->cfg.task_stack, c, c->cfg.task_prio, &c->task,
                                c->cfg.task_core) != pdPASS) {
        ESP_LOGE(TAG, "no memory for its task (%u internal free)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        c->task = NULL;
        return ESP_ERR_NO_MEM;
    }
    c->running = true;
    return ESP_OK;
}

esp_err_t ws_bt_stop(ws_bt_handle_t c)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    if (!c->running) return ESP_OK;
    c->stop_req = true;
    if (on_task(c)) return ESP_OK;                   /* from its handler: as it returns */
    poke();
    if (xSemaphoreTake(c->done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "its task did not end");
        return ESP_ERR_TIMEOUT;
    }
    c->running = false;
    return ESP_OK;
}

esp_err_t ws_bt_close(ws_bt_handle_t c, TickType_t timeout)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    if (!c->running) return ESP_FAIL;
    c->close_tmo = timeout;
    c->close_req = true;
    if (on_task(c)) return ESP_OK;
    poke();
    if (xSemaphoreTake(c->done, timeout + pdMS_TO_TICKS(1000)) == pdTRUE) {
        c->running = false;
        return ESP_OK;
    }
    return ws_bt_stop(c);
}

esp_err_t ws_bt_destroy(ws_bt_handle_t c)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    if (c->running && ws_bt_stop(c) != ESP_OK) return ESP_FAIL;   /* its task still has it */
    if (c->done) vSemaphoreDelete(c->done);
    if (c->tx) vSemaphoreDelete(c->tx);
    heap_caps_free(c->rb);
    heap_caps_free(c);
    return ESP_OK;
}
#endif /* VFO_BT_RADIO && !VFO_RADIO_ICOM */
