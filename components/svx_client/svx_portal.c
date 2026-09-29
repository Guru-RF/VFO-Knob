/* Talkgroup names from the reflector's portal. See svx_portal.h. */
#include "svx_portal.h"
#include "svx_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "nvs.h"

static const char *TAG = "svx-portal";

#define MAX_NAMES   128
#define MAX_BODY    (16 * 1024)
#define DAY_MS      (24ull * 3600 * 1000)
#define RETRY_MS    (10ull * 60 * 1000)
#define NS          "svx"

typedef struct {
    uint32_t tg;
    char     name[32];
} tg_name_t;

EXT_RAM_BSS_ATTR static tg_name_t s_names[MAX_NAMES];
static int      s_n;
static uint64_t s_t_ok, s_t_try;          /* ms; 0 = never */

static uint64_t ms_now(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static int by_tg(const void *a, const void *b)
{
    const uint32_t x = ((const tg_name_t *)a)->tg, y = ((const tg_name_t *)b)->tg;
    return x < y ? -1 : x > y;
}

/* {"8": "70cm Repeaters", ...} into s_names. Keys that are not numbers, and
 * values that are not strings, are skipped: portals differ. -1 if the text
 * is not a JSON object at all. */
static int parse(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, root) {
        if (n >= MAX_NAMES) break;
        if (!it->string || !cJSON_IsString(it) || !it->valuestring[0]) continue;
        char *end;
        const unsigned long id = strtoul(it->string, &end, 10);
        if (end == it->string || *end || id == 0 || id > 0xFFFFFFFFul) continue;
        s_names[n].tg = (uint32_t)id;
        strlcpy(s_names[n].name, it->valuestring, sizeof s_names[n].name);
        n++;
    }
    cJSON_Delete(root);
    qsort(s_names, (size_t)n, sizeof s_names[0], by_tg);
    s_n = n;
    return n;
}

/* The portal is a sibling of the reflector, not a child: reflector.x and x
 * both have theirs at portal.x. */
static const char *bare(const char *reflector)
{
    return strncasecmp(reflector, "reflector.", 10) == 0 ? reflector + 10 : reflector;
}

static void load_cache_job(void *p)
{
    const char *reflector = p;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    char host[64] = "";
    size_t n = sizeof host;
    nvs_get_str(h, "tghost", host, &n);
    size_t len = 0;
    if (strcasecmp(host, bare(reflector)) == 0 &&
        nvs_get_blob(h, "tgjson", NULL, &len) == ESP_OK && len && len <= MAX_BODY) {
        char *json = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
        if (json && nvs_get_blob(h, "tgjson", json, &len) == ESP_OK) {
            json[len] = 0;
            if (parse(json) > 0) ESP_LOGI(TAG, "%d talkgroup names from the last visit", s_n);
        }
        free(json);
    }
    nvs_close(h);
}

void svx_portal_load_cache(const char *reflector)
{
    svx_flash_safe(load_cache_job, (void *)reflector);
}

bool svx_portal_due(void)
{
    const uint64_t t = ms_now();
    if (!s_t_try) return true;
    if (t - s_t_try < RETRY_MS) return false;
    return !s_t_ok || t - s_t_ok >= DAY_MS;
}

typedef struct {
    const char *host, *json;
    size_t      len;
} store_job_t;

static void store_job(void *p)
{
    const store_job_t *j = p;
    const char *host = j->host, *json = j->json;
    const size_t len = j->len;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t old = 0;
    char *prev = NULL;
    if (nvs_get_blob(h, "tgjson", NULL, &old) == ESP_OK && old == len &&
        (prev = heap_caps_malloc(old, MALLOC_CAP_SPIRAM)) &&
        nvs_get_blob(h, "tgjson", prev, &old) == ESP_OK && memcmp(prev, json, len) == 0) {
        free(prev);                              /* unchanged: spare the flash */
        nvs_close(h);
        return;
    }
    free(prev);
    nvs_set_str(h, "tghost", host);
    nvs_set_blob(h, "tgjson", json, len);
    nvs_commit(h);
    nvs_close(h);
}

static void store(const char *host, const char *json, size_t len)
{
    store_job_t j = { .host = host, .json = json, .len = len };
    svx_flash_safe(store_job, &j);
}

bool svx_portal_fetch(const char *reflector)
{
    s_t_try = ms_now();
    const char *host = bare(reflector);
    struct in_addr a;
    if (!host[0] || inet_aton(host, &a)) {       /* an address has no portal */
        s_t_ok = s_t_try;
        return false;
    }
    char url[128];
    snprintf(url, sizeof url, "https://portal.%s/talkgroups.json", host);
    esp_http_client_config_t hc = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 5000,
        .buffer_size       = 1536,
    };
    esp_http_client_handle_t h = esp_http_client_init(&hc);
    if (!h) return false;

    char *body = NULL;
    int   len = 0, status = 0;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        if (status == 200 && (body = heap_caps_malloc(MAX_BODY + 1, MALLOC_CAP_SPIRAM))) {
            while (len < MAX_BODY) {
                int k = esp_http_client_read(h, body + len, MAX_BODY - len);
                if (k <= 0) break;
                len += k;
            }
            body[len] = 0;
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);

    bool changed = false;
    if (!body) {
        if (status) ESP_LOGW(TAG, "%s: HTTP %d", url, status);
        else        ESP_LOGW(TAG, "%s: no answer", url);
    } else if (parse(body) < 0) {
        ESP_LOGW(TAG, "%s is not a JSON object", url);
    } else {
        ESP_LOGI(TAG, "%d talkgroup names from %s", s_n, url);
        s_t_ok = ms_now();
        store(host, body, (size_t)len);
        changed = true;
    }
    free(body);
    return changed;
}

const char *svx_portal_name(uint32_t tg)
{
    int lo = 0, hi = s_n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (s_names[mid].tg == tg) return s_names[mid].name;
        if (s_names[mid].tg < tg) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

int svx_portal_ids(uint32_t *ids, int max)
{
    int n = 0;
    for (int i = 0; i < s_n && n < max; i++) ids[n++] = s_names[i].tg;
    return n;
}
