/* The phone firmware's radio: a telephone, one SIP account, over WiFi.
 *
 * Its own small SIP user agent (sip.c) and G.711 (g711.c), on SVXConnect's
 * face. radio.h maps onto a telephone like this:
 *
 *   the dial        steps through the favourites (the talkgroups' place)
 *   the frequency   is the chosen favourite's name, or who the call is with
 *   the arc         is the received audio's level, as on SVXConnect
 *   the gain's      mute
 *   PTT             dials the chosen favourite, answers a call that rings,
 *                   and hangs up one that is up -- radio_ptt_toggle(); unkey
 *                   only ever hangs up (a headset's button)
 *
 * Audio is the knob's own at 16 kHz -- speaker and microphone, or a Bluetooth
 * headset through the second chip -- and 8 kHz G.711 on the wire, a halfband
 * filter between. Both ways at once, unlike a reflector: without a headset
 * the microphone is held back while the far end talks, against the speaker's
 * echo. One task does it all, on core 0 beside lwIP.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include "audio_in.h"
#include "audio_out.h"
#include "bt_link.h"
#include "net_prov.h"
#include "ptt_fsm.h"
#include "radio.h"

#include "contacts.h"
#include "g711.h"
#include "phone_client.h"
#include "g722.h"
#include "phone_priv.h"
#include "sip.h"

static const char *TAG = "phone";

#define NVS_NS       "phone"
#define FRAME        320            /* 20 ms at 16 kHz: the knob's audio   */
#define NB           160            /* 20 ms at 8 kHz: a G.711 packet      */
#define HB_M         16             /* the halfband's odd taps a side: 63  */
#define RTP_BASE     40000
#define LEVELS       64
#define SILENT_DB    -90.0f
/* What the speaker keeps queued in a call: the network's packets come up to
 * 70 ms apart (measured), and a queue that runs dry between them leaves a
 * hole each time -- on the jack, and in a headset fed from it: a robotic
 * voice. */
#define PLAYOUT_MS   100            /* the least cushion, the network calm  */
#define PLAYOUT_TOP_MS 240         /* the most, after it has been rough     */
#define PLAYOUT_OVER_MS 60         /* more over the cushion: pauses shortened */
#define PLAYOUT_UNDER_MS 40        /* more under it: pauses lengthened        */
#define PLAYOUT_MAX_MS 400         /* more queued still: dropped back at once */
#define LATE_BLOCK   100            /* packets: the network's quickest, in    */
#define LATE_BLOCKS  8              /* 8 blocks of 2 s                        */
#define WORST_BLOCK  500            /* packets: its worst lateness, kept for  */
#define WORST_BLOCKS 6              /* 6 blocks of 10 s, the last minute      */
#define FLOOR_BLOCK  25             /* frames: the far end's floor, the quietest */
#define FLOOR_BLOCKS 6              /* of 6 x 0.5 s */

#define ECHO_DB      -42.0f         /* the far end louder: our mic held     */
#define ECHO_HOLD_MS 250
#define DTMF_MS      120            /* a key's tone                          */

static inline uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

/* ------------------------------------------------------------- settings */

static sip_account_t s_acc;
static volatile bool s_acc_dirty;
static SemaphoreHandle_t s_fav_mx;
static StaticSemaphore_t s_fav_mx_buf;
EXT_RAM_BSS_ATTR static phone_fav_t s_favs[PHONE_FAV_MAX];
static int s_nfav;

static void nvs_str(nvs_handle_t h, const char *k, char *out, size_t cap)
{
    size_t n = cap;
    if (nvs_get_str(h, k, out, &n) != ESP_OK) out[0] = 0;
}

static void settings_load(void)
{
    nvs_handle_t h;
    memset(&s_acc, 0, sizeof s_acc);
    s_acc.port = 5060;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    nvs_str(h, "user", s_acc.user, sizeof s_acc.user);
    nvs_str(h, "pass", s_acc.pass, sizeof s_acc.pass);
    nvs_str(h, "domain", s_acc.domain, sizeof s_acc.domain);
    nvs_str(h, "number", s_acc.number, sizeof s_acc.number);
    uint16_t port = 0;
    if (nvs_get_u16(h, "port", &port) == ESP_OK && port) s_acc.port = port;
    size_t len = sizeof s_favs;
    if (nvs_get_blob(h, "favs", s_favs, &len) == ESP_OK) s_nfav = (int)(len / sizeof s_favs[0]);
    nvs_close(h);
}

void phone_account_get(sip_account_t *a, bool *has_pass)
{
    *a = s_acc;
    if (has_pass) *has_pass = s_acc.pass[0] != 0;
    a->pass[0] = 0;
}

esp_err_t phone_account_set(const sip_account_t *a)
{
    sip_account_t n = *a;
    if (!n.pass[0]) strlcpy(n.pass, s_acc.pass, sizeof n.pass);
    if (!n.port) n.port = 5060;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_str(h, "user", n.user);
    nvs_set_str(h, "pass", n.pass);
    nvs_set_str(h, "domain", n.domain);
    nvs_set_str(h, "number", n.number);
    nvs_set_u16(h, "port", n.port);
    e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) {
        s_acc = n;
        s_acc_dirty = true;
        ESP_LOGI(TAG, "account %s@%s saved", n.user, n.domain);
    }
    return e;
}

int phone_favs_get(phone_fav_t *out, int max)
{
    xSemaphoreTake(s_fav_mx, portMAX_DELAY);
    const int n = MIN(max, s_nfav);
    memcpy(out, s_favs, (size_t)n * sizeof *out);
    xSemaphoreGive(s_fav_mx);
    return n;
}

esp_err_t phone_favs_set(const phone_fav_t *in, int n)
{
    if (n < 0) n = 0;
    if (n > PHONE_FAV_MAX) n = PHONE_FAV_MAX;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = n ? nvs_set_blob(h, "favs", in, (size_t)n * sizeof *in) : nvs_erase_key(h, "favs");
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return e;
    xSemaphoreTake(s_fav_mx, portMAX_DELAY);
    memcpy(s_favs, in, (size_t)n * sizeof *in);
    s_nfav = n;
    xSemaphoreGive(s_fav_mx);
    ESP_LOGI(TAG, "%d favourite%s", n, n == 1 ? "" : "s");
    return ESP_OK;
}

/* ------------------------------------------------------------- requests */

static volatile int32_t s_detents;
static volatile bool    s_toggle, s_hangup_req, s_answer_req, s_mute_req, s_mute_val;
static volatile bool    s_dial_due;
static char             s_dial[32];
static char             s_dtmf_q[16];
static volatile uint8_t s_dtmf_w, s_dtmf_r;
static portMUX_TYPE     s_req = portMUX_INITIALIZER_UNLOCKED;

bool phone_dial(const char *number)
{
    if (!number || !*number) return false;
    sip_reg_t r = sip_reg_state(NULL, 0);
    if (r != SIP_REG_OK) return false;
    taskENTER_CRITICAL(&s_req);
    strlcpy(s_dial, number, sizeof s_dial);
    s_dial_due = true;
    taskEXIT_CRITICAL(&s_req);
    return true;
}

void phone_answer(void) { s_answer_req = true; }
void phone_hangup(void) { s_hangup_req = true; }

void phone_dtmf(char key)
{
    if (!strchr("0123456789*#ABCD", key)) return;
    taskENTER_CRITICAL(&s_req);
    if ((uint8_t)(s_dtmf_w - s_dtmf_r) < sizeof s_dtmf_q)
        s_dtmf_q[s_dtmf_w++ % sizeof s_dtmf_q] = key;
    taskEXIT_CRITICAL(&s_req);
}

/* ------------------------------------------------------------- the task's */

static struct {
    int        rtp_fd;
    uint16_t   rtp_port;
    sip_media_t media;
    struct sockaddr_in dst;
    bool       latched;
    uint16_t   seq;
    uint32_t   ts, ssrc;
    uint64_t   t_next;
    int        sel;                      /* the favourite chosen */
    radio_call_t call;
    char       peer[32], peer_num[24], why[16];
    uint64_t   t_call;                   /* placed, rang, or answered */
    uint64_t   t_up;                     /* answered, this call; 0 if not */
    bool       out;                      /* this call ours, not theirs */
    bool       muted;
    float      lvl[LEVELS];
    uint32_t   lvl_n;
    uint64_t   t_audio, t_far;           /* audio heard; the far end loud   */
    float      mic_db;
    /* resamplers' history */
    int16_t    up_hist[2 * HB_M - 1], dn_hist[4 * HB_M - 2];
    /* DTMF being sent */
    char       key;
    int        key_frames;
    uint32_t   key_ts;
    bool       key_end;
    int        key_ends;
    uint32_t   tone_n;                   /* in-band and local tones */
    bool       mic_on;
    uint64_t   t_tone;                   /* ring, ringback, busy: their clock */
    uint32_t   tone_fed;                 /* ms of them queued since */
    uint32_t   rx_packets, tx_packets, rx_lost;
    uint32_t   rx_other, rx_late;        /* dropped: not G.711; behind */
    uint32_t   rx_shortened;             /* frames of pauses not played: behind */
    uint32_t   rx_lengthened;            /* frames of pauses played twice: short */
    /* The network's lateness: each packet's arrival against its timestamp,
     * over the quickest of the last 16 s (block minima, so a clock drifting
     * apart is followed), and the worst of it lately, falling back slowly. */
    bool       lt_on;
    uint32_t   lt_ts0;
    uint64_t   lt_t0;
    int32_t    lt_min[LATE_BLOCKS], lt_cur;
    uint8_t    lt_n, lt_i, lt_have;
    float      late_ms, late_now;        /* the worst lately; this packet's */
    float      wl_max[WORST_BLOCKS], wl_cur;
    uint16_t   wl_n;
    uint8_t    wl_i, wl_have;
    float      fl_min[FLOOR_BLOCKS], fl_cur; /* the far end's floor, in dB */
    uint8_t    fl_n, fl_i;
    uint32_t   rx_ssrc;                  /* the stream being played */
    int16_t    carry[NB];                /* decoded, short of a 20 ms frame */
    uint16_t   carry_n;
    /* G.722 (payload 9): its coder each way, and its 16 kHz samples short of
     * a frame -- no resampling either way. */
    g722_enc_t g722e;
    g722_dec_t g722d;
    int16_t    carry16[FRAME];
    uint16_t   carry16_n;
    uint8_t    codec_pt;                 /* the codec those belong to */
    uint16_t   rx_seq;
    bool       rx_seq_ok;
} C = { .rtp_fd = -1, .sel = 0 };

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    radio_link_t link;
    char         link_why[16];
    radio_call_t call;
    char         call_why[16], peer[32], peer_num[24];
    uint64_t     t_call;
    int          sel, nfav;
    char         fav_name[32], fav_num[24];
    bool         muted;
    float        rx_db, mic_db;
    bool         hd;                    /* the call in G.722 */
    uint32_t     txa_sent, txa_failed;
} P;

/* ------------------------------------------------------------- 8 <-> 16 kHz */

/* A halfband low-pass at 16 kHz, 15 taps: every other one zero but the
 * centre, so each direction is a few multiplies a sample. */
/* The halfband for 8 <-> 16 kHz: 63 taps, Kaiser-windowed (beta 8), its odd
 * taps out from the centre, nearest first. Flat to 3.4 kHz, 58 dB down at
 * 4.6 kHz, 85 from 5 kHz: a voice's mirror image above 4 kHz, which a 15-tap
 * filter left 15 dB down -- metallic in a wideband headset -- is gone, and so
 * is a headset's sibilance folded back into what the far end hears. */
#define HB_CENTRE 0.49999997f
static const float HB[HB_M] = {
    +0.31707287f, -0.10244251f, +0.05772404f, -0.03748938f,
    +0.02563769f, -0.01780470f, +0.01231558f, -0.00837621f,
    +0.00554365f, -0.00353441f, +0.00214591f, -0.00122208f,
    +0.00063811f, -0.00029356f, +0.00010904f, -0.00002402f
};

/* 8 kHz -> 16 kHz: the samples as they are, and between each two the
 * filter's. Delay: 3.5 input samples. */
static void up2(const int16_t *in, int16_t *out, int n)
{
    enum { H = 2 * HB_M - 1 };
    int16_t h[H + NB];
    memcpy(h, C.up_hist, sizeof C.up_hist);
    memcpy(h + H, in, (size_t)n * sizeof *in);
    for (int i = 0; i < n; i++) {
        const int16_t *x = h + i + H;       /* x[0] is in[i] */
        /* Between x[-HB_M] and x[-HB_M + 1]: the pairs about that middle. */
        float v = 0.0f;
        for (int k = 0; k < HB_M; k++) v += HB[k] * (float)(x[-(HB_M - 1) + k] + x[-HB_M - k]);
        out[2 * i]     = (int16_t)lrintf(2.0f * HB_CENTRE * (float)x[-HB_M]);
        out[2 * i + 1] = (int16_t)MAX(-32768, MIN(32767, lrintf(2.0f * v)));
    }
    memcpy(C.up_hist, h + n, sizeof C.up_hist);
}

/* 16 kHz -> 8 kHz: filtered, every other sample kept. */
static void down2(const int16_t *in, int16_t *out, int n)
{
    enum { H = 4 * HB_M - 2 };
    int16_t h[H + FRAME];
    memcpy(h, C.dn_hist, sizeof C.dn_hist);
    memcpy(h + H, in, (size_t)n * sizeof *in);
    for (int i = 0; i < n / 2; i++) {
        const int16_t *x = h + 2 * i + H;   /* the newest of 4 * HB_M - 1 */
        float v = HB_CENTRE * (float)x[-(2 * HB_M - 1)];
        for (int k = 0; k < HB_M; k++)
            v += HB[k] * (float)(x[-(2 * HB_M - 2) + 2 * k] + x[-2 * HB_M - 2 * k]);
        out[i] = (int16_t)MAX(-32768, MIN(32767, lrintf(v)));
    }
    memcpy(C.dn_hist, h + n, sizeof C.dn_hist);
}

/* ------------------------------------------------------------- tones */

static float tone2(float f1, float f2, uint32_t n)
{
    return 0.5f * (sinf(2.0f * (float)M_PI * f1 * n / 16000.0f) +
                   sinf(2.0f * (float)M_PI * f2 * n / 16000.0f));
}

static void dtmf_freqs(char k, float *lo, float *hi)
{
    static const char *const KEYS = "123A456B789C*0#D";
    static const float ROW[4] = { 697, 770, 852, 941 }, COL[4] = { 1209, 1336, 1477, 1633 };
    const char *p = strchr(KEYS, k);
    const int i = p ? (int)(p - KEYS) : 13;
    *lo = ROW[i / 4];
    *hi = COL[i % 4];
}

/* One 20 ms frame of a call's progress tones, for the speaker: the ring of a
 * call coming in, the ringback of one going out (Belgium's 425 Hz, 1 s on,
 * 3 s off), the busy tone. Silent between. */
static bool progress_tone(int16_t *out, uint64_t t)
{
    const uint32_t ms = (uint32_t)(t - C.t_tone);
    float amp = 0.0f, f1 = 0, f2 = 0;
    if (C.call == RADIO_CALL_IN) {
        const uint32_t p = ms % 3000;               /* ring-ring, rest */
        if (p < 400 || (p >= 600 && p < 1000)) { amp = 0.30f; f1 = 440; f2 = 480; }
    } else if (C.call == RADIO_CALL_OUT && !strcmp(C.why, "ringing") && !C.media.on) {
        if (ms % 4000 < 1000) { amp = 0.18f; f1 = f2 = 425; }
    } else if (C.call == RADIO_CALL_ENDED && !strcmp(C.why, "busy")) {
        if (ms % 1000 < 500) { amp = 0.18f; f1 = f2 = 425; }
    } else {
        return false;
    }
    for (int i = 0; i < FRAME; i++, C.tone_n++)
        out[i] = amp ? (int16_t)(amp * 32767.0f * tone2(f1, f2, C.tone_n)) : 0;
    return true;
}

/* ------------------------------------------------------------- levels */

static void level_push(const int16_t *pcm, int n)
{
    int pk = 0;
    for (int i = 0; i < n; i++) pk = MAX(pk, abs(pcm[i]));
    const float db = pk ? 20.0f * log10f(pk / 32768.0f) : SILENT_DB;
    C.lvl[C.lvl_n++ % LEVELS] = db;
    C.t_audio = now_ms();
    /* Loud: when it will be heard -- behind what is queued, and the jack's
     * 45 ms of DMA -- not when it came in. The echo hold runs from there. */
    if (db > ECHO_DB) C.t_far = C.t_audio + audio_out_queued() * 1000u / AUDIO_RATE_HZ + 45;
}

static float level_now(uint64_t t)
{
    const uint32_t queued = (uint32_t)(audio_out_queued() / FRAME);
    if (t - C.t_audio > 250 && queued == 0) return SILENT_DB;
    const uint32_t behind = MIN(queued, LEVELS - 1);
    if (behind >= C.lvl_n) return SILENT_DB;
    return C.lvl[(C.lvl_n - 1 - behind) % LEVELS];
}

/* ------------------------------------------------------------- RTP */

static void rtp_open(void)
{
    if (C.rtp_fd >= 0) return;
    for (int tries = 0; tries < 20 && C.rtp_fd < 0; tries++) {
        const uint16_t port = (uint16_t)(RTP_BASE + 2 * (esp_random() % 2000));
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return;
        struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(port),
                                  .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (bind(fd, (struct sockaddr *)&me, sizeof me) == 0) {
            C.rtp_fd = fd;
            C.rtp_port = port;
        } else {
            close(fd);
        }
    }
}

static void rtp_send(uint8_t pt, bool marker, const uint8_t *payload, size_t n, uint32_t ts)
{
    uint8_t pkt[12 + 200];
    if (n > 200 || !C.media.on) return;
    pkt[0] = 0x80;
    pkt[1] = (uint8_t)((marker ? 0x80 : 0) | (pt & 0x7F));
    pkt[2] = (uint8_t)(C.seq >> 8); pkt[3] = (uint8_t)C.seq;
    pkt[4] = (uint8_t)(ts >> 24); pkt[5] = (uint8_t)(ts >> 16); pkt[6] = (uint8_t)(ts >> 8); pkt[7] = (uint8_t)ts;
    pkt[8] = (uint8_t)(C.ssrc >> 24); pkt[9] = (uint8_t)(C.ssrc >> 16);
    pkt[10] = (uint8_t)(C.ssrc >> 8); pkt[11] = (uint8_t)C.ssrc;
    memcpy(pkt + 12, payload, n);
    C.seq++;
    if (sendto(C.rtp_fd, pkt, 12 + n, 0, (struct sockaddr *)&C.dst, sizeof C.dst) > 0) {
        C.tx_packets++;
        P.txa_sent++;
    } else {
        P.txa_failed++;
    }
}

/* A frame of a pause: within 6 dB of the far end's floor -- the quietest
 * frame of the last 3 s, so a noisier room is followed and a long sentence
 * is not taken for one -- or near digital silence. Levels in dB over one
 * LSB; the floor starts at 0, so a call's first 3 s count silence alone. */
static volatile bool s_meters = true;    /* see phone_set_meters() */

/* A packet's lateness, in ms: its arrival against its RTP timestamp (8 kHz),
 * over the quickest of the last 16 s. The worst of the last minute is kept
 * in late_ms; the cushion follows it, back down a minute after the network
 * has calmed. */
static void late_note(uint32_t ts)
{
    const uint64_t t = now_ms();
    if (!C.lt_on) {
        C.lt_on = true;
        C.lt_ts0 = ts;
        C.lt_t0 = t;
        C.lt_n = C.lt_i = C.lt_have = 0;
    }
    const int32_t transit = (int32_t)((t - C.lt_t0) * 8) - (int32_t)(ts - C.lt_ts0);
    if (!C.lt_n || transit < C.lt_cur) C.lt_cur = transit;
    int32_t quickest = C.lt_cur;
    for (int b = 0; b < LATE_BLOCKS; b++)
        if (C.lt_have & (1u << b) && C.lt_min[b] < quickest) quickest = C.lt_min[b];
    if (++C.lt_n == LATE_BLOCK) {
        C.lt_min[C.lt_i] = C.lt_cur;
        C.lt_have |= (uint8_t)(1u << C.lt_i);
        C.lt_i = (uint8_t)((C.lt_i + 1) % LATE_BLOCKS);
        C.lt_n = 0;
    }
    const float late = (float)(transit - quickest) / 8.0f;
    /* Over a second: the timestamps jumped (a stream restarted under the same
     * SSRC), not a packet a second late -- measured afresh from the next. */
    if (late > 1000.0f) {
        C.lt_on = false;
        C.late_now = 0.0f;
        return;
    }
    C.late_now = late;
    /* The worst of the last minute, held: a line with a late spike every half
     * minute let a cushion that fell back in 10 s run dry at the next one. */
    if (!C.wl_n || late > C.wl_cur) C.wl_cur = late;
    float worst = C.wl_cur;
    for (int b = 0; b < WORST_BLOCKS; b++)
        if (C.wl_have & (1u << b) && C.wl_max[b] > worst) worst = C.wl_max[b];
    if (++C.wl_n == WORST_BLOCK) {
        C.wl_max[C.wl_i] = C.wl_cur;
        C.wl_have |= (uint8_t)(1u << C.wl_i);
        C.wl_i = (uint8_t)((C.wl_i + 1) % WORST_BLOCKS);
        C.wl_n = 0;
    }
    C.late_ms = worst;
}

/* The cushion the speaker keeps: the worst recent lateness and 30 ms more,
 * from PLAYOUT_MS to PLAYOUT_TOP_MS -- the delay the call has only while the
 * network makes it needed. */
static uint32_t cushion_ms(void)
{
    const float c = C.late_ms + 30.0f;
    return c < PLAYOUT_MS ? PLAYOUT_MS : c > PLAYOUT_TOP_MS ? PLAYOUT_TOP_MS : (uint32_t)c;
}

void phone_set_meters(bool on) { s_meters = on; }
bool phone_meters(void)        { return s_meters; }

static bool pause_frame(const int16_t *x)
{
    int64_t e = 0;
    for (int i = 0; i < FRAME; i++) e += (int32_t)x[i] * x[i];
    const float db = 10.0f * log10f((float)e / FRAME + 1.0f);
    if (!C.fl_n || db < C.fl_cur) C.fl_cur = db;
    float floor_db = C.fl_cur;
    for (int b = 0; b < FLOOR_BLOCKS; b++)
        if (C.fl_min[b] < floor_db) floor_db = C.fl_min[b];
    if (++C.fl_n == FLOOR_BLOCK) {
        C.fl_min[C.fl_i] = C.fl_cur;
        C.fl_i = (uint8_t)((C.fl_i + 1) % FLOOR_BLOCKS);
        C.fl_n = 0;
    }
    return db < floor_db + 6.0f || db < 30.0f;
}

static void play(const int16_t *pcm16k)
{
    const bool pause = pause_frame(pcm16k);
    level_push(pcm16k, FRAME);
    /* Nearly dry -- the call's start, after a ring that set the speaker off
     * frame by frame, or a gap the network left -- the cushion is laid
     * first, in silence, so the next gap is ridden out rather than heard. */
    const size_t queued = audio_out_queued();
    const uint32_t cush = cushion_ms();
    const size_t cush_n = AUDIO_RATE_HZ * cush / 1000;
    /* The level the queue would have, this packet on time: within a burst
     * after a late one, the queue climbs as each packet is less late, and
     * this stays put -- the pauses neither cut nor lengthened by the burst. */
    const size_t level = queued + (size_t)(C.late_now * (AUDIO_RATE_HZ / 1000));
    if (queued < FRAME / 2) {
        static const int16_t quiet[FRAME];
        for (size_t q = 0; q < cush_n; q += FRAME)
            audio_out_feed_pcm16(quiet, FRAME, 1);
        audio_out_kick();               /* under the pre-roll: start it now */
    } else if (queued > AUDIO_RATE_HZ * PLAYOUT_MAX_MS / 1000) {
        /* Far behind -- the speaker held up while the network went on: the
         * backlog dropped back to the cushion, not kept as a delay. */
        static uint64_t t_said;
        audio_out_trim(cush_n);
        if (now_ms() - t_said > 2000) {
            t_said = now_ms();
            ESP_LOGI(TAG, "%u ms queued to play: back to %u", (unsigned)(queued * 1000u / AUDIO_RATE_HZ),
                     (unsigned)cush);
        }
    } else if (pause && level > cush_n + AUDIO_RATE_HZ * PLAYOUT_OVER_MS / 1000) {
        /* Behind -- a gap the network made up for in a burst, or the speaker
         * held up: a pause shortened by this frame, so the delay goes without
         * a word lost. Dropping the backlog at once cut 150 ms of speech. */
        C.rx_shortened++;
        return;
    } else if (pause && level + FRAME + AUDIO_RATE_HZ * PLAYOUT_UNDER_MS / 1000 < cush_n) {
        /* Short of the cushion the network now asks for: a pause lengthened
         * by this frame, so the next late packet finds the cushion there
         * rather than an empty speaker. */
        audio_out_feed_pcm16(pcm16k, FRAME, 1);
        C.rx_lengthened++;
    }
    audio_out_feed_pcm16(pcm16k, FRAME, 1);
}

static void rtp_rx(void)
{
    uint8_t pkt[512];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    const int n = recvfrom(C.rtp_fd, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
    if (n < 12 || (pkt[0] & 0xC0) != 0x80 || !C.media.on) return;
    /* Symmetric RTP: the far end's media comes from where we send it --
     * behind its NAT, a port its SDP could not know. Latched once. */
    if (!C.latched && (from.sin_addr.s_addr != C.dst.sin_addr.s_addr || from.sin_port != C.dst.sin_port)) {
        C.dst = from;
        C.latched = true;
        ESP_LOGI(TAG, "RTP latched to %s:%u", inet_ntoa(from.sin_addr), (unsigned)ntohs(from.sin_port));
    }
    const uint8_t pt = pkt[1] & 0x7F;
    const uint16_t seq = (uint16_t)(pkt[2] << 8 | pkt[3]);
    const uint32_t ssrc = (uint32_t)pkt[8] << 24 | (uint32_t)pkt[9] << 16 | (uint32_t)pkt[10] << 8 | pkt[11];
    size_t off = 12 + 4u * (pkt[0] & 0x0F);
    if (pkt[0] & 0x10) {                       /* a header extension */
        if ((size_t)n < off + 4) return;
        off += 4 + 4u * (pkt[off + 2] << 8 | pkt[off + 3]);
    }
    size_t len = (size_t)n > off ? (size_t)n - off : 0;
    if (pkt[0] & 0x20 && len) len -= MIN(len, pkt[n - 1]);   /* padding */
    /* Comfort noise (RFC 3389), though never offered: the far end gone quiet
     * says how loud its background is, and that is what is heard until its
     * voice is back -- not a hole. */
    if (pt == 13 && len >= 1) {
        const float a = 32767.0f * powf(10.0f, -(float)(pkt[off] & 0x7F) / 20.0f);
        int16_t nb[NB], wb[FRAME];
        for (int i = 0; i < NB; i++) nb[i] = (int16_t)(a * ((float)(esp_random() & 0xFFFF) / 32768.0f - 1.0f));
        up2(nb, wb, NB);
        play(wb);
        if (ssrc == C.rx_ssrc && C.rx_seq_ok) C.rx_seq = seq;    /* not lost */
        return;
    }
    /* G.711 either way, A-law or u-law as the packet says: an exchange can
     * answer in the other one than it rang in. DTMF events, and anything
     * else, are not ours to play. */
    if ((pt != 0 && pt != 8 && pt != 9) || len == 0) {
        if ((int)pt != C.media.dtmf_pt) C.rx_other++;
        else if (ssrc == C.rx_ssrc && C.rx_seq_ok) C.rx_seq = seq; /* not lost */
        return;
    }
    /* Another stream -- the callee's own, once answered, after a media
     * server's ringing -- numbers its packets afresh. */
    if (ssrc != C.rx_ssrc) {
        C.rx_ssrc   = ssrc;
        C.rx_seq_ok = false;
        C.lt_on     = false;                  /* its clock, from scratch */
        C.carry_n   = C.carry16_n = 0;
        g722_dec_init(&C.g722d);              /* its coder, from scratch too */
    }
    if (C.rx_seq_ok) {
        const uint16_t gap = (uint16_t)(seq - C.rx_seq - 1);
        if (gap && gap < 50) {
            C.rx_lost += gap;
        } else if (gap > 0xFFFF - 50) {         /* late: already played past */
            C.rx_late++;
            return;
        } else if (gap) {                       /* further off: a jump, followed */
            C.lt_on = false;
            C.carry_n = C.carry16_n = 0;
        }
    }
    C.rx_seq = seq;
    C.rx_seq_ok = true;
    C.rx_packets++;
    late_note((uint32_t)pkt[4] << 24 | (uint32_t)pkt[5] << 16 | (uint32_t)pkt[6] << 8 | pkt[7]);
    /* The network's side, measured every 10 s in a call: the longest wait
     * between packets, and what the speaker has queued. */
    {
        static uint64_t t0, t_last;
        static uint32_t n, gap_max;
        const uint64_t t = now_ms();
        if (!t0 || C.rx_packets == 1) { t0 = t; n = 0; gap_max = 0; }   /* a call's first */
        else if (t - t_last > gap_max) gap_max = (uint32_t)(t - t_last);
        t_last = t;
        n++;
        if (t - t0 >= 10000) {
            ESP_LOGI(TAG, "network: %lu packets in %.1f s, longest gap %lu ms, lately %.0f ms late; "
                     "%u ms queued to play, cushion %lu; pauses so far %lu ms cut, %lu ms added",
                     (unsigned long)n, (double)(t - t0) / 1000.0, (unsigned long)gap_max,
                     (double)C.late_ms, (unsigned)(audio_out_queued() * 1000u / AUDIO_RATE_HZ),
                     (unsigned long)cushion_ms(), (unsigned long)C.rx_shortened * 20,
                     (unsigned long)C.rx_lengthened * 20);
            t0 = t;
            n = 0;
            gap_max = 0;
        }
    }
    if (pt == 9) {
        /* G.722: 16 kHz as it comes, two samples a byte, through its own carry
         * to 20 ms frames -- no resampling. */
        for (size_t at = 0; at < len; ) {
            const size_t k = MIN(len - at, (size_t)((FRAME - C.carry16_n) / 2));
            g722_decode(&C.g722d, pkt + off + at, (int)k, C.carry16 + C.carry16_n);
            C.carry16_n = (uint16_t)(C.carry16_n + 2 * k);
            at += k;
            if (C.carry16_n == FRAME) {
                play(C.carry16);
                C.carry16_n = 0;
            }
        }
        return;
    }
    /* Re-framed to 20 ms through a carry: a far end's 10, 30 or 40 ms packets
     * (its own ptime) play whole and seamless, not padded or cut. */
    for (size_t at = 0; at < len; ) {
        const size_t k = MIN(len - at, (size_t)(NB - C.carry_n));
        g711_decode(pt, pkt + off + at, C.carry + C.carry_n, (int)k);
        C.carry_n = (uint16_t)(C.carry_n + k);
        at += k;
        if (C.carry_n == NB) {
            int16_t wb[FRAME];
            up2(C.carry, wb, NB);
            play(wb);
            C.carry_n = 0;
        }
    }
}

/* One frame of DTMF, if a key is going: RFC 4733 where the far end takes it,
 * else its tones into `pcm`. True if the frame was an event, sent. */
static bool dtmf_frame(int16_t *pcm)
{
    if (!C.key) {
        taskENTER_CRITICAL(&s_req);
        if (s_dtmf_r != s_dtmf_w) C.key = s_dtmf_q[s_dtmf_r++ % sizeof s_dtmf_q];
        taskEXIT_CRITICAL(&s_req);
        if (!C.key) return false;
        C.key_frames = 0;
        C.key_ends = 0;
        C.key_ts = C.ts;
        ESP_LOGI(TAG, "DTMF %c", C.key);
    }
    float lo, hi;
    dtmf_freqs(C.key, &lo, &hi);
    const int frames = DTMF_MS / 20;
    if (C.media.dtmf_pt >= 0) {
        static const char *const EV = "0123456789*#ABCD";
        const uint8_t ev = (uint8_t)(strchr(EV, C.key) - EV);
        const bool end = C.key_frames >= frames;
        const uint16_t dur = (uint16_t)(NB * MIN(C.key_frames + 1, frames));
        const uint8_t p[4] = { ev, (uint8_t)((end ? 0x80 : 0) | 10), (uint8_t)(dur >> 8), (uint8_t)dur };
        rtp_send((uint8_t)C.media.dtmf_pt, C.key_frames == 0, p, sizeof p, C.key_ts);
        C.key_frames++;
        if (end && ++C.key_ends >= 3) {
            C.key = 0;
            C.ts += NB * (uint32_t)frames;
        }
        /* The key, heard here too. */
        if (!end) {
            int16_t t[FRAME];
            for (int i = 0; i < FRAME; i++, C.tone_n++) t[i] = (int16_t)(0.15f * 32767.0f * tone2(lo, hi, C.tone_n));
            audio_out_feed_pcm16(t, FRAME, 1);
            audio_out_kick();           /* no far end flowing: under the pre-roll */
        }
        return true;
    }
    /* In the audio: the tone for its frames, then a frame of quiet. */
    if (C.key_frames < frames) {
        for (int i = 0; i < FRAME; i++, C.tone_n++) pcm[i] = (int16_t)(0.35f * 32767.0f * tone2(lo, hi, C.tone_n));
    } else {
        memset(pcm, 0, FRAME * sizeof *pcm);
        C.key = 0;
    }
    C.key_frames++;
    return false;
}

/* Every 20 ms while media flows: the microphone (or quiet, muted, early,
 * held against echo) as G.711; DTMF in its place while a key goes. */
static void tx_pump(uint64_t t)
{
    while (C.media.on && (int64_t)(t - C.t_next) >= 0) {
        int16_t pcm[FRAME], nb[NB];
        uint8_t out[NB];
        const bool talk = C.call == RADIO_CALL_UP;
        if (!talk || !C.mic_on || !audio_in_take(pcm, FRAME)) memset(pcm, 0, sizeof pcm);
        const bool headset = bt_link_headset_audio();
        /* The face's mute is the knob's own microphone's; a headset has its
         * own, on the headset. */
        const bool muted = headset ? bt_link_headset_muted() : C.muted;
        if (muted) memset(pcm, 0, sizeof pcm);
        /* No headset: the speaker is beside the microphone. While the far end
         * is loud, ours is held back -- a speakerphone's half duplex. */
        if (!headset && (int64_t)(t - C.t_far) < ECHO_HOLD_MS)     /* until 250 ms after it is heard */
            for (int i = 0; i < FRAME; i++) pcm[i] /= 8;
        int pk = 0;
        for (int i = 0; i < FRAME; i++) pk = MAX(pk, abs(pcm[i]));
        C.mic_db = pk ? 20.0f * log10f(pk / 32768.0f) : SILENT_DB;
        if (talk && dtmf_frame(pcm)) {
            C.t_next += 20;
            continue;
        }
        if (C.media.pt == 9) {
            g722_encode(&C.g722e, pcm, FRAME, out);    /* 160 bytes: 16 kHz as it is */
        } else {
            down2(pcm, nb, FRAME);
            g711_encode(C.media.pt, nb, out, NB);
        }
        rtp_send(C.media.pt, false, out, NB, C.ts);    /* 8 kHz RTP clock for both */
        C.ts += NB;
        C.t_next += 20;
        if ((int64_t)(t - C.t_next) > 200) C.t_next = t;     /* a stall: not made up */
    }
}

/* ------------------------------------------------------------- SIP's events */

static void set_mic(bool on)
{
    if (on == C.mic_on) return;
    C.mic_on = on;
    audio_in_set_active(on);
}

/* ------------------------------------------------------------- the history */

EXT_RAM_BSS_ATTR static phone_call_t s_hist[PHONE_HIST_MAX];
static int               s_nhist;
static volatile uint32_t s_hist_seq;
static volatile uint8_t  s_missed;              /* fresh ones, for the face */
static volatile bool     s_hist_dirty, s_hist_saving;
static uint64_t          s_hist_hold;
static SemaphoreHandle_t s_hist_mx;
static StaticSemaphore_t s_hist_mx_buf;

static void history_count(void)
{
    uint8_t n = 0;
    for (int i = 0; i < s_nhist; i++) n += s_hist[i].fresh;
    s_missed = n;
}

static void history_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof s_hist;
    if (nvs_get_blob(h, "hist", s_hist, &n) == ESP_OK) s_nhist = (int)(n / sizeof s_hist[0]);
    nvs_close(h);
    history_count();
}

static void history_save_task(void *arg)
{
    (void)arg;
    phone_call_t *copy = heap_caps_malloc(sizeof s_hist, MALLOC_CAP_SPIRAM);
    if (copy) {
        xSemaphoreTake(s_hist_mx, portMAX_DELAY);
        const int n = s_nhist;
        memcpy(copy, s_hist, sizeof s_hist[0] * (size_t)n);
        xSemaphoreGive(s_hist_mx);
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_set_blob(h, "hist", copy, sizeof copy[0] * (size_t)n) == ESP_OK) nvs_commit(h);
            nvs_close(h);
        }
        free(copy);
    }
    s_hist_saving = false;
    vTaskDelete(NULL);
}

/* The history to flash, after a change: on a task of its own, as this one's
 * stack is in PSRAM and a flash write wants an internal one. */
static void history_flush(uint64_t t)
{
    if (!s_hist_dirty || s_hist_saving || t < s_hist_hold) return;
    s_hist_dirty  = false;
    s_hist_saving = true;
    if (xTaskCreatePinnedToCore(history_save_task, "phist", 4096, NULL, 2, NULL, 0) != pdPASS) {
        s_hist_saving = false;
        s_hist_dirty  = true;
        s_hist_hold   = t + 10000;             /* no memory just now */
    }
}

/* A call over: into the history, newest first. */
static void history_add(void)
{
    phone_call_t e = { 0 };
    strlcpy(e.number, C.peer_num, sizeof e.number);
    if (!e.number[0]) return;
    if (strcmp(C.peer, C.peer_num)) strlcpy(e.name, C.peer, sizeof e.name);
    const time_t now = time(NULL);
    e.when  = now > 1700000000 ? (uint32_t)now : 0;
    e.secs  = C.t_up ? (uint16_t)MIN((now_ms() - C.t_up) / 1000, 65535) : 0;
    e.kind  = C.out ? PHONE_CALL_OUT : C.t_up ? PHONE_CALL_IN
            : !strcmp(C.why, "declined") ? PHONE_CALL_DECLINED : PHONE_CALL_MISSED;
    e.fresh = e.kind == PHONE_CALL_MISSED;
    xSemaphoreTake(s_hist_mx, portMAX_DELAY);
    memmove(&s_hist[1], &s_hist[0], sizeof s_hist[0] * (size_t)MIN(s_nhist, PHONE_HIST_MAX - 1));
    s_hist[0] = e;
    if (s_nhist < PHONE_HIST_MAX) s_nhist++;
    history_count();
    xSemaphoreGive(s_hist_mx);
    s_hist_seq++;
    s_hist_dirty = true;
}

int phone_history(phone_call_t *out, int max)
{
    if (!s_hist_mx || !out) return 0;
    xSemaphoreTake(s_hist_mx, portMAX_DELAY);
    const int n = MIN(s_nhist, max);
    memcpy(out, s_hist, sizeof s_hist[0] * (size_t)n);
    xSemaphoreGive(s_hist_mx);
    return n;
}

uint32_t phone_history_seq(void) { return s_hist_seq; }

void phone_history_seen(void)
{
    if (!s_hist_mx || !s_missed) return;
    xSemaphoreTake(s_hist_mx, portMAX_DELAY);
    for (int i = 0; i < s_nhist; i++) s_hist[i].fresh = 0;
    s_missed = 0;
    xSemaphoreGive(s_hist_mx);
    s_hist_seq++;
    s_hist_dirty = true;
}

static void on_call(sip_call_t s, const char *peer, const char *number, const char *why)
{
    static const radio_call_t MAP[] = {
        [SIP_CALL_IDLE] = RADIO_CALL_IDLE, [SIP_CALL_OUT] = RADIO_CALL_OUT,
        [SIP_CALL_IN] = RADIO_CALL_IN, [SIP_CALL_UP] = RADIO_CALL_UP,
        [SIP_CALL_ENDED] = RADIO_CALL_ENDED,
    };
    const radio_call_t was = C.call;
    C.call = MAP[s];
    strlcpy(C.why, why ? why : "", sizeof C.why);
    if (C.call != was) {
        /* Which way it began, and whether it was answered: the history's. */
        if (C.call == RADIO_CALL_OUT || C.call == RADIO_CALL_IN) {
            C.out  = C.call == RADIO_CALL_OUT;
            C.t_up = 0;
        }
        if (C.call == RADIO_CALL_UP) C.t_up = now_ms();
        C.t_call = C.t_tone = now_ms();
        C.tone_fed = 0;
        C.tone_n = 0;
    }
    if (peer && *peer) {
        /* Who, by the favourites' name for the number where there is one. */
        strlcpy(C.peer, peer, sizeof C.peer);
        strlcpy(C.peer_num, number ? number : "", sizeof C.peer_num);
        xSemaphoreTake(s_fav_mx, portMAX_DELAY);
        for (int i = 0; i < s_nfav && number && *number; i++) {
            const char *a = s_favs[i].number, *b = number;
            const size_t la = strlen(a), lb = strlen(b);
            const size_t k = MIN(la, lb);
            /* The last nine digits match: +3259... and 059... are one number. */
            if (k >= 6 && !strcmp(a + la - MIN(k, 9), b + lb - MIN(k, 9))) {
                strlcpy(C.peer, s_favs[i].name, sizeof C.peer);
                break;
            }
        }
        xSemaphoreGive(s_fav_mx);
    }
    if (C.call == RADIO_CALL_ENDED && was != RADIO_CALL_ENDED && was != RADIO_CALL_IDLE)
        history_add();
    set_mic(C.call == RADIO_CALL_UP);
    /* A call begun: the microphone live in a headset, the knob's own muted
     * until asked for -- a tap on the speaker -- as it hears the jack's
     * speaker and the room. Answering keeps what was set while it rang. */
    if (C.call != was && (C.call == RADIO_CALL_OUT || C.call == RADIO_CALL_IN))
        C.muted = !bt_link_headset_connected();
    /* The headset's audio only for a call: from its ringing -- in the
     * headset as on the jack -- or from dialling, its ringback, through the
     * call and its last seconds (a busy tone). Open while it rings, an
     * answer is at once: no two seconds' wait for the headset. */
    static bool hs_audio;
    if (C.call == RADIO_CALL_OUT || C.call == RADIO_CALL_IN || C.call == RADIO_CALL_UP) hs_audio = true;
    else if (C.call != RADIO_CALL_ENDED) hs_audio = false;
    bt_link_want_audio(hs_audio);
    /* No flush at the answer: a flush stops the speaker until it has its
     * pre-roll again, and a headset fed from it runs dry meanwhile. What was
     * queued of the ring plays out instead -- 80 ms at most. */
    if (C.call == RADIO_CALL_ENDED || C.call == RADIO_CALL_IDLE) {
        audio_out_kick();               /* the rest plays out, not parked */
        C.carry_n = 0;
        C.key = 0;
        s_dtmf_r = s_dtmf_w;
        if (C.rx_packets || C.tx_packets)
            ESP_LOGI(TAG, "call over: %lu packets in (%lu lost, %lu late, %lu not audio), %lu out; "
                     "pauses %lu ms cut, %lu ms added",
                     (unsigned long)C.rx_packets, (unsigned long)C.rx_lost, (unsigned long)C.rx_late,
                     (unsigned long)C.rx_other, (unsigned long)C.tx_packets,
                     (unsigned long)C.rx_shortened * 20, (unsigned long)C.rx_lengthened * 20);
        C.rx_packets = C.tx_packets = C.rx_lost = C.rx_late = C.rx_other = 0;
        C.rx_shortened = C.rx_lengthened = 0;
        C.lt_on = false;
        C.late_ms = C.late_now = 0.0f;
        C.wl_n = C.wl_i = C.wl_have = 0;
        memset(C.fl_min, 0, sizeof C.fl_min);
        C.fl_n = C.fl_i = 0;
    }
}

static const char *codec_name(uint8_t pt)
{
    return pt == 9 ? "G.722" : pt == 8 ? "PCMA" : "PCMU";
}

static void on_media(const sip_media_t *m)
{
    const bool was = C.media.on;
    C.media = *m;
    if (!m->on) {
        C.latched = false;
        C.rx_seq_ok = false;
        return;
    }
    if (!was || C.dst.sin_addr.s_addr != m->ip || ntohs(C.dst.sin_port) != m->port) {
        if (was) {
            struct in_addr a = { .s_addr = m->ip };
            ESP_LOGI(TAG, "media now %s at %s:%u", codec_name(m->pt), inet_ntoa(a),
                     (unsigned)m->port);
        }
        memset(&C.dst, 0, sizeof C.dst);
        C.dst.sin_family = AF_INET;
        C.dst.sin_addr.s_addr = m->ip;
        C.dst.sin_port = htons(m->port);
        C.latched = false;
    }
    if (!was) {
        C.ssrc = esp_random();
        C.seq = (uint16_t)esp_random();
        C.ts = esp_random();
        C.t_next = now_ms();
        memset(C.up_hist, 0, sizeof C.up_hist);
        memset(C.dn_hist, 0, sizeof C.dn_hist);
        C.rx_seq_ok = false;
        ESP_LOGI(TAG, "media: %s to %s:%u%s", codec_name(m->pt),
                 inet_ntoa(C.dst.sin_addr), (unsigned)m->port, m->dtmf_pt >= 0 ? ", RFC 4733 DTMF" : "");
    }
    if (!was || m->pt != C.codec_pt) {
        g722_enc_init(&C.g722e);
        g722_dec_init(&C.g722d);
        C.carry_n = C.carry16_n = 0;
        C.codec_pt = m->pt;
    }
}

/* ------------------------------------------------------------- the loop */

/* Answering with a headset whose audio is shut: its audio first, then the
 * call. A Jabra takes two seconds to open its audio once a call is up on
 * it, and the caller's first words would go in them; this way they hear two
 * more rings instead. Three seconds at most, headset or not. */
#define ANSWER_WAIT_MS 3000
static uint64_t s_answer_at;            /* 0: no answer waiting */

static void answer(uint64_t t)
{
    if (C.call != RADIO_CALL_IN) return;
    if (bt_link_headset_connected() && !bt_link_headset_audio()) {
        if (!s_answer_at) ESP_LOGI(TAG, "answering once the headset's audio is open");
        bt_link_want_audio(true);
        if (!s_answer_at) s_answer_at = t + ANSWER_WAIT_MS;
        return;
    }
    s_answer_at = 0;
    sip_answer();
}

static void answer_due(uint64_t t)
{
    if (!s_answer_at) return;
    if (C.call != RADIO_CALL_IN) {
        s_answer_at = 0;                        /* the caller gave up */
    } else if (bt_link_headset_audio() || t >= s_answer_at) {
        s_answer_at = 0;
        sip_answer();
    }
}

/* A headset come or gone: the audio goes where it is -- only in the headset
 * while one is connected, on the jack while none is -- and in a call the
 * microphone follows: the headset's live, the knob's own muted again. A call
 * ringing in rings on both: the jack is muted only once it is a call. */
static void headset_follow(void)
{
    static int was = -1;
    const bool hs = bt_link_headset_connected();
    audio_out_dac_mute(hs && C.call != RADIO_CALL_IN);
    if ((int)hs == was) return;
    was = hs;
    if (C.call != RADIO_CALL_IDLE && C.call != RADIO_CALL_ENDED) {
        C.muted = !hs;
        ESP_LOGI(TAG, "headset %s: %s", hs ? "connected" : "gone",
                 hs ? "the call in it, its microphone live" : "the call on the jack, muted");
    }
}

static void requests(uint64_t t)
{
    headset_follow();
    answer_due(t);
    const int32_t d = __atomic_exchange_n(&s_detents, 0, __ATOMIC_RELAXED);
    if (d && (C.call == RADIO_CALL_IDLE || C.call == RADIO_CALL_ENDED)) {
        xSemaphoreTake(s_fav_mx, portMAX_DELAY);
        const int n = s_nfav;
        xSemaphoreGive(s_fav_mx);
        if (n) C.sel = ((C.sel + d) % n + n) % n;
    }
    if (s_mute_req) {
        s_mute_req = false;
        C.muted = s_mute_val;
        ESP_LOGI(TAG, "%s", C.muted ? "muted" : "unmuted");
    }
    if (s_dial_due) {
        char num[32];
        taskENTER_CRITICAL(&s_req);
        strlcpy(num, s_dial, sizeof num);
        s_dial_due = false;
        taskEXIT_CRITICAL(&s_req);
        if (!sip_call(num)) ESP_LOGW(TAG, "cannot call %s now", num);
    }
    if (s_answer_req) {
        s_answer_req = false;
        answer(t);
    }
    if (s_hangup_req) {
        s_hangup_req = false;
        sip_hangup();
    }
    if (s_toggle) {
        s_toggle = false;
        switch (C.call) {
        case RADIO_CALL_IN:  answer(t); break;
        case RADIO_CALL_OUT:
        case RADIO_CALL_UP:  sip_hangup(); break;
        default: {
            phone_fav_t f = { 0 };
            xSemaphoreTake(s_fav_mx, portMAX_DELAY);
            if (s_nfav) f = s_favs[MIN(C.sel, s_nfav - 1)];
            xSemaphoreGive(s_fav_mx);
            if (f.number[0] && !sip_call(f.number)) ESP_LOGW(TAG, "cannot call %s now", f.number);
            break;
        }
        }
    }
    (void)t;
}

static void publish(uint64_t t)
{
    char why[32] = "";
    const sip_reg_t r = sip_reg_state(why, sizeof why);
    phone_fav_t f = { 0 };
    xSemaphoreTake(s_fav_mx, portMAX_DELAY);
    const int nfav = s_nfav;
    if (nfav) f = s_favs[MIN(C.sel, nfav - 1)];
    xSemaphoreGive(s_fav_mx);
    const float rx = level_now(t);
    taskENTER_CRITICAL(&s_mux);
    P.link = r == SIP_REG_OK ? RADIO_LINK_READY : r == SIP_REG_TRYING ? RADIO_LINK_CONNECTING : RADIO_LINK_DOWN;
    strlcpy(P.link_why, r == SIP_REG_OFF ? "NO ACCOUNT" : r == SIP_REG_FAILED ? "NOT REGISTERED" : "",
            sizeof P.link_why);
    P.call = C.call;
    P.hd   = C.media.on && C.media.pt == 9;
    strlcpy(P.call_why, C.why, sizeof P.call_why);
    strlcpy(P.peer, C.peer, sizeof P.peer);
    strlcpy(P.peer_num, C.peer_num, sizeof P.peer_num);
    P.t_call = C.t_call;
    P.sel = C.sel;
    P.nfav = nfav;
    strlcpy(P.fav_name, f.name, sizeof P.fav_name);
    strlcpy(P.fav_num, f.number, sizeof P.fav_num);
    P.muted = C.muted;
    P.rx_db = rx;
    P.mic_db = C.mic_db;
    taskEXIT_CRITICAL(&s_mux);
}

static bool have_ip(void)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    return nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr;
}

static void phone_task(void *arg)
{
    (void)arg;
    const sip_events_t ev = { .on_call = on_call, .on_media = on_media };
    bool started = false;
    /* The clock, for the history's "12 min ago". */
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sc);
    for (;;) {
        const uint64_t t = now_ms();
        if (!have_ip()) {
            if (started) { sip_stop(); started = false; }
            vTaskDelay(pdMS_TO_TICKS(200));
            publish(t);
            continue;
        }
        rtp_open();
        if (!started || s_acc_dirty) {
            s_acc_dirty = false;
            sip_start(&s_acc, &ev, C.rtp_port);
            started = true;
        }
        fd_set rd;
        FD_ZERO(&rd);
        int maxfd = -1;
        const int sfd = sip_socket();
        if (sfd >= 0) { FD_SET(sfd, &rd); maxfd = MAX(maxfd, sfd); }
        if (C.rtp_fd >= 0) { FD_SET(C.rtp_fd, &rd); maxfd = MAX(maxfd, C.rtp_fd); }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 5000 };
        if (maxfd >= 0 && select(maxfd + 1, &rd, NULL, NULL, &tv) > 0) {
            if (sfd >= 0 && FD_ISSET(sfd, &rd)) sip_on_readable(now_ms());
            if (C.rtp_fd >= 0 && FD_ISSET(C.rtp_fd, &rd)) rtp_rx();
        } else if (maxfd < 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        const uint64_t t2 = now_ms();
        sip_tick(t2);
        /* On the network, and no call to disturb: no SIP account needed. */
        contacts_tick(net_prov_is_connected() && C.call == RADIO_CALL_IDLE);
        requests(t2);
        history_flush(t2);
        tx_pump(t2);
        /* The progress tones, kept topped up to the call's cushion: paced
         * by what the speaker has played, on their own clock of 20 ms a
         * frame. A timer feeding a frame every 20 ms fed a little less than
         * that, and the speaker -- and a headset fed from it -- ran dry over
         * and over: sixteen times in one ring. Kicked, as the cushion is
         * short of the speaker's pre-roll. */
        if (!C.media.on || C.call != RADIO_CALL_UP) {
            for (int i = 0; i < 8 && audio_out_queued() < AUDIO_RATE_HZ * PLAYOUT_MS / 1000; i++) {
                int16_t pcm[FRAME];
                if (!progress_tone(pcm, C.t_tone + C.tone_fed)) break;
                audio_out_feed_pcm16(pcm, FRAME, 1);
                audio_out_kick();
                C.tone_fed += 20;
            }
        }
        publish(t2);
    }
}

/* ------------------------------------------------------------- radio.h */

static void phone_init(void);
size_t radio_web_endpoints(const httpd_uri_t **out);
size_t radio_web_endpoints(const httpd_uri_t **out)
{
    phone_init();                       /* its handlers may run at once */
    *out = phone_web_uris;
    return phone_web_uris_n;
}

const char *radio_link_name(void) { return "SIP"; }

/* The settings, the history and their locks: before anything can ask for
 * them. The page's server asks for its endpoints at boot, seconds before
 * the client starts, and a request for the favourites came in between --
 * onto a lock not yet made. Both callers are app_main's, one after the
 * other. */
static void phone_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    s_fav_mx  = xSemaphoreCreateMutexStatic(&s_fav_mx_buf);
    s_hist_mx = xSemaphoreCreateMutexStatic(&s_hist_mx_buf);
    settings_load();
    history_load();
    contacts_load();
}

esp_err_t radio_start(const char *host, uint16_t port, const char *user, const char *pass)
{
    (void)host; (void)port; (void)user; (void)pass;
    static bool once;
    if (once) return ESP_OK;
    once = true;
    phone_init();
    /* Its stack in PSRAM, its TCB internal and never freed: the task never
     * writes flash (the web page's task saves the settings). 8 kB left 512
     * bytes unused with a call ringing: SIP's digests and SDP are on it. */
    static StaticTask_t tcb;
    StackType_t *stack = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (!stack) return ESP_ERR_NO_MEM;
    if (!xTaskCreateStaticPinnedToCore(phone_task, "phone", 16384, NULL, 5, stack, &tcb, 0))
        return ESP_FAIL;
    ESP_LOGI(TAG, "telephone: %s%s%s", s_acc.user[0] ? s_acc.user : "no account",
             s_acc.user[0] ? "@" : "", s_acc.domain);
    return ESP_OK;
}

int64_t radio_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz)
{
    (void)accel_mult; (void)step_hz;
    __atomic_add_fetch(&s_detents, detents, __ATOMIC_RELAXED);
    return 0;
}

void radio_set_step(int32_t step_hz)            { (void)step_hz; }
void radio_audio_suspend(bool suspend)          { (void)suspend; }
void radio_set_mode(const char *mode)           { (void)mode; }
void radio_set_filter(int32_t lo, int32_t hi)   { (void)lo; (void)hi; }
void radio_select_filter(uint8_t n)             { (void)n; }
void radio_set_rit(int32_t hz)                  { (void)hz; }
void radio_set_agc(const char *agc)             { (void)agc; }
void radio_set_gain(int8_t gain)                { (void)gain; }
void radio_goto_freq(int64_t hz)                { (void)hz; }
void radio_memory_mode(bool on)                 { (void)on; }
void radio_memory_group(uint8_t group)          { (void)group; }
void radio_select_rx(uint8_t rx)                { (void)rx; }
void radio_set_antenna(uint8_t ant, bool rx_ant) { (void)ant; (void)rx_ant; }
void radio_tune(void)     {}
void radio_atu_tune(void) {}
void radio_atu_memories(bool on) { (void)on; }
void radio_set_rf_gain(uint8_t pct)  { (void)pct; }
void radio_set_rf_power(uint8_t pct) { (void)pct; }
void radio_set_tuner(bool on)        { (void)on; }
void radio_set_squelch(uint8_t pct)  { (void)pct; }
bool radio_get_choice(uint8_t i, char *title, size_t tn, char *name, size_t nn)
{
    (void)i; (void)title; (void)tn; (void)name; (void)nn;
    return false;
}
void radio_choose(uint8_t i) { (void)i; }
int         radio_found_count(void)                       { return 0; }
bool        radio_found_get(int i, char *name, size_t cap) { (void)i; (void)name; (void)cap; return false; }
const char *radio_found_via(void)                         { return ""; }
int         radio_found_active(void)                      { return -1; }
esp_err_t   radio_found_use(int i)                        { (void)i; return ESP_ERR_NOT_SUPPORTED; }
void radio_tg_lock(bool locked) { (void)locked; }
void radio_mute(bool muted)     { s_mute_val = muted; s_mute_req = true; }

/* PTT: the call's next step -- dial, answer, hang up. Unkey only hangs up. */
void radio_ptt_key(void)    {}
void radio_ptt_unkey(void)  { s_hangup_req = true; }
void radio_ptt_toggle(void) { s_toggle = true; }
void radio_ptt_force_abort(uint8_t reason) { (void)reason; s_hangup_req = true; }

bool radio_is_ready(void) { return P.link == RADIO_LINK_READY; }
bool radio_on_air(void)   { return P.call == RADIO_CALL_UP; }

void radio_get_status(radio_status_t *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
    const uint64_t t = now_ms();
    taskENTER_CRITICAL(&s_mux);
    o->link       = P.link;
    strlcpy(o->link_why, P.link_why, sizeof o->link_why);
    o->reflector  = true;
    o->tg         = P.nfav ? (uint32_t)(P.sel + 1) : 0;
    strlcpy(o->tg_name, P.fav_name, sizeof o->tg_name);
    strlcpy(o->server, s_acc.number[0] ? s_acc.number : s_acc.user, sizeof o->server);
    o->call       = (uint8_t)P.call;
    strlcpy(o->call_why, P.call_why, sizeof o->call_why);
    o->call_ms    = P.call != RADIO_CALL_IDLE ? (uint32_t)(t - P.t_call) : 0;
    o->call_hd    = P.hd;
    strlcpy(o->peer, P.peer, sizeof o->peer);
    strlcpy(o->peer_num, P.peer_num, sizeof o->peer_num);
    strlcpy(o->fav_num, P.fav_num, sizeof o->fav_num);
    o->n_fav      = (uint8_t)P.nfav;
    o->n_missed   = s_missed;
    o->muted      = P.muted;
    o->rx_level_db = P.rx_db;
    o->smeter_dbm = P.rx_db;
    o->tx_mic_dbm = P.mic_db;
    o->permit     = PERMIT_ALL;
    o->ptt_state  = PTT_IDLE;
    o->tx         = false;
    o->n_trx      = 1;
    o->txa_sent   = P.txa_sent;
    o->txa_failed = P.txa_failed;
    taskEXIT_CRITICAL(&s_mux);
}

size_t phone_state_json(char *b, size_t cap)
{
    radio_status_t st;
    radio_get_status(&st);
    static const char *CALL[] = { "idle", "calling", "ringing", "talking", "ended" };
    sip_account_t a;
    bool has_pass;
    phone_account_get(&a, &has_pass);
    char why[32] = "";
    const sip_reg_t r = sip_reg_state(why, sizeof why);
    static const char *REG[] = { "off", "registering", "registered", "failed" };
    int n = snprintf(b, cap,
        "{\"registered\":%s,\"registration\":\"%s\",\"why\":\"%s\",\"number\":\"%s\","
        "\"call\":\"%s\",\"call_why\":\"%s\",\"peer\":\"%s\",\"peer_number\":\"%s\","
        "\"seconds\":%lu,\"muted\":%s,\"favourite\":%lu,\"favourites\":%u,\"public_ip\":\"%s\"}",
        r == SIP_REG_OK ? "true" : "false", REG[r], why, a.number,
        CALL[st.call <= RADIO_CALL_ENDED ? st.call : 0], st.call_why, st.peer, st.peer_num,
        (unsigned long)(st.call_ms / 1000), st.muted ? "true" : "false",
        (unsigned long)st.tg, (unsigned)st.n_fav, sip_public_ip());
    return n > 0 ? MIN((size_t)n, cap - 1) : 0;
}
