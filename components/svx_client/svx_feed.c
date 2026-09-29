/* Talker locations from an enhanced reflector's feed. See svx_feed.h; the
 * URL and the message shapes are SVXConnect-Omarchy's (src/net/reflectorfeed). */
#include "svx_feed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "svx-feed";

#define MSG_MAX   4096          /* a talk_start is under 500 bytes */
#define TALKERS   32

static esp_websocket_client_handle_t s_ws;
static char  *s_msg;            /* one message being reassembled, PSRAM */
static size_t s_len;
static bool   s_skip;           /* the rest of a message too big to want */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    char call[16];
    char where[32];
} s_seen[TALKERS];              /* newest overwrites oldest */
static int s_next;

static void remember(const char *call, const char *where)
{
    taskENTER_CRITICAL(&s_mux);
    int slot = -1;
    for (int i = 0; i < TALKERS; i++)
        if (strcasecmp(s_seen[i].call, call) == 0) { slot = i; break; }
    if (slot < 0) { slot = s_next; s_next = (s_next + 1) % TALKERS; }
    strlcpy(s_seen[slot].call, call, sizeof s_seen[slot].call);
    strlcpy(s_seen[slot].where, where, sizeof s_seen[slot].where);
    taskEXIT_CRITICAL(&s_mux);
}

bool svx_feed_where(const char *callsign, char *out, size_t cap)
{
    bool found = false;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < TALKERS && !found; i++)
        if (s_seen[i].call[0] && strcasecmp(s_seen[i].call, callsign) == 0) {
            strlcpy(out, s_seen[i].where, cap);
            found = true;
        }
    taskEXIT_CRITICAL(&s_mux);
    return found;
}

static const char *str_of(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : NULL;
}

static void on_message(const char *json, size_t len)
{
    /* Most of the traffic is node_upsert: look before parsing. */
    if (!memmem(json, len, "talk_start", 10)) return;
    cJSON *root = cJSON_ParseWithLength(json, len);
    const char *type = str_of(root, "type");
    if (type && strcmp(type, "talk_start") == 0) {
        const cJSON *ses  = cJSON_GetObjectItemCaseSensitive(root, "session");
        const cJSON *node = cJSON_GetObjectItemCaseSensitive(ses, "node");
        const cJSON *qth  = cJSON_GetObjectItemCaseSensitive(node, "qth");
        const char *call  = str_of(ses, "callsign");
        /* The node's own words for where it is; else its locator. */
        const char *where = str_of(node, "nodeLocation");
        if (!where) where = str_of(qth, "loc");
        if (call && where) remember(call, where);
    }
    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    const esp_websocket_event_data_t *e = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "following the reflector's feed");
        s_len = 0;
        s_skip = false;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "feed closed; trying again in a minute");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (e->op_code != 0x01 && e->op_code != 0x00) break;     /* text only */
        if (e->payload_offset == 0) {
            s_len  = 0;
            s_skip = e->payload_len > MSG_MAX || !s_msg;
        }
        if (s_skip) break;               /* the snapshot: unread */
        if (s_len + (size_t)e->data_len > MSG_MAX) { s_skip = true; break; }
        memcpy(s_msg + s_len, e->data_ptr, (size_t)e->data_len);
        s_len += (size_t)e->data_len;
        if (e->payload_offset + e->data_len >= e->payload_len) {
            on_message(s_msg, s_len);
            s_len = 0;
        }
        break;
    default:
        break;
    }
}

void svx_feed_stop(void)
{
    if (!s_ws) return;
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    ESP_LOGI(TAG, "feed off");
}

void svx_feed_start(const char *reflector)
{
    if (s_ws || !reflector[0]) return;
    /* reflector.<domain>, whether the domain was given with it or without. */
    char uri[96];
    if (strncasecmp(reflector, "reflector.", 10) == 0)
        snprintf(uri, sizeof uri, "wss://%s/", reflector);
    else
        snprintf(uri, sizeof uri, "wss://reflector.%s/", reflector);

    s_msg = heap_caps_malloc(MSG_MAX, MALLOC_CAP_SPIRAM);
    esp_websocket_client_config_t cfg = {
        .uri                  = uri,
        .crt_bundle_attach    = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 60000,
        .network_timeout_ms   = 3000,     /* how long stopping it can take */
        .ping_interval_sec    = 30,
        .pingpong_timeout_sec = 90,
        /* Below the reflector task, beside lwIP on core 0. */
        .task_prio            = 4,
        .task_stack           = 6144,
        .task_core_id_set     = true,
        .task_core_id         = 0,
        .buffer_size          = 2048,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGW(TAG, "no memory for the feed");
        return;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        ESP_LOGW(TAG, "feed did not start");
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        return;
    }
    ESP_LOGI(TAG, "looking for a feed at %s", uri);
}
