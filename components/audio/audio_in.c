#include "audio_in.h"
#include "board_pins.h"

#include <math.h>
#include <string.h>

#include "driver/i2s_pdm.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

static const char *TAG = "mic";

/* ~340 ms of mono int16 at 24 kHz, in PSRAM. Generous, because the consumer is
 * paced by the server's chrono and a WiFi hiccup should cost latency, not a
 * hole in the transmitted audio. */
#define MIC_RING_BYTES (16 * 1024)
#define MIC_READ_SAMPLES 512
/* The PDM element is quiet: speech close to the knob peaked at 1-5% of full
 * scale, and the S3 has no PDM gain stage of its own. This fixed boost sets
 * where the operator's 0-200% sits. At 8x, 200% only reached -20 dB on
 * AetherSDR's mic Level gauge; at 32x it barely reached 0 with AetherSDR's TCI
 * TX slider at 100% (at its 50% default the radio sees the knob's full scale
 * as -6 dBFS). So 64x: 100% is where 200% was, and 200% is the most there is
 * -- peaks at the limiter, which is what MIC_KNEE is for. */
#define MIC_PREGAIN 64.0f
/* Above this the level is rounded off rather than clipped: y = k + (1-k) *
 * u/(1+u), which leaves the slope continuous at the knee and never quite
 * reaches full scale. -3 dBFS. At 200% loud syllables get here; hard clipping
 * there would be harsh on the air. */
#define MIC_KNEE (0.7f * 32767.0f)
/* One-pole DC blocker, corner ~19 Hz at 24 kHz. Boosting the element's DC
 * offset along with the speech would spend headroom on nothing. */
#define MIC_DC_POLE 0.995f
/* A headset's microphone comes already levelled -- speech near -12 dBFS from
 * the hands-free profile's coder -- so the operator's 100% leaves it as it
 * is, and 200% doubles it into the same soft knee. */
#define EXT_PREGAIN 1.0f

static i2s_chan_handle_t  s_rx;
static RingbufHandle_t    s_ring;
static volatile bool      s_active;
static volatile bool      s_priming;    /* start of an over: build a cushion */
static volatile bool      s_dc_reset;
static volatile uint8_t   s_gain = 100;
static audio_in_stats_t   s_stats;
static TaskHandle_t       s_task;
static SemaphoreHandle_t  s_mx;         /* keying, against the source changing */
static volatile bool      s_ext;        /* a headset's microphone, not the PDM one */
static volatile bool      s_pdm_on;     /* the PDM channel is enabled */
static void             (*s_ext_hook)(bool active);

bool audio_in_active(void) { return s_active; }
bool audio_in_ext(void) { return s_ext; }
void audio_in_set_ext_hook(void (*hook)(bool active)) { s_ext_hook = hook; }
void audio_in_set_gain(uint8_t g) { s_gain = g > 200 ? 200 : g; }
void audio_in_stats(audio_in_stats_t *st) { if (st) *st = s_stats; }

void audio_in_set_active(bool on)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (on == s_active) { xSemaphoreGive(s_mx); return; }
    s_active = on;
    if (on) {
        /* Drop anything stale so the over starts with live audio, not with
         * whatever was in the buffer when the last one ended. */
        size_t n;
        void *p;
        while ((p = xRingbufferReceiveUpTo(s_ring, &n, 0, MIC_RING_BYTES)))
            vRingbufferReturnItem(s_ring, p);
        s_priming  = true;
        s_dc_reset = true;
        if (!s_ext) {
            i2s_channel_enable(s_rx);
            s_pdm_on = true;
        }
        if (s_ext_hook) s_ext_hook(true);
        ESP_LOGI(TAG, "capture ON%s", s_ext ? ": the headset's microphone" : "");
    } else {
        if (s_pdm_on) {
            i2s_channel_disable(s_rx);
            s_pdm_on = false;
        }
        if (s_ext_hook) s_ext_hook(false);
        ESP_LOGI(TAG, "capture OFF");
    }
    xSemaphoreGive(s_mx);
}

void audio_in_use_ext(bool on)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (on != s_ext) {
        s_ext = on;
        ESP_LOGI(TAG, "microphone: %s", on ? "the headset's" : "the knob's");
        /* A headset come in the middle of an over takes it over. */
        if (on && s_active) {
            if (s_pdm_on) {
                i2s_channel_disable(s_rx);
                s_pdm_on = false;
            }
            if (s_ext_hook) s_ext_hook(true);
        }
        /* ...and one gone in the middle hands it back: a telephone's call
         * goes on, on the knob's own microphone, from a fresh buffer. */
        if (!on && s_active && !s_pdm_on) {
            size_t n;
            void  *p;
            while ((p = xRingbufferReceiveUpTo(s_ring, &n, 0, MIC_RING_BYTES)))
                vRingbufferReturnItem(s_ring, p);
            s_priming  = true;
            s_dc_reset = true;
            i2s_channel_enable(s_rx);
            s_pdm_on = true;
            if (s_ext_hook) s_ext_hook(false);
        }
    }
    xSemaphoreGive(s_mx);
}

void audio_in_feed_ext(const int16_t *pcm, size_t n)
{
    if (!s_active || !s_ext || !n) return;
    EXT_RAM_BSS_ATTR static int16_t buf[512];
    const float g = EXT_PREGAIN * (float)s_gain / 100.0f;
    while (n) {
        const size_t k = n > 512 ? 512 : n;
        int32_t pk = 0;
        for (size_t i = 0; i < k; i++) {
            float v = (float)pcm[i] * g;
            const float a = fabsf(v);
            if (a > MIC_KNEE) {
                const float u   = (a - MIC_KNEE) / (32767.0f - MIC_KNEE);
                const float lim = MIC_KNEE + (32767.0f - MIC_KNEE) * u / (1.0f + u);
                v = v < 0.0f ? -lim : lim;
            }
            buf[i] = (int16_t)v;
            const int32_t m = buf[i] < 0 ? -buf[i] : buf[i];
            if (m > pk) pk = m;
        }
        s_stats.peak = (float)pk / 32768.0f;
        if (xRingbufferSend(s_ring, buf, k * sizeof(int16_t), 0) != pdTRUE) s_stats.overruns++;
        else                                                               s_stats.blocks++;
        pcm += k;
        n -= k;
    }
}

static void mic_task(void *arg)
{
    (void)arg;
    static int16_t buf[MIC_READ_SAMPLES];
    float x1 = 0.0f, y1 = 0.0f;              /* DC blocker state */

    for (;;) {
        if (!s_active || !s_pdm_on) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

        size_t got = 0;
        if (i2s_channel_read(s_rx, buf, sizeof buf, &got,
                             pdMS_TO_TICKS(100)) != ESP_OK || !got)
            continue;

        size_t count = got / sizeof(int16_t);
        if (s_dc_reset) {
            /* Start from the first sample, so the offset is not a step. */
            s_dc_reset = false;
            x1 = buf[0];
            y1 = 0.0f;
        }
        const float g = MIC_PREGAIN * (float)s_gain / 100.0f;
        for (size_t i = 0; i < count; i++) {
            const float x = buf[i];
            const float y = x - x1 + MIC_DC_POLE * y1;
            x1 = x;
            y1 = y;
            float v = y * g;
            const float a = fabsf(v);
            if (a > MIC_KNEE) {
                const float u   = (a - MIC_KNEE) / (32767.0f - MIC_KNEE);
                const float lim = MIC_KNEE + (32767.0f - MIC_KNEE) * u / (1.0f + u);
                v = v < 0.0f ? -lim : lim;
            }
            buf[i] = (int16_t)v;
        }
        /* Peak, for a mic-level indicator and for catching a dead microphone
         * before an operator discovers it mid-QSO. */
        int32_t pk = 0;
        for (size_t i = 0; i < count; i++) {
            int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > pk) pk = a;
        }
        s_stats.peak = (float)pk / 32768.0f;

        if (xRingbufferSend(s_ring, buf, got, 0) != pdTRUE) s_stats.overruns++;
        else                                                s_stats.blocks++;
    }
}

esp_err_t audio_in_init(void)
{
    s_ring = xRingbufferCreateWithCaps(MIC_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                       MALLOC_CAP_SPIRAM);
    s_mx   = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ring && s_mx, ESP_ERR_NO_MEM, TAG, "ring");

    /* PDM receive exists only on I2S0 on the ESP32-S3; the DAC output is
     * pinned to I2S1 for exactly this reason. */
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num  = 4;
    cc.dma_frame_num = 240;
    cc.auto_clear    = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, NULL, &s_rx), TAG, "chan");

    i2s_pdm_rx_config_t pc = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(TX_AUDIO_RATE_HZ),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                   I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = BOARD_PIN_PDM_CLK,
            .din = BOARD_PIN_PDM_DATA,
            .invert_flags = { .clk_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_rx, &pc), TAG, "pdm");

    /* Not enabled here: the microphone stays off until PTT asks for it. */
    xTaskCreatePinnedToCore(mic_task, "mic", 3072, NULL, 11, &s_task, 1);
    ESP_LOGI(TAG, "PDM mic ready on clk=%d din=%d at %d Hz (idle until PTT)",
             BOARD_PIN_PDM_CLK, BOARD_PIN_PDM_DATA, TX_AUDIO_RATE_HZ);
    return ESP_OK;
}

bool audio_in_take(int16_t *out, size_t samples)
{
    if (!out || !samples) return false;
    const size_t want = samples * sizeof(int16_t);

    /* Hold back at the start of an over until two requests' worth is
     * buffered. The microphone delivers in DMA-sized steps and the requests
     * arrive over a network, so without a cushion the two clocks meet at the
     * wrong moment every few frames and each meeting is a hole in the audio.
     * Costs 43 ms of latency, once. */
    if (s_priming) {
        const size_t buffered = MIC_RING_BYTES - xRingbufferGetCurFreeSize(s_ring);
        if (buffered < 2 * want) return false;
        s_priming = false;
    }

    /* A byte ring hands back contiguous runs, so a read across the wrap comes
     * back short; the second read collects the rest. Each run is returned
     * before the next is taken, as a byte ring requires. */
    size_t have = 0;
    for (int pass = 0; pass < 2 && have < want; pass++) {
        size_t   got = 0;
        uint8_t *p   = xRingbufferReceiveUpTo(s_ring, &got, 0, want - have);
        if (!p) break;
        memcpy((uint8_t *)out + have, p, got);
        vRingbufferReturnItem(s_ring, p);
        have += got;
    }
    if (have < want) {
        memset((uint8_t *)out + have, 0, want - have);
        s_stats.starved++;
    }
    return have > 0;
}
