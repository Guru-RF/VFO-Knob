#include "audio_out.h"
#include "board_pins.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

static const char *TAG = "audio";

/* ~0.5 s at 24 kHz stereo int16. Lives in PSRAM: it is streamed through, not
 * touched by DMA, so it has no business competing for internal RAM -- which is
 * the scarce resource that the display and WiFi are already fighting over. */
#define RING_BYTES (48 * 1024)
/* Wait for this much before starting playback, so a burst of jitter at the
 * start of a stream does not produce an immediate underrun. A FlexRadio is
 * often a routed hop or a VPN away, and its 10 ms packets come in bursts:
 * 85 ms ran dry every few seconds there, so that firmware keeps 170. */
#if VFO_RADIO_MULTIFLEX
#define PREROLL_BYTES (16 * 1024)
#else
#define PREROLL_BYTES (8 * 1024)
#endif

static i2s_chan_handle_t s_tx;
static RingbufHandle_t   s_ring;

/* A second receiver: a web SDR's audio (components/sdr_rx), mono at the same
 * 24 kHz, in a ring of its own with a longer pre-roll -- a web SDR is further
 * away and burstier than the radio. While one plays, the radio goes to the
 * left ear and the SDR to the right, each levelled to the same loudness, and
 * the balance fades between them. Without one, playback is as it always was. */
#define SDR_RING_BYTES (40 * 1024)          /* ~0.8 s of mono int16 */
#define SDR_PREROLL    (12 * 1024)          /* 250 ms */
#define MIX_FRAMES     240                  /* 10 ms blocks */
static RingbufHandle_t   s_sdr_ring;
static volatile bool     s_sdr_on, s_sdr_flush, s_sdr_mute;
static volatile int8_t   s_balance;         /* -100 radio .. 0 split .. +100 SDR */
static bool              s_sdr_playing;
typedef struct { float gain; } leveler_t;
static leveler_t         s_lv_radio = { 1.0f }, s_lv_sdr = { 1.0f };
static volatile uint8_t  s_vol = 40;
static volatile bool     s_playing;
static volatile bool     s_kick, s_flush;
static audio_stats_t     s_stats;
static int16_t           s_conv[1024];   /* scratch, playback task only */

void audio_out_set_volume(uint8_t v) { s_vol = v > 100 ? 100 : v; }

void audio_out_stats(audio_stats_t *st) { if (st) *st = s_stats; }

void audio_out_kick(void)  { s_kick = true; }
void audio_out_flush(void) { s_flush = true; }

size_t audio_out_queued(void)
{
    return s_ring ? (RING_BYTES - xRingbufferGetCurFreeSize(s_ring)) / 4 : 0;
}

void audio_out_set_balance(int8_t b) { s_balance = b < -100 ? -100 : (b > 100 ? 100 : b); }

void audio_out_sdr(bool on)
{
    if (on == s_sdr_on) return;
    s_sdr_on = on;
    /* What is left belongs to the session just ended. The playback task drops
     * it: a byte ring has one reader at a time. */
    if (!on) s_sdr_flush = true;
}

void audio_out_sdr_mute(bool mute) { s_sdr_mute = mute; }

bool audio_out_feed_sdr(const int16_t *pcm, size_t n)
{
    if (!s_sdr_ring || !s_sdr_on || !pcm || !n) return false;
    return xRingbufferSend(s_sdr_ring, pcm, n * 2, 0) == pdTRUE;
}

static void drain(RingbufHandle_t r)
{
    size_t n;
    void  *p;
    while ((p = xRingbufferReceiveUpTo(r, &n, 0, SIZE_MAX)))
        vRingbufferReturnItem(r, p);
}

/* Up to `bytes` out of a byte ring, across its wrap. */
static size_t ring_take(RingbufHandle_t r, uint8_t *dst, size_t bytes)
{
    size_t got = 0;
    while (got < bytes) {
        size_t n = 0;
        uint8_t *p = xRingbufferReceiveUpTo(r, &n, 0, bytes - got);
        if (!p) break;
        memcpy(dst + got, p, n);
        vRingbufferReturnItem(r, p);
        got += n;
    }
    return got;
}

/* Each source towards the same loudness: quick to come down, slow to come
 * back up, holding through silence, within -14 to +18 dB. */
static void level(leveler_t *lv, float *x, int n)
{
    float e = 0;
    for (int i = 0; i < n; i++) e += x[i] * x[i];
    const float rms = sqrtf(e / n);
    if (rms > 60.0f) {
        float want = 3000.0f / rms;
        if (want < 0.2f) want = 0.2f;
        if (want > 8.0f) want = 8.0f;
        lv->gain += (want - lv->gain) * (want < lv->gain ? 0.3f : 0.01f);
    }
    for (int i = 0; i < n; i++) x[i] *= lv->gain;
}

static int16_t sat16(float v) { return v > 32767.0f ? 32767 : (v < -32768.0f ? -32768 : (int16_t)v); }

/* One 10 ms block of the two sources mixed, or nothing when neither plays. */
static void mix_block(void)
{
    EXT_RAM_BSS_ATTR static int16_t radio[MIX_FRAMES * 2], sdr[MIX_FRAMES], out[MIX_FRAMES * 2];
    EXT_RAM_BSS_ATTR static float   fr[MIX_FRAMES], fs[MIX_FRAMES];

    const size_t rbuf = RING_BYTES - xRingbufferGetCurFreeSize(s_ring);
    const size_t sbuf = SDR_RING_BYTES - xRingbufferGetCurFreeSize(s_sdr_ring);
    if (!s_playing && rbuf >= PREROLL_BYTES) s_playing = true;
    if (!s_sdr_playing && sbuf >= SDR_PREROLL) s_sdr_playing = true;
    if (!s_playing && !s_sdr_playing) { vTaskDelay(pdMS_TO_TICKS(5)); return; }

    size_t got = s_playing ? ring_take(s_ring, (uint8_t *)radio, sizeof radio) : 0;
    if (s_playing && got < sizeof radio) { s_stats.underruns++; s_playing = false; }
    memset((uint8_t *)radio + got, 0, sizeof radio - got);
    got = s_sdr_playing ? ring_take(s_sdr_ring, (uint8_t *)sdr, sizeof sdr) : 0;
    if (s_sdr_playing && got < sizeof sdr) s_sdr_playing = false;
    memset((uint8_t *)sdr + got, 0, sizeof sdr - got);

    for (int i = 0; i < MIX_FRAMES; i++) {
        fr[i] = 0.5f * ((float)radio[2 * i] + (float)radio[2 * i + 1]);
        fs[i] = (float)sdr[i];
    }
    level(&s_lv_radio, fr, MIX_FRAMES);
    /* Not while transmitting: the SDR hears the over a second late, and from
     * a speaker the microphone would hear it too. Its ring runs on meanwhile,
     * so it comes back in time. */
    if (s_sdr_mute) memset(fs, 0, sizeof fs);
    else            level(&s_lv_sdr, fs, MIX_FRAMES);

    /* 0: radio left, SDR right. Towards -100 the radio takes the right ear
     * too, towards +100 the SDR the left: at either end one source alone. */
    const float b  = s_balance / 100.0f;
    const float lr = b > 0 ? 1.0f - b : 1.0f, ls = b > 0 ? b : 0.0f;
    const float rs = b < 0 ? 1.0f + b : 1.0f, rr = b < 0 ? -b : 0.0f;
    const float g  = s_vol / 100.0f;
    for (int i = 0; i < MIX_FRAMES; i++) {
        out[2 * i]     = sat16(g * (lr * fr[i] + ls * fs[i]));
        out[2 * i + 1] = sat16(g * (rs * fs[i] + rr * fr[i]));
    }
    size_t written = 0;
    i2s_channel_write(s_tx, out, sizeof out, &written, portMAX_DELAY);
}

static void play_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_sdr_flush) {
            s_sdr_flush   = false;
            drain(s_sdr_ring);
            s_sdr_playing = false;
            s_lv_radio.gain = s_lv_sdr.gain = 1.0f;
        }
        if (s_sdr_on) { mix_block(); continue; }
        /* Pre-roll. Starting playback the instant the first bytes arrive means
         * the very next scheduling hiccup is an audible gap; waiting for a
         * cushion first costs a few tens of milliseconds once, at the start of
         * the stream, and nothing thereafter. The previous version had this
         * check with an empty body and underran about once a second. */
        if (s_flush) {
            s_flush = false;
            size_t n;
            void  *p;
            while ((p = xRingbufferReceiveUpTo(s_ring, &n, 0, RING_BYTES)))
                vRingbufferReturnItem(s_ring, p);
            s_playing = false;
        }
        size_t buffered = RING_BYTES - xRingbufferGetCurFreeSize(s_ring);
        if (!s_playing) {
            /* A kick starts whatever is there; one that finds nothing is
             * spent, so it cannot cut the next stream's pre-roll short. */
            if (s_kick) { s_kick = false; s_playing = buffered > 0; }
            if (!s_playing) {
                if (buffered < PREROLL_BYTES) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
                s_playing = true;
            }
        } else {
            s_kick = false;
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
    s_sdr_ring = xRingbufferCreateWithCaps(SDR_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                           MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_sdr_ring, ESP_ERR_NO_MEM, TAG, "sdr ring");

        /* I2S1 explicitly, not AUTO: PDM receive for the microphone is only
     * available on I2S0 on the ESP32-S3, so the DAC must not take it. */
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    /* 4 x 180 frames = 2.9 kB of DMA memory, about half the default. Internal
     * RAM is the contended resource on this board; at 24 kHz this is still
     * 30 ms of buffering, comfortably more than the scheduler needs. */
    cc.dma_desc_num  = 4;
    cc.dma_frame_num = 180;
    /* Zero each DMA buffer once it has been played. Without this the DMA
     * keeps cycling through whatever it last held whenever the stream stops
     * -- a network stall, or the radio going quiet -- and the knob loops the
     * last 30 ms of audio until data returns. */
    cc.auto_clear    = true;
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

bool audio_out_feed_pcm16(const int16_t *pcm, size_t frames, uint8_t channels)
{
    if (!s_ring || !pcm || !frames || (channels != 1 && channels != 2)) return false;
    s_stats.format      = TCI_AUDIO_FMT_INT16;
    s_stats.sample_rate = AUDIO_RATE_HZ;
    s_stats.channels    = channels;

    static int16_t tmp[512];                 /* the one network task feeds it */
    size_t done = 0;
    while (done < frames) {
        size_t chunk = frames - done;
        if (chunk > sizeof tmp / sizeof tmp[0] / 2) chunk = sizeof tmp / sizeof tmp[0] / 2;
        if (channels == 1) {
            for (size_t i = 0; i < chunk; i++)
                tmp[2 * i] = tmp[2 * i + 1] = pcm[done + i];
        } else {
            memcpy(tmp, pcm + 2 * done, chunk * 4);
        }
        if (xRingbufferSend(s_ring, tmp, chunk * 4, 0) != pdTRUE) {
            s_stats.dropped++;
            return false;
        }
        done += chunk;
    }
    s_stats.frames++;
    return true;
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
