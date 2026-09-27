#include "audio_out.h"
#include "board_pins.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

static const char *TAG = "audio";

/* ~0.5 s at 24 kHz stereo int16. Lives in PSRAM: it is streamed through, not
 * touched by DMA, so it has no business competing for internal RAM -- which is
 * the scarce resource that the display and WiFi are already fighting over. */
#define RING_BYTES (48 * 1024)
/* Wait for this much before starting playback, so a burst of jitter at the
 * start of a stream does not produce an immediate underrun. */
#define PREROLL_BYTES (8 * 1024)

static i2s_chan_handle_t s_tx;
static RingbufHandle_t   s_ring;
static volatile uint8_t  s_vol = 40;
static volatile bool     s_playing;
static audio_stats_t     s_stats;
static int16_t           s_conv[1024];   /* scratch, playback task only */

void audio_out_set_volume(uint8_t v) { s_vol = v > 100 ? 100 : v; }

void audio_out_stats(audio_stats_t *st) { if (st) *st = s_stats; }

static void play_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Pre-roll. Starting playback the instant the first bytes arrive means
         * the very next scheduling hiccup is an audible gap; waiting for a
         * cushion first costs a few tens of milliseconds once, at the start of
         * the stream, and nothing thereafter. The previous version had this
         * check with an empty body and underran about once a second. */
        size_t buffered = RING_BYTES - xRingbufferGetCurFreeSize(s_ring);
        if (!s_playing) {
            if (buffered < PREROLL_BYTES) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            s_playing = true;
        }

        size_t n = 0;
        uint8_t *p = xRingbufferReceiveUpTo(s_ring, &n, pdMS_TO_TICKS(60),
                                            sizeof s_conv);
        if (!p) {
            /* Ran dry: go back to buffering rather than stuttering along the
             * bottom of the ring. */
            if (s_playing) { s_stats.underruns++; s_playing = false; }
            continue;
        }

        /* Volume in the playback path, never on the network path: that one
         * must stay allocation-free and as short as possible, because it
         * shares a socket with PTT. */
        int16_t *smp   = (int16_t *)p;
        size_t   count = n / sizeof(int16_t);
        if (s_vol != 100) {
            int32_t g = s_vol;
            for (size_t i = 0; i < count; i++)
                smp[i] = (int16_t)(((int32_t)smp[i] * g) / 100);
        }

        size_t written = 0;
        i2s_channel_write(s_tx, smp, n, &written, portMAX_DELAY);
        vRingbufferReturnItem(s_ring, p);
    }
}

esp_err_t audio_out_init(void)
{
    /* GPIO0 HIGH hands the PCM5100A to the ESP32-S3 rather than the secondary
     * ESP32. board_init() already does this; assert it here too because a
     * silent mux in the wrong position produces perfect logs and no sound. */
    gpio_set_level(BOARD_PIN_AUDIO_MUX_SEL, 1);

    s_ring = xRingbufferCreateWithCaps(RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                       MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_ring, ESP_ERR_NO_MEM, TAG, "ring");

        /* I2S1 explicitly, not AUTO: PDM receive for the microphone is only
     * available on I2S0 on the ESP32-S3, so the DAC must not take it. */
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    /* 4 x 180 frames = 2.9 kB of DMA memory, about half the default. Internal
     * RAM is the contended resource on this board; at 24 kHz this is still
     * 30 ms of buffering, comfortably more than the scheduler needs. */
    cc.dma_desc_num  = 4;
    cc.dma_frame_num = 180;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &s_tx, NULL), TAG, "chan");

    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,     /* PCM5100A derives its own clock */
            .bclk = BOARD_PIN_I2S_BCLK,
            .ws   = BOARD_PIN_I2S_WS,
            .dout = BOARD_PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &sc), TAG, "std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable");

    s_stats.sample_rate = AUDIO_RATE_HZ;
    s_stats.channels    = AUDIO_CHANNELS;

    /* Core 1 with the rest of the "feel" work; priority above the UI so a
     * redraw cannot cause an audible gap, but below the knob. */
    xTaskCreatePinnedToCore(play_task, "audio", 3072, NULL, 11, NULL, 1);
    ESP_LOGI(TAG, "I2S up: %d Hz stereo, bclk=%d ws=%d dout=%d, %d kB PSRAM ring",
             AUDIO_RATE_HZ, BOARD_PIN_I2S_BCLK, BOARD_PIN_I2S_WS,
             BOARD_PIN_I2S_DOUT, RING_BYTES / 1024);
    return ESP_OK;
}

bool audio_out_feed(const void *frame, size_t len)
{
    if (!s_ring || len <= sizeof(tci_audio_hdr_t)) return false;

    const tci_audio_hdr_t *h = frame;
    if (h->type != TCI_AUDIO_TYPE_RX) return false;

    const uint8_t *payload = (const uint8_t *)frame + sizeof *h;
    size_t         avail   = len - sizeof *h;
    uint32_t       count   = h->length;          /* REAL samples, not frames */

    s_stats.format      = h->format;
    s_stats.sample_rate = h->sample_rate;
    s_stats.channels    = h->channels;

    /* Convert into a bounded stack buffer, in chunks. int16 passes straight
     * through; float32 is scaled and saturated. */
    static int16_t tmp[512];
    size_t done = 0;

    while (done < count) {
        size_t chunk = count - done;
        if (chunk > sizeof tmp / sizeof tmp[0]) chunk = sizeof tmp / sizeof tmp[0];

        if (h->format == TCI_AUDIO_FMT_INT16) {
            if ((done + chunk) * 2 > avail) break;
            memcpy(tmp, payload + done * 2, chunk * 2);
        } else if (h->format == TCI_AUDIO_FMT_FLOAT32) {
            if ((done + chunk) * 4 > avail) break;
            const float *f = (const float *)(payload + done * 4);
            for (size_t i = 0; i < chunk; i++) {
                float v = f[i] * 32767.0f;
                if (v >  32767.0f) v =  32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                tmp[i] = (int16_t)v;
            }
        } else {
            return false;                 /* int24/int32 not requested */
        }

        /* Never block the WebSocket task. Dropping audio is always better than
         * stalling the socket that also carries PTT. */
        if (xRingbufferSend(s_ring, tmp, chunk * 2, 0) != pdTRUE) {
            s_stats.dropped++;
            return false;
        }
        done += chunk;
    }
    s_stats.frames++;
    return true;
}
