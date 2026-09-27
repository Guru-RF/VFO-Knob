#include "audio_in.h"
#include "board_pins.h"

#include <string.h>

#include "driver/i2s_pdm.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

static const char *TAG = "mic";

/* ~340 ms of mono int16 at 24 kHz, in PSRAM. Generous, because the consumer is
 * paced by the server's chrono and a WiFi hiccup should cost latency, not a
 * hole in the transmitted audio. */
#define MIC_RING_BYTES (16 * 1024)
#define MIC_READ_SAMPLES 512

static i2s_chan_handle_t  s_rx;
static RingbufHandle_t    s_ring;
static volatile bool      s_active;
static volatile uint8_t   s_gain = 100;
static audio_in_stats_t   s_stats;
static TaskHandle_t       s_task;

bool audio_in_active(void) { return s_active; }
void audio_in_set_gain(uint8_t g) { s_gain = g > 200 ? 200 : g; }
void audio_in_stats(audio_in_stats_t *st) { if (st) *st = s_stats; }

void audio_in_set_active(bool on)
{
    if (on == s_active) return;
    s_active = on;
    if (on) {
        /* Drop anything stale so the over starts with live audio, not with
         * whatever was in the buffer when the last one ended. */
        size_t n;
        void *p;
        while ((p = xRingbufferReceiveUpTo(s_ring, &n, 0, MIC_RING_BYTES)))
            vRingbufferReturnItem(s_ring, p);
        i2s_channel_enable(s_rx);
        ESP_LOGI(TAG, "capture ON");
    } else {
        i2s_channel_disable(s_rx);
        ESP_LOGI(TAG, "capture OFF");
    }
}

static void mic_task(void *arg)
{
    (void)arg;
    static int16_t buf[MIC_READ_SAMPLES];

    for (;;) {
        if (!s_active) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }

        size_t got = 0;
        if (i2s_channel_read(s_rx, buf, sizeof buf, &got,
                             pdMS_TO_TICKS(100)) != ESP_OK || !got)
            continue;

        size_t count = got / sizeof(int16_t);
        if (s_gain != 100) {
            int32_t g = s_gain;
            for (size_t i = 0; i < count; i++) {
                int32_t v = ((int32_t)buf[i] * g) / 100;
                if (v >  32767) v =  32767;
                if (v < -32768) v = -32768;
                buf[i] = (int16_t)v;
            }
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
    ESP_RETURN_ON_FALSE(s_ring, ESP_ERR_NO_MEM, TAG, "ring");

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

bool audio_in_take(float *out, size_t out_floats)
{
    if (!out || out_floats < 2) return false;
    size_t frames = out_floats / 2;
    size_t want   = frames * sizeof(int16_t);

    size_t got = 0;
    int16_t *p = (int16_t *)xRingbufferReceiveUpTo(s_ring, &got, 0, want);
    if (!p) { s_stats.starved++; return false; }

    size_t have = got / sizeof(int16_t);
    for (size_t i = 0; i < frames; i++) {
        /* Duplicated stereo: AetherSDR detects this layout explicitly because
         * it is what WSJT-X produces, so it is the best-tested route through
         * its canonicalisation. Short reads pad with silence. */
        float v = (i < have) ? (float)p[i] / 32768.0f : 0.0f;
        out[i * 2]     = v;
        out[i * 2 + 1] = v;
    }
    vRingbufferReturnItem(s_ring, (void *)p);
    if (have < frames) s_stats.starved++;
    return true;
}
