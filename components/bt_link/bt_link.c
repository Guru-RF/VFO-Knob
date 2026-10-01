/* The knob's end of the link to its second chip. See bt_link.h. */
#include "bt_link.h"

#include <stdio.h>
#include <string.h>

#include "audio_in.h"
#include "audio_out.h"
#include "board_pins.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "bt";

#define LINK_UART   UART_NUM_1
/* The driver's rings: the heap puts them in PSRAM (the driver asks for
 * MALLOC_CAP_DEFAULT; init logs what it took of internal RAM, ~130 bytes).
 * Out, 2 kB is 10 ms of the wire, which empties it four times as fast as the
 * audio fills it; in, 4 kB holds 80 ms of the headset's microphone while this
 * task waits behind the WiFi on its core. */
#define RX_RING     4096
#define TX_RING     2048
#define FOUND_MAX   16

static SemaphoreHandle_t s_tx;
static btl_rx_t         *s_rx;
static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;
static bt_link_status_t  s_st;
static int64_t           s_heard_us, s_ping_us;
EXT_RAM_BSS_ATTR static btl_found_t s_found[FOUND_MAX];
static int               s_nfound;
static volatile uint32_t s_ptt_taken;
static volatile bool     s_hs_conn;     /* a headset is connected: its microphone is the only one */
static volatile bool     s_hs_audio;    /* ...and its audio is open */
static volatile bool     s_mic;         /* keyed: the companion sends the microphone */
static volatile bool     s_boom;        /* the boom arm is the PTT */

static bool send(uint8_t type, const void *p, uint16_t n)
{
    static uint8_t *f;
    if (!s_tx) return false;                    /* not started: the setup firmware */
    if (!f) f = heap_caps_malloc(BTL_MAX_PAYLOAD + 7, MALLOC_CAP_SPIRAM);
    if (!f || n > BTL_MAX_PAYLOAD) return false;
    xSemaphoreTake(s_tx, portMAX_DELAY);
    const size_t len = btl_frame(f, type, p, n);
    const int w = uart_write_bytes(LINK_UART, f, len);
    xSemaphoreGive(s_tx);
    return w == (int)len;
}

static void hello(bool ask)
{
    static const char v[] = "knob";
    uint8_t p[2 + sizeof v];
    p[0] = BTL_PROTO;
    p[1] = ask ? BTL_HELLO_ASK : 0;
    memcpy(p + 2, v, sizeof v - 1);
    send(BTL_HELLO, p, sizeof p - 1);
}

/* What the knob wants of the companion: after each hello, as either side
 * may have started afresh. */
static void push_config(void)
{
    uint8_t a[9];
    const uint32_t dn = AUDIO_RATE_HZ, up = TX_AUDIO_RATE_HZ;
    a[0] = 1;                                   /* the headset's audio whenever it is on */
    memcpy(a + 1, &dn, 4);
    memcpy(a + 5, &up, 4);
    send(BTL_CMD_AUDIO, a, sizeof a);
    const uint8_t m = s_mic;
    send(BTL_CMD_MIC, &m, 1);
    send(BTL_CMD_STATE, NULL, 0);
}

/* The playback task's: everything the jack plays, to the headset's ear, in
 * frames of 10 ms -- a frame lost on the wire is then 10 ms, not 21. */
#define DN_FRAME 240
static void tap(const int16_t *stereo, size_t frames)
{
    if (!s_hs_audio) return;
    EXT_RAM_BSS_ATTR static int16_t mono[DN_FRAME];
    while (frames) {
        const size_t n = frames > DN_FRAME ? DN_FRAME : frames;
        for (size_t i = 0; i < n; i++)
            mono[i] = (int16_t)(((int32_t)stereo[2 * i] + stereo[2 * i + 1]) / 2);
        send(BTL_AUDIO_DN, mono, (uint16_t)(n * 2));
        stereo += 2 * n;
        frames -= n;
    }
}

/* audio_in's: keying started or stopped -- the headset's microphone only then. */
static void mic_hook(bool active)
{
    s_mic = active;
    const uint8_t m = active;
    send(BTL_CMD_MIC, &m, 1);
}

static const char *link_name(uint8_t l)
{
    return l == BTL_LINK_CONNECTED ? "connected" : l == BTL_LINK_CONNECTING ? "connecting" : "idle";
}

/* While a headset is connected the knob's own microphone is off: keying
 * takes the headset's, or -- its audio not open yet -- silence, never the
 * knob's across the room. */
static void headset(bool conn, bool audio)
{
    s_hs_audio = conn && audio;
    if (conn == s_hs_conn) return;
    s_hs_conn = conn;
    audio_in_use_ext(conn);
}

static void on_state(const uint8_t *p, uint16_t n)
{
    btl_state_t s;
    memset(&s, 0, sizeof s);
    memcpy(&s, p, n < sizeof s ? n : sizeof s);
    s.name[sizeof s.name - 1] = 0;
    btl_state_t was;
    taskENTER_CRITICAL(&s_lock);
    was = s_st.hs;
    s_st.hs = s;
    taskEXIT_CRITICAL(&s_lock);
    if (s.link != was.link || s.audio != was.audio)
        ESP_LOGI(TAG, "headset %s: %s%s", s.name[0] ? s.name : "-", link_name(s.link),
                 s.audio == BTL_AUDIO_MSBC_16K ? ", audio mSBC 16 kHz"
                 : s.audio == BTL_AUDIO_CVSD_8K ? ", audio CVSD 8 kHz" : "");
    headset(s.link == BTL_LINK_CONNECTED, s.audio != BTL_AUDIO_NONE);
    if (s.link == BTL_LINK_CONNECTED && (s.mic == 0) != (was.link == BTL_LINK_CONNECTED && was.mic == 0))
        ESP_LOGI(TAG, "headset microphone %s", s.mic == 0 ? "muted" : "live");
}

static void on_found(const uint8_t *p, uint16_t n)
{
    btl_found_t f;
    memset(&f, 0, sizeof f);
    memcpy(&f, p, n < sizeof f ? n : sizeof f);
    f.name[sizeof f.name - 1] = 0;
    taskENTER_CRITICAL(&s_lock);
    int i = 0;
    while (i < s_nfound && memcmp(s_found[i].bda, f.bda, 6)) i++;
    if (i == s_nfound && s_nfound < FOUND_MAX) s_nfound++;
    if (i < s_nfound) {
        if (!f.name[0]) memcpy(f.name, s_found[i].name, sizeof f.name);   /* a name once heard stays */
        s_found[i] = f;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void on_frame(uint8_t type, const uint8_t *p, uint16_t n)
{
    const int64_t now = esp_timer_get_time();
    s_heard_us = now;
    switch (type) {
    case BTL_HELLO: {
        char v[33];
        const int vn = n > 2 ? (n - 2 < 32 ? n - 2 : 32) : 0;
        memcpy(v, p + 2, (size_t)vn);
        v[vn] = 0;
        const bool ask = n >= 2 && (p[1] & BTL_HELLO_ASK);
        taskENTER_CRITICAL(&s_lock);
        const bool first = !s_st.companion;
        s_st.companion = true;
        strlcpy(s_st.version, v, sizeof s_st.version);
        taskEXIT_CRITICAL(&s_lock);
        if (first || ask) ESP_LOGI(TAG, "the companion %s: protocol %u, %s", ask ? "started" : "answers",
                                   n ? p[0] : 0, v);
        if (ask) hello(false);
        push_config();
        break;
    }
    case BTL_PONG:
        if (n >= 4) {
            taskENTER_CRITICAL(&s_lock);
            s_st.pongs++;
            s_st.rtt_us = (uint32_t)(now - s_ping_us);
            taskEXIT_CRITICAL(&s_lock);
        }
        break;
    case BTL_EVT_STATE:
        on_state(p, n);
        break;
    case BTL_EVT_FOUND:
        on_found(p, n);
        break;
    case BTL_EVT_SCAN_DONE:
        ESP_LOGI(TAG, "scan done: %d found", s_nfound);
        break;
    case BTL_EVT_BUTTON:
        if (n && (p[0] == BTL_BTN_HANGUP || p[0] == BTL_BTN_ANSWER)) {
            taskENTER_CRITICAL(&s_lock);
            s_st.presses++;
            taskEXIT_CRITICAL(&s_lock);
        }
        break;
    case BTL_EVT_VOLUME:
        /* The headset's own gains, as it changes them. A headset that mutes
         * its microphone says so as a gain of 0 -- the Jabras do. */
        if (n >= 2) {
            taskENTER_CRITICAL(&s_lock);
            const bool was_muted = s_st.hs.mic == 0;
            s_st.hs.spk = p[0];
            s_st.hs.mic = p[1];
            taskEXIT_CRITICAL(&s_lock);
            if ((p[1] == 0) != was_muted)
                ESP_LOGI(TAG, "headset microphone %s", p[1] == 0 ? "muted" : "live");
            ESP_LOGD(TAG, "headset volume %u, microphone %u", p[0], p[1]);
        }
        break;
    case BTL_EVT_LOG:
        ESP_LOGI(TAG, "companion: %.*s", (int)n, (const char *)p);
        break;
    case BTL_AUDIO_UP:
        if (n >= 2) {
            EXT_RAM_BSS_ATTR static int16_t pcm[BTL_MAX_PAYLOAD / 2];
            memcpy(pcm, p, n & ~1u);            /* the frame's payload is not aligned */
            audio_in_feed_ext(pcm, n / 2);
            s_st.up_frames++;
        }
        break;
    default:
        ESP_LOGD(TAG, "frame 0x%02x, %u bytes", type, n);
        break;
    }
}

static void link_task(void *arg)
{
    (void)arg;
    uint8_t *chunk = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    int64_t t_hello = 0, t_ping = 0;
    uint32_t seq = 0;
    for (;;) {
        const int n = uart_read_bytes(LINK_UART, chunk, 512, pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++)
            if (btl_rx_put(s_rx, chunk[i])) on_frame(s_rx->type, s_rx->buf, s_rx->len);
        const int64_t now = esp_timer_get_time();
        /* Hello until it answers -- it may start before the knob or after. */
        if (!s_st.companion && now - t_hello > 1000000) {
            t_hello = now;
            hello(true);
        }
        /* And a ping every five seconds, for the link's health -- with the
         * headset's state, in case a change of it was lost on the wire. */
        if (s_st.companion && now - t_ping > 5000000) {
            t_ping = now;
            s_ping_us = now;
            seq++;
            send(BTL_PING, &seq, sizeof seq);
            send(BTL_CMD_STATE, NULL, 0);
            taskENTER_CRITICAL(&s_lock);
            s_st.pings++;
            s_st.bad = s_rx->bad;
            taskEXIT_CRITICAL(&s_lock);
        }
        if (s_st.companion && now - s_heard_us > 15000000) {
            ESP_LOGW(TAG, "the companion went quiet");
            taskENTER_CRITICAL(&s_lock);
            s_st.companion = false;
            memset(&s_st.hs, 0, sizeof s_st.hs);
            taskEXIT_CRITICAL(&s_lock);
            headset(false, false);
        }
    }
}

void bt_link_status(bt_link_status_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_lock);
    *out = s_st;
    taskEXIT_CRITICAL(&s_lock);
}

bool bt_link_headset_audio(void) { return s_hs_audio; }

bool bt_link_headset_connected(void)
{
    taskENTER_CRITICAL(&s_lock);
    const bool c = s_st.companion && s_st.hs.link == BTL_LINK_CONNECTED;
    taskEXIT_CRITICAL(&s_lock);
    return c;
}

bool bt_link_boom_ptt(void) { return s_boom; }

/* From the web server's task: its stack is internal, so NVS is safe here. */
void bt_link_set_boom_ptt(bool on)
{
    s_boom = on;
    nvs_handle_t h;
    if (nvs_open("btlink", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "boom", on);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "the boom arm %s the PTT", on ? "is" : "is no longer");
}

bool bt_link_headset_muted(void)
{
    taskENTER_CRITICAL(&s_lock);
    const bool m = s_st.companion && s_st.hs.link == BTL_LINK_CONNECTED && s_st.hs.mic == 0;
    taskEXIT_CRITICAL(&s_lock);
    return m;
}

int bt_link_found(btl_found_t *out, int max)
{
    taskENTER_CRITICAL(&s_lock);
    const int n = s_nfound < max ? s_nfound : max;
    memcpy(out, s_found, (size_t)n * sizeof *out);
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

void bt_link_scan(uint8_t seconds)
{
    taskENTER_CRITICAL(&s_lock);
    s_nfound = 0;
    taskEXIT_CRITICAL(&s_lock);
    send(BTL_CMD_SCAN, &seconds, 1);
}

void bt_link_connect(const uint8_t bda[6])    { send(BTL_CMD_CONNECT, bda, 6); }
void bt_link_disconnect(void)                 { send(BTL_CMD_DISCONNECT, NULL, 0); }
void bt_link_forget(const uint8_t bda[6])     { send(BTL_CMD_FORGET, bda, 6); }

bool bt_link_take_ptt(void)
{
    const uint32_t p = s_st.presses;
    if (p == s_ptt_taken) return false;
    s_ptt_taken = p;
    return true;
}

esp_err_t bt_link_init(void)
{
    nvs_handle_t h;
    uint8_t boom = 0;
    if (nvs_open("btlink", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "boom", &boom);
        nvs_close(h);
    }
    s_boom = boom;
    s_tx = xSemaphoreCreateMutex();
    s_rx = heap_caps_calloc(1, sizeof *s_rx, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_tx && s_rx, ESP_ERR_NO_MEM, TAG, "memory");
    const uart_config_t c = {
        .baud_rate  = BTL_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_ERROR(uart_driver_install(LINK_UART, RX_RING, TX_RING, 0, NULL, 0), TAG, "uart");
    const size_t driver = before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_ERROR(uart_param_config(LINK_UART, &c), TAG, "uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(LINK_UART, BOARD_PIN_COMPANION_TX, BOARD_PIN_COMPANION_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart pins");
    /* The receive FIFO's 128 bytes last 0.64 ms at 2 Mbit/s: the driver's
     * default asks for them at 120, 40 us to spare. At 32 there are 0.5 ms --
     * the headset's microphone comes this way, 50 kB/s of it. */
    ESP_RETURN_ON_ERROR(uart_set_rx_full_threshold(LINK_UART, 32), TAG, "uart threshold");
    ESP_RETURN_ON_ERROR(uart_set_rx_timeout(LINK_UART, 4), TAG, "uart timeout");
    audio_out_set_tap(tap);
    audio_in_set_ext_hook(mic_hook);
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCoreWithCaps(link_task, "btlink", 4096, NULL, 5, NULL, 0,
                                                        MALLOC_CAP_SPIRAM) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "link to the second chip: UART%d, %d baud; its driver takes %u bytes of internal RAM",
             LINK_UART, BTL_BAUD, (unsigned)driver);
    return ESP_OK;
}
