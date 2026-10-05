/* What a device is to the knob. See kind.h. */
#include "kind.h"

#include <string.h>

#include "bt_link_proto.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

static const char *TAG = "kind";

/* NVS hfp/kinds: the records, newest first. Written by one firmware and read
 * by the next, so the layout is as frozen as the protocol's. */
#define NVS_NS  "hfp"
#define NVS_KEY "kinds"
typedef struct __attribute__((packed)) {
    uint8_t bda[6];
    uint8_t kind;                       /* BTL_KIND_* */
    uint8_t why;                        /* BTL_KWHY_*, and KIND_HELD */
} rec_t;
_Static_assert(sizeof(rec_t) == 8, "frozen");
/* In why: it has held a call's audio open (kind_set_held). */
#define KIND_HELD 0x80

/* The BTC task (the stack's events), link_rx (the knob's commands) and the
 * main loop (the flush) all come here. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static rec_t        s_rec[KIND_MAX];
static int          s_n;
static bool         s_dirty;

/* ---- what a scan heard -------------------------------------------------- */

uint8_t kind_classify(uint32_t cod, uint8_t svc)
{
    const bool known = svc & BTL_SVC_KNOWN;
    const bool a2dp  = svc & BTL_SVC_A2DP;
    const bool hf    = svc & (BTL_SVC_HFP | BTL_SVC_HSP);
    if (known && a2dp && !hf) return BTL_KIND_SPEAKER;     /* music only */
    if (known && hf && !a2dp) return BTL_KIND_HEADSET;     /* calls only */
    if (esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV) {
        switch (esp_bt_gap_get_cod_minor_dev(cod)) {
        case 5:                         /* loudspeaker */
        case 7:                         /* portable audio */
        case 8:                         /* car audio */
        case 9:                         /* set-top box */
        case 10:                        /* hi-fi */
        case 15:                        /* video display and loudspeaker */
            return known && !a2dp ? BTL_KIND_HEADSET : BTL_KIND_SPEAKER;
        default:
            break;
        }
    }
    /* A wearable headset, hands-free, headphones -- with a microphone at the
     * head, as likely as not -- and anything not said: as before. */
    return BTL_KIND_HEADSET;
}

uint8_t kind_eir_services(const uint8_t *eir)
{
    uint8_t  len   = 0;
    uint8_t  known = BTL_SVC_KNOWN;
    uint8_t *u     = esp_bt_gap_resolve_eir_data((uint8_t *)eir, ESP_BT_EIR_TYPE_CMPL_16BITS_UUID, &len);
    if (!u) {
        u     = esp_bt_gap_resolve_eir_data((uint8_t *)eir, ESP_BT_EIR_TYPE_INCMPL_16BITS_UUID, &len);
        known = 0;
    }
    if (!u) return 0;
    uint8_t svc = known;
    /* The device's own sides of the profiles: never a phone's (0x111F,
     * 0x1112, 0x110A), which a phone lists. */
    for (int i = 0; i + 1 < len; i += 2) {
        switch (u[i] | u[i + 1] << 8) {
        case 0x111E: svc |= BTL_SVC_HFP;   break;   /* hands-free */
        case 0x1108:                                /* headset */
        case 0x1131: svc |= BTL_SVC_HSP;   break;   /* headset, its HS side */
        case 0x110B: svc |= BTL_SVC_A2DP;  break;   /* audio sink */
        case 0x110C:                                /* AV remote control target */
        case 0x110E: svc |= BTL_SVC_AVRCP; break;   /* AV remote control */
        default:     break;
        }
    }
    return svc;
}

const char *kind_minor_str(uint32_t cod)
{
    switch (esp_bt_gap_get_cod_major_dev(cod)) {
    case ESP_BT_COD_MAJOR_DEV_AV:
        switch (esp_bt_gap_get_cod_minor_dev(cod)) {
        case 1:  return "wearable headset";
        case 2:  return "hands-free";
        case 4:  return "microphone";
        case 5:  return "loudspeaker";
        case 6:  return "headphones";
        case 7:  return "portable audio";
        case 8:  return "car audio";
        case 9:  return "set-top box";
        case 10: return "hi-fi";
        case 11: return "VCR";
        case 12: return "video camera";
        case 13: return "camcorder";
        case 14: return "video monitor";
        case 15: return "video display and loudspeaker";
        case 16: return "video conferencing";
        case 18: return "gaming toy";
        default: return "audio/video";
        }
    case ESP_BT_COD_MAJOR_DEV_COMPUTER:   return "a computer";
    case ESP_BT_COD_MAJOR_DEV_PHONE:      return "a phone";
    case ESP_BT_COD_MAJOR_DEV_LAN_NAP:    return "a network point";
    case ESP_BT_COD_MAJOR_DEV_PERIPHERAL: return "a peripheral";
    case ESP_BT_COD_MAJOR_DEV_IMAGING:    return "imaging";
    case ESP_BT_COD_MAJOR_DEV_WEARABLE:   return "a wearable";
    case ESP_BT_COD_MAJOR_DEV_TOY:        return "a toy";
    case ESP_BT_COD_MAJOR_DEV_HEALTH:     return "health";
    default:                              return cod ? "uncategorised" : "none given";
    }
}

/* ---- the verdicts ------------------------------------------------------- */

static int find(const uint8_t *bda)
{
    for (int i = 0; i < s_n; i++)
        if (!memcmp(s_rec[i].bda, bda, 6)) return i;
    return -1;
}

void kind_load(void)
{
    rec_t  r[KIND_MAX];
    size_t n = sizeof r;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    const esp_err_t e = nvs_get_blob(h, NVS_KEY, r, &n);
    nvs_close(h);
    if (e != ESP_OK) return;
    /* Only what this firmware can read: a later one's kinds or reasons are
     * not taken for a headset's. */
    int k = 0;
    for (size_t i = 0; i < n / sizeof r[0]; i++)
        if (r[i].kind <= BTL_KIND_SPEAKER && (r[i].why & ~KIND_HELD) <= BTL_KWHY_USER) r[k++] = r[i];
    portENTER_CRITICAL(&s_mux);
    memcpy(s_rec, r, (size_t)k * sizeof r[0]);
    s_n = k;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "%d device%s known", k, k == 1 ? "" : "s");
}

bool kind_lookup(const uint8_t bda[6], uint8_t *kind, uint8_t *why)
{
    portENTER_CRITICAL(&s_mux);
    const int i = find(bda);
    if (i >= 0) {
        *kind = s_rec[i].kind;
        *why  = s_rec[i].why & ~KIND_HELD;
    }
    portEXIT_CRITICAL(&s_mux);
    return i >= 0;
}

/* Its record, i or a new one, set and moved to the front; the oldest goes
 * when the table is full. (s_mux held.) */
static void put(int i, const uint8_t *bda, uint8_t kind, uint8_t why)
{
    if (i < 0) i = s_n < KIND_MAX ? s_n++ : KIND_MAX - 1;
    memmove(&s_rec[1], &s_rec[0], (size_t)i * sizeof s_rec[0]);
    memcpy(s_rec[0].bda, bda, 6);
    s_rec[0].kind = kind;
    s_rec[0].why  = why;
    s_dirty       = true;
}

void kind_store(const uint8_t bda[6], uint8_t kind, uint8_t why)
{
    portENTER_CRITICAL(&s_mux);
    const int i = find(bda);
    /* A headset's mark stays whatever its verdict becomes. */
    if (i < 0) put(i, bda, kind, why);
    else if (s_rec[i].kind != kind || (s_rec[i].why & ~KIND_HELD) != why)
        put(i, bda, kind, why | (s_rec[i].why & KIND_HELD));
    portEXIT_CRITICAL(&s_mux);
}

void kind_set_held(const uint8_t bda[6], uint8_t kind, uint8_t why)
{
    portENTER_CRITICAL(&s_mux);
    const int i = find(bda);
    if (i < 0) put(i, bda, kind, why | KIND_HELD);
    else if (!(s_rec[i].why & KIND_HELD)) put(i, bda, s_rec[i].kind, s_rec[i].why | KIND_HELD);
    portEXIT_CRITICAL(&s_mux);
}

bool kind_held(const uint8_t bda[6])
{
    portENTER_CRITICAL(&s_mux);
    const int i = find(bda);
    const bool held = i >= 0 && (s_rec[i].why & KIND_HELD);
    portEXIT_CRITICAL(&s_mux);
    return held;
}

void kind_forget(const uint8_t bda[6])
{
    portENTER_CRITICAL(&s_mux);
    const int i = find(bda);
    if (i >= 0) {
        memmove(&s_rec[i], &s_rec[i + 1], (size_t)(s_n - i - 1) * sizeof s_rec[0]);
        s_n--;
        s_dirty = true;
    }
    portEXIT_CRITICAL(&s_mux);
}

void kind_flush(void)
{
    rec_t r[KIND_MAX];
    int   n;
    portENTER_CRITICAL(&s_mux);
    const bool dirty = s_dirty;
    s_dirty = false;
    n = s_n;
    memcpy(r, s_rec, sizeof r);
    portEXIT_CRITICAL(&s_mux);
    if (!dirty) return;
    nvs_handle_t h;
    esp_err_t    e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = n ? nvs_set_blob(h, NVS_KEY, r, (size_t)n * sizeof r[0]) : nvs_erase_key(h, NVS_KEY);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    /* Not tried again: what is in RAM still holds until the chip restarts. */
    if (e != ESP_OK) ESP_LOGW(TAG, "NVS %s/%s not written: %s", NVS_NS, NVS_KEY, esp_err_to_name(e));
}
