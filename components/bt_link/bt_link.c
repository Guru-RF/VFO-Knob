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
static volatile bool     s_spk_audio;   /* a speaker's audio is open: it plays what the jack does */
static volatile bool     s_av_full;     /* ...and its own volume is the knob's VOLUME: it is sent the
                                           knob's audio before the VOLUME (AUDIO_DN_FULL) */
static volatile uint8_t  s_av_at = 0xFF; /* ...where it says it plays, 0-127 (0xFF unsaid) */
static volatile bool     s_mic;         /* keyed: the companion sends the microphone */
static volatile bool     s_boom;        /* the boom arm is the PTT */
/* The headset's audio -- or the speaker's: open whenever it is connected, as
 * a radio's audio never stops, or, on the telephone, only while it says: its
 * calls. */
#if VFO_RADIO_PHONE
static volatile bool     s_want_audio = false;
#else
static volatile bool     s_want_audio = true;
#endif
/* What keeps this knob busy, as an update of the second chip's firmware
 * calls it when it steps aside: a telephone's calls, a radio's overs. */
#if VFO_RADIO_PHONE
#define BUSY_WHY BT_UPD_WHY_CALL
#else
#define BUSY_WHY BT_UPD_WHY_OVER
#endif

/* The second chip's own firmware, on its way (bt_link.h). All of it is
 * link_task's, from its PSRAM stack and the image in PSRAM: no flash, no
 * NVS, no wait on anything but the UART. Other tasks hand an image over
 * (s_hand), say whether it is quiet (s_allow_us), or ask for a stop
 * (s_stop_req), each under s_lock; link_task acts within one pass. */
enum { U_IDLE, U_WAIT, U_BEGIN, U_SEND, U_END, U_AFTER };
EXT_RAM_BSS_ATTR static struct {
    uint8_t         ph;             /* U_*: link_task's alone */
    uint8_t        *img;            /* PSRAM, ours to free */
    btl_upd_begin_t b;              /* its size, SHA-256, identity and version */
    char            ver[17];        /* ...the version, terminated */
    bool            forced;
    uint8_t         from_sha[8];    /* the chip's firmware at the handover */
    int64_t         quiet_since;    /* 0: not quiet now */
    int64_t         retry_at;       /* no BEGIN before */
    int64_t         t0;             /* READY: the transfer's start */
    int64_t         t_said;         /* BEGIN or END last sent, */
    int             said;           /* ...so many times */
    uint32_t        next, acked;    /* DATA: the next offset to send; the chip's next */
    int64_t         t_progress, t_back;
    uint32_t        resends;        /* times gone back to the chip's next */
    bool            ended;          /* END sent: the chip may have switched to it */
    bool            done;           /* ...and its DONE heard, or its INFO said so */
    bool            restarted;      /* END sent, and the chip restarted */
    int64_t         t_restart;
    int64_t         t_done;         /* DONE: the trial's clock */
    uint8_t         fails;          /* tries of this image that failed */
    bool            busy;           /* the busy callback, last asked */
    int64_t         t_busy, t_keep;
} U;
EXT_RAM_BSS_ATTR static bt_link_upd_t s_upd;           /* under s_lock */
EXT_RAM_BSS_ATTR static struct {                        /* under s_lock */
    uint8_t *img;
    size_t   len;
    uint8_t  sha[32];
    bool     forced;
} s_hand;
EXT_RAM_BSS_ATTR static uint8_t s_dp[BTL_MAX_PAYLOAD];  /* a DATA frame's payload */
static uint8_t           s_stop_req;                    /* under s_lock */
static int64_t           s_allow_us;                    /* under s_lock */
static bool              s_quiet;
static bool            (*s_busy_cb)(void);

/* A speaker's own volume, the knob's VOLUME (bt_link.h, bt_link_proto.h's
 * BTL_AV_*). Under s_lock: the knob's loop says its VOLUME and takes the
 * speaker's turns; link_task sends the one and hears the other. */
static struct {
    uint8_t knob;               /* the knob's VOLUME, as its loop last said; 0xFF not yet */
    uint8_t sent;               /* ...as last sent to the companion, or taken from the speaker; 0xFF none
                                   since the companion's hello */
    uint8_t take;               /* a turn of the speaker's own, as the knob's VOLUME, for its loop; 0xFF none */
    uint8_t turns;              /* the speaker's own turns, as the companion last counted them, */
    bool    known;              /* ...since this speaker came: a change of them is one */
    int64_t t_sent;             /* a VOLUME last sent, */
    int64_t t_dial;             /* ...a new one */
} s_av = { .knob = 0xFF, .sent = 0xFF, .take = 0xFF };
/* The dial turning: its latest, no oftener than this; and the same again
 * this often while a speaker takes it, for a frame lost on the wire. */
#define AV_EVERY_US 250000
#define AV_AGAIN_US 5000000
/* A turn of the speaker's own this soon after a new VOLUME went: the two
 * crossed on the wire -- the companion counted the turn before it had the
 * dial's VOLUME, which it sets after -- and the dial's stands. */
#define AV_CROSS_US 300000

static bool send(uint8_t type, const void *p, uint16_t n)
{
    static uint8_t *f;
    if (!s_tx) return false;                    /* not started: the setup firmware */
    if (!f) f = heap_caps_malloc(BTL_MAX_PAYLOAD + 7, MALLOC_CAP_SPIRAM);
    if (!f || n > BTL_MAX_PAYLOAD) return false;
    const int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s_tx, portMAX_DELAY);
    const int64_t t1 = esp_timer_get_time();
    const size_t len = btl_frame(f, type, p, n);
    const int w = uart_write_bytes(LINK_UART, f, len);
    xSemaphoreGive(s_tx);
    /* A send held long: waiting for the lock, or for the UART's ring. */
    const int64_t t2 = esp_timer_get_time();
    static int64_t t_said;
    if (t2 - t0 > 40000 && t2 - t_said > 10000000) {
        t_said = t2;
        ESP_LOGW(TAG, "a send to the second chip held %lld ms: %lld for the lock, %lld writing",
                 (long long)((t2 - t0) / 1000), (long long)((t1 - t0) / 1000), (long long)((t2 - t1) / 1000));
    }
    return w == (int)len;
}

/* An update of the second chip going steps aside now, for this (bt_link.h's
 * whys). From any task; link_task acts on it within one pass. */
static void upd_stop_req(uint8_t why)
{
    taskENTER_CRITICAL(&s_lock);
    s_stop_req = why;
    taskEXIT_CRITICAL(&s_lock);
}

static void hello(bool ask)
{
    static const char v[] = "knob";
    uint8_t p[2 + sizeof v];
    p[0] = BTL_PROTO;
    /* SPEAKERS: a speaker connected, this knob keys its own microphone and
     * takes none of the speaker's buttons -- so the companion may play to
     * one. Without it, every device is a headset to this knob. AV_VOLUME:
     * it sends a speaker that takes it its VOLUME, and its audio before it. */
    p[1] = (ask ? BTL_HELLO_ASK : 0) | BTL_HELLO_SPEAKERS | BTL_HELLO_AV_VOLUME;
    memcpy(p + 2, v, sizeof v - 1);
    send(BTL_HELLO, p, sizeof p - 1);
}

/* What the knob wants of the companion: after each hello, as either side
 * may have started afresh. */
static void send_audio(void)
{
    uint8_t a[9];
    const uint32_t dn = AUDIO_RATE_HZ, up = TX_AUDIO_RATE_HZ;
    a[0] = s_want_audio;
    memcpy(a + 1, &dn, 4);
    memcpy(a + 5, &up, 4);
    send(BTL_CMD_AUDIO, a, sizeof a);
}

static void push_config(void)
{
    send_audio();
    const uint8_t m = s_mic;
    send(BTL_CMD_MIC, &m, 1);
    send(BTL_CMD_STATE, NULL, 0);
}

/* The playback task's: everything the jack plays, to the headset's ear --
 * or the speaker's -- in frames of 10 ms: a frame lost on the wire is then
 * 10 ms, not 21. Given before the VOLUME (audio_out.h), it applies it --
 * but for a speaker whose own volume is the VOLUME. */
#define DN_FRAME 240
static void tap(const int16_t *stereo, size_t frames, uint8_t volume)
{
    /* What the headset is given, measured every 10 s: its chip runs dry
     * when this falls short of the knob's rate, or comes in lumps. */
    static int64_t  t0, t_last;
    static uint32_t sent, pause_max;
    /* The level sent, in thousandths, and its swell into full level (below):
     * each stream starts from silence, as the first after a start does --
     * else one that opens at full level jumps there. */
    static int32_t g;
    static bool    was, swell;
    const bool spk = s_spk_audio;
    if (!s_hs_audio && !spk) {
        t0 = t_last = 0;
        sent = pause_max = 0;
        g = 0;
        was = swell = false;
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (!t0 || (t_last && now - t_last > 1000000)) {      /* a new stream */
        t0 = now;
        sent = pause_max = 0;
        t_last = 0;
    }
    if (t_last && (uint32_t)((now - t_last) / 1000) > pause_max) pause_max = (uint32_t)((now - t_last) / 1000);
    t_last = now;
    sent += frames;
    if (now - t0 >= 10000000) {
        const double s = (double)(now - t0) / 1e6;
        ESP_LOGI(TAG, "%s fed %lu samples in %.2f s: %+.0f ppm of %u Hz, longest pause %lu ms",
                 spk ? "speaker" : "headset", (unsigned long)sent, s,
                 ((double)sent / s / AUDIO_RATE_HZ - 1.0) * 1e6,
                 (unsigned)AUDIO_RATE_HZ, (unsigned long)pause_max);
        t0 = now;
        sent = pause_max = 0;
    }
    /* A speaker beside the knob's own microphone: silent while that is keyed,
     * or the over would carry it. The over itself, not the companion's
     * microphone (s_mic), which a headset gone in the middle of one turns
     * off. Not the telephone's: its call goes both ways at once, and its
     * speakerphone holds the microphone back instead. */
#if VFO_RADIO_PHONE
    const bool hush = false;
#else
    const bool hush = spk && audio_in_active();
#endif
    /* A speaker whose own volume is the knob's VOLUME turns the audio down
     * itself: it gets it at full level, marked so -- the companion plays
     * that to such a speaker only. Less, in proportion, what it says it
     * plays above the VOLUME asked: a step of its own, a set not answered
     * yet -- the dial just turned down is heard at once. All else gets the
     * jack's loudness. A VOLUME of 0 is silence on any speaker, whatever its
     * own 0 is. And a speaker a quarter of all that, 12 dB down: it is a
     * loudspeaker, and what the VOLUME gives the jack's earphones filled the
     * room from one at 2 (the JLab, 2026-10-05). In thousandths of full
     * scale. */
    const bool full = spk && s_av_full, loud = full && volume;
    int32_t    want = (int32_t)volume * 10;
    if (loud) {
        const uint8_t asked = btl_av_from_knob(volume), at = s_av_at;
        want = at > asked && at <= 127 ? 1000 * asked / at : 1000;
    }
    if (spk) want /= 4;
    /* Into full level -- the switch to it, or up from a VOLUME of 0 -- no
     * faster than some 27 dB a second; anything else at once: it swells,
     * never jumps, and a speaker that took the knob's VOLUME in word only is
     * heard coming. */
    if (loud && !was) swell = true;
    was = loud;
    EXT_RAM_BSS_ATTR static int16_t mono[DN_FRAME];
    while (frames) {
        const size_t n = frames > DN_FRAME ? DN_FRAME : frames;
        if (!swell || want <= g) {
            g     = want;
            swell = false;
        } else if ((g += g / 32 + 1) >= want) {
            g     = want;
            swell = false;
        }
        for (size_t i = 0; i < n; i++)
            mono[i] = hush ? 0 : (int16_t)((((int32_t)stereo[2 * i] + stereo[2 * i + 1]) / 2) * g / 1000);
        send(full ? BTL_AUDIO_DN_FULL : BTL_AUDIO_DN, mono, (uint16_t)(n * 2));
        stereo += 2 * n;
        frames -= n;
    }
}

/* audio_in's: keying started or stopped -- the headset's microphone only then. */
static void mic_hook(bool active)
{
    s_mic = active;
    /* An over -- on the telephone, a call -- comes first: an update of the
     * second chip going stops. */
    if (active) upd_stop_req(BUSY_WHY);
    const uint8_t m = active;
    send(BTL_CMD_MIC, &m, 1);
}

static const char *link_name(uint8_t l)
{
    return l == BTL_LINK_CONNECTED ? "connected" : l == BTL_LINK_CONNECTING ? "connecting" : "idle";
}

static const char *audio_name(uint8_t a)
{
    return a == BTL_AUDIO_SBC_44K    ? ", audio SBC 44.1 kHz"
           : a == BTL_AUDIO_MSBC_16K ? ", audio mSBC 16 kHz"
           : a == BTL_AUDIO_CVSD_8K  ? ", audio CVSD 8 kHz" : "";
}

/* While a headset is connected the knob's own microphone is off: keying
 * takes the headset's, or -- its audio not open yet -- silence, never the
 * knob's across the room. A speaker leaves it on: keying takes the knob's
 * own, as with nothing connected, and the speaker only listens. */
static void device(const btl_state_t *s)
{
    const bool conn = s->link == BTL_LINK_CONNECTED, open = s->audio != BTL_AUDIO_NONE;
    const bool spk  = s->kind == BTL_KIND_SPEAKER;
    /* Full level only to a speaker whose own volume the companion says is
     * the knob's -- and off before anything else is on: the tap reads them
     * as it goes. Where it plays first, for the tap's sums. */
    const bool full = conn && spk && open && (s->av & BTL_AV_SET);
    s_av_at     = full ? s->av_volume : 0xFF;
    s_av_full   = full;
    s_hs_audio  = conn && !spk && open;
    s_spk_audio = conn && spk && open;
    if ((conn && !spk) == s_hs_conn) return;
    s_hs_conn = conn && !spk;
    audio_in_use_ext(s_hs_conn);
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
    const bool spk = s.kind == BTL_KIND_SPEAKER;
    if (s.link != was.link || s.audio != was.audio || s.kind != was.kind)
        ESP_LOGI(TAG, "%s %s: %s%s", spk ? "speaker" : "headset", s.name[0] ? s.name : "-",
                 link_name(s.link), audio_name(s.audio));
    /* How far behind the jack a speaker plays: the telephone holds its
     * microphone back that much longer after the far end. */
    if (spk && s.audio == BTL_AUDIO_SBC_44K && s.delay_ms && s.delay_ms != was.delay_ms)
        ESP_LOGI(TAG, "speaker %s: %u ms behind the jack", s.name[0] ? s.name : "-", (unsigned)s.delay_ms);
    device(&s);
    const bool hs     = s.link == BTL_LINK_CONNECTED && !spk;
    const bool hs_was = was.link == BTL_LINK_CONNECTED && was.kind != BTL_KIND_SPEAKER;
    if (hs && (s.mic == 0) != (hs_was && was.mic == 0))
        ESP_LOGI(TAG, "headset microphone %s", s.mic == 0 ? "muted" : "live");
    /* A speaker's own volume. While it takes the knob's, a turn of its own
     * -- the companion counts them -- comes back as the knob's VOLUME, and
     * is not sent back to it; unless it crossed a new VOLUME of the dial's
     * on the wire, which the companion sets after it. The companion keeps
     * the knob's VOLUME for the next speaker that takes it. */
    const bool takes       = s.link == BTL_LINK_CONNECTED && spk && (s.av & BTL_AV_TAKES);
    const bool set         = takes && (s.av & BTL_AV_SET);
    const bool refused     = takes && (s.av & BTL_AV_REFUSED);
    const bool takes_was   = was.link == BTL_LINK_CONNECTED && was.kind == BTL_KIND_SPEAKER && (was.av & BTL_AV_TAKES);
    const bool set_was     = takes_was && (was.av & BTL_AV_SET);
    const bool refused_was = takes_was && (was.av & BTL_AV_REFUSED);
    const int64_t now      = esp_timer_get_time();
    uint8_t own = 0xFF;
    bool crossed = false;
    taskENTER_CRITICAL(&s_lock);
    if (!takes) {
        s_av.take  = 0xFF;
        s_av.known = false;
    } else {
        if (set && s_av.known && s.av_turns != s_av.turns && s.av_volume <= 127) {
            if (now - s_av.t_dial >= AV_CROSS_US) {
                own = btl_av_to_knob(s.av_volume);
                s_av.take = own;
                s_av.sent = own;        /* where it is now */
            } else {
                crossed = true;
            }
        }
        s_av.turns = s.av_turns;
        s_av.known = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    const char *sn = s.name[0] ? s.name : "-";
    if (takes && !takes_was) ESP_LOGI(TAG, "speaker %s takes its volume from the knob", sn);
    if (set != set_was && (set || (s.link == BTL_LINK_CONNECTED && spk)))
        ESP_LOGI(TAG, "speaker %s: %s", sn,
                 set ? "its own volume is the knob's VOLUME -- its audio goes at full level"
                     : "its audio at the knob's VOLUME again");
    if (refused && !refused_was)
        ESP_LOGI(TAG, "speaker %s did not take the knob's VOLUME: it keeps its own, the knob scales its audio", sn);
    if (own != 0xFF)
        ESP_LOGI(TAG, "speaker %s turned itself to %u of 127: the knob's VOLUME %u", sn, (unsigned)s.av_volume,
                 (unsigned)own);
    if (crossed)
        ESP_LOGI(TAG, "speaker %s turned itself to %u of 127 as the dial turned: the dial's VOLUME stands", sn,
                 (unsigned)s.av_volume);
    /* A headset or a speaker coming, there, or looked for, while an update
     * goes over: it comes first. The chip calls none meanwhile, so this is
     * one coming of its own -- switched on -- or the page's scan. */
    if ((s.link != BTL_LINK_IDLE || s.scanning || s.audio != BTL_AUDIO_NONE) &&
        (U.ph == U_BEGIN || U.ph == U_SEND || U.ph == U_END))
        upd_stop_req(BTL_UPD_WHY_HEADSET);
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

/* ---- the second chip's own firmware, on its way ------------------------------
 *
 * WAIT holds the image until it has been quiet for two minutes on end; BEGIN
 * asks the chip to take it; SEND streams it, at most BTL_UPD_WINDOW frames
 * unanswered, going back to the chip's next when one is lost; END has it
 * checked and switched to, and AFTER waits for the chip, restarted, to say
 * how it went: kept, or gone back to the firmware before. A stop before the
 * switch costs nothing but the time: the chip is as it was, and the image is
 * held for the next quiet moment. */

/* Where an image says what it is: its esp_app_desc_t follows the image
 * header (24 bytes) and its first segment's (8). */
#define IMG_VERSION      0x30           /* esp_app_desc_t.version */
#define IMG_APP_SHA      0xB0           /* esp_app_desc_t.app_elf_sha256 */

#define QUIET_US         120000000LL    /* quiet this long without a break: it goes */
#define HEADSET_AGAIN_US 10000000LL     /* a headset in the way that never connected: again so soon */
#define BEGIN_AGAIN_US   2000000LL      /* BEGIN unanswered: said again -- */
#define END_AGAIN_US     3000000LL      /* END too; the chip's check takes about a second -- */
#define SAY_TIMES        5              /* ...so many times in all */
#define GO_BACK_US       1000000LL      /* no progress: from the chip's next again */
#define STALL_US         30000000LL     /* ...none for this long: given up */
#define RESTART_US       15000000LL     /* END, then the chip restarted: what it runs, said within */
#define AFTER_US         180000000LL    /* DONE: how it went, said within -- the trial is 2 minutes */
#define ALLOW_US         10000000LL     /* the caller's say-so, stale after */
#define BUSY_EVERY_US    100000LL
#define KEEP_EVERY_US    1000000LL
#define FAILS            3              /* tries of one image that failed: then it is let go */

static const char *hex8(char out[17], const uint8_t *b)
{
    for (int i = 0; i < 8; i++) sprintf(out + 2 * i, "%02x", b[i]);
    return out;
}

/* The chip's refusals and failures, and the knob's own whys, in words. */
static const char *why_words(uint8_t w)
{
    switch (w) {
    case BTL_UPD_WHY_HEADSET:    return "a headset or a speaker";
    case BTL_UPD_WHY_TRIAL:      return "the firmware it runs is on trial";
    case BTL_UPD_WHY_BUSY:       return "another image is coming in";
    case BTL_UPD_WHY_SIZE:       return "its size";
    case BTL_UPD_WHY_BEFORE:     return "it went back here before";
    case BTL_UPD_WHY_BOOTLOADER: return "its bootloader cannot go back -- it needs the bench once";
    case BTL_UPD_WHY_FLASH:      return "the flash";
    case BTL_UPD_WHY_SHA:        return "its bytes are not the SHA-256 sent";
    case BTL_UPD_WHY_IMAGE:      return "the image did not verify";
    case BTL_UPD_WHY_PROJECT:    return "not a second-chip firmware";
    case BTL_UPD_WHY_QUIET:      return "nothing reached it for 15 s";
    case BTL_UPD_WHY_KNOB:       return "the knob's own update";
    case BTL_UPD_WHY_MEMORY:     return "no memory for it";
    case BTL_UPD_WHY_NO_SESSION: return "it had no update going";
    case BTL_UPD_WHY_RESTARTED:  return "it restarted";
    case BT_UPD_WHY_OVER:        return "an over";
    case BT_UPD_WHY_CALL:        return "a call";
    case BT_UPD_WHY_PAGE:        return "the Bluetooth page";
    case BT_UPD_WHY_HELD:        return "the knob was busy";
    case BT_UPD_WHY_GONE:        return "the chip went quiet";
    case BT_UPD_WHY_NO_ANSWER:   return "no answer";
    case BT_UPD_WHY_STALLED:     return "it stopped taking it";
    default:                     return "?";
    }
}

static const char *back_words(uint8_t c)
{
    switch (c) {
    case BTL_BACK_POWER:   return "the power went before it was kept";
    case BTL_BACK_QUIET:   return "no knob kept it within 2 minutes";
    case BTL_BACK_CRASHED: return "it crashed on trial";
    case BTL_BACK_HUNG:    return "it hung on trial";
    case BTL_BACK_EARLY:   return "it stopped before its trial began";
    case BTL_BACK_GUARD:   return "it crashed 3 times in a row after it was kept";
    default:               return "?";
    }
}

/* Gone back from for one of these, an image is never sent again -- nor
 * would the chip take it. */
static bool back_real(uint8_t c)
{
    return c == BTL_BACK_CRASHED || c == BTL_BACK_HUNG || c == BTL_BACK_EARLY || c == BTL_BACK_GUARD;
}

/* Results no retry mends: that image never goes to the chip again. */
static bool blocks(uint8_t result, uint8_t why)
{
    switch (result) {
    case BT_UPD_WENT_BACK: return back_real(why);
    case BT_UPD_FAILED:    return why == BTL_UPD_WHY_IMAGE || why == BTL_UPD_WHY_PROJECT || why == BTL_UPD_WHY_SIZE;
    case BT_UPD_REFUSED:   return why == BTL_UPD_WHY_BEFORE || why == BTL_UPD_WHY_SIZE || why == BTL_UPD_WHY_PROJECT;
    default:               return false;
    }
}

/* An ABORT's why, as the chip knows them. */
static uint8_t wire_why(uint8_t why)
{
    switch (why) {
    case BT_UPD_WHY_OVER:
    case BT_UPD_WHY_CALL:
    case BT_UPD_WHY_HELD:     return BTL_UPD_WHY_BUSY;
    case BT_UPD_WHY_PAGE:
    case BTL_UPD_WHY_HEADSET: return BTL_UPD_WHY_HEADSET;
    case BTL_UPD_WHY_KNOB:    return BTL_UPD_WHY_KNOB;
    default:                  return BTL_UPD_WHY_NONE;
    }
}

static void upd_abort(uint8_t why)
{
    const uint8_t w = wire_why(why);
    send(BTL_UPD_ABORT, &w, 1);
}

/* For bt_link_update_status(): what the update is doing. */
static void pub(uint8_t phase, uint8_t pct)
{
    taskENTER_CRITICAL(&s_lock);
    s_upd.phase   = phase;
    s_upd.percent = pct;
    taskEXIT_CRITICAL(&s_lock);
}

/* ...and how the last try ended. */
static void result(uint8_t res, uint8_t why, const char *text)
{
    const bool block = blocks(res, why);
    taskENTER_CRITICAL(&s_lock);
    s_upd.result = res;
    s_upd.why    = why;
    s_upd.block  = block;
    memcpy(s_upd.last_sha, U.b.app_sha, sizeof s_upd.last_sha);
    strlcpy(s_upd.text, text, sizeof s_upd.text);
    s_upd.seq++;
    taskEXIT_CRITICAL(&s_lock);
}

/* The image done with: kept, gone back from, refused for good, or no longer
 * the one the chip wants. */
static void let_go(void)
{
    free(U.img);
    U.img = NULL;
    U.ph  = U_IDLE;
    pub(BT_UPD_IDLE, 0);
}

/* Held for the next quiet moment. A headset or speaker in the way that
 * never connected -- a remembered one, switched off, which the chip calls
 * every minute for 5 s -- leaves the quiet as it was, and BEGIN goes again
 * soon; anything else starts the two minutes over. */
static void again(uint8_t why, int64_t now)
{
    U.ph = U_WAIT;
    if ((why == BTL_UPD_WHY_HEADSET || why == BT_UPD_WHY_PAGE) && s_st.hs.link != BTL_LINK_CONNECTED)
        U.retry_at = now + HEADSET_AGAIN_US;
    else
        U.quiet_since = 0;
    pub(BT_UPD_WAITING, 0);
}

/* Stopped before the chip switched to it: nothing changed there. Not a
 * try. */
static void stopped(uint8_t why, bool abort, int64_t now)
{
    if (abort) upd_abort(why);
    char t[96];
    snprintf(t, sizeof t, "update to %s stopped: %s; again at a quiet moment", U.ver, why_words(why));
    ESP_LOGI(TAG, "second chip: %s", t);
    result(BT_UPD_STOPPED, why, t);
    again(why, now);
}

/* A try that ended with the chip as it was, or with no word of how it went:
 * held for the next quiet moment -- unless it can never go (blocks()), the
 * chip takes none, or it has failed FAILS times. */
static void ended(uint8_t res, uint8_t why, const char *t, int64_t now)
{
    ESP_LOGW(TAG, "second chip: %s", t);
    result(res, why, t);
    if (blocks(res, why) || (res == BT_UPD_REFUSED && why == BTL_UPD_WHY_BOOTLOADER)) {
        let_go();
    } else if (++U.fails >= FAILS) {
        ESP_LOGW(TAG, "second chip: %s let go after %d tries; handed over again, it goes again", U.ver, FAILS);
        let_go();
    } else {
        again(why, now);
    }
}

static void failed(uint8_t res, uint8_t why, int32_t err, int64_t now)
{
    char t[96];
    const char *what = res == BT_UPD_REFUSED ? "refused" : "failed";
    const char *then = blocks(res, why) ? " -- not sent again" : "";
    if (err) snprintf(t, sizeof t, "%s %s: %s (%s)%s", U.ver, what, why_words(why), esp_err_to_name(err), then);
    else     snprintf(t, sizeof t, "%s %s: %s%s", U.ver, what, why_words(why), then);
    ended(res, why, t, now);
}

/* BEGIN refused. A headset, a trial or another image in the way: not now,
 * and not a try. */
static void refused(uint8_t why, int64_t now)
{
    if (why != BTL_UPD_WHY_HEADSET && why != BTL_UPD_WHY_TRIAL && why != BTL_UPD_WHY_BUSY) {
        failed(BT_UPD_REFUSED, why, 0, now);
        return;
    }
    char t[96];
    snprintf(t, sizeof t, "%s not taken now: %s; again at a quiet moment", U.ver, why_words(why));
    ESP_LOGI(TAG, "second chip: %s", t);
    result(BT_UPD_REFUSED, why, t);
    again(why, now);
}

/* The chip took it and switched to it: its DONE -- or, that lost, the word
 * of the firmware it restarted into. Each one a try of that image. */
static void switched(int64_t now)
{
    if (!U.done) {
        U.done = true;
        taskENTER_CRITICAL(&s_lock);
        s_upd.dones++;
        taskEXIT_CRITICAL(&s_lock);
    }
    U.ph     = U_AFTER;
    U.t_done = now;
    pub(BT_UPD_RESTARTING, 100);
}

/* The lines the release gate looks for (tools/release.sh): which firmware
 * went to which, by their identities. */
static void kept(void)
{
    char a[17], z[17], t[96];
    ESP_LOGI(TAG, "second chip: update from %s [%s] to %s [%s]: kept", s_upd.from, hex8(a, U.from_sha), U.ver,
             hex8(z, U.b.app_sha));
    snprintf(t, sizeof t, "updated from %s to %s", s_upd.from, U.ver);
    result(BT_UPD_KEPT, BTL_UPD_WHY_NONE, t);
    let_go();
}

static void went_back(uint8_t cls)
{
    char a[17], z[17], t[96];
    const char *then = back_real(cls) ? "not sent again" : "it may come again";
    ESP_LOGW(TAG, "second chip: update from %s [%s] to %s [%s]: went back (%s): %s", s_upd.from,
             hex8(a, U.from_sha), U.ver, hex8(z, U.b.app_sha), back_words(cls), then);
    snprintf(t, sizeof t, "%s went back: %s -- %s", U.ver, back_words(cls), then);
    result(BT_UPD_WENT_BACK, cls, t);
    let_go();
}

/* Handed over for the chip as it was then, by a caller who looked: the
 * chip changed since -- a cable, a firmware gone back -- and the reasons
 * are gone. Let go: not a result of the chip's. */
static void dropped(const char *why)
{
    char t[96];
    snprintf(t, sizeof t, "%s not sent: %s", U.ver, why);
    ESP_LOGW(TAG, "second chip: %s", t);
    result(BT_UPD_NONE, BTL_UPD_WHY_NONE, t);
    let_go();
}

/* Still the image to send, by what the chip last said of itself (WAIT)? An
 * image it went back from for real it refuses; one handed over for a
 * release, unforced, is not for a development build, nor for a firmware
 * other than the one it was meant to replace. */
static void premise(const btl_upd_info_t *i, int64_t now)
{
    if (back_real(i->back) && !memcmp(i->back_sha, U.b.app_sha, 8))
        failed(BT_UPD_REFUSED, BTL_UPD_WHY_BEFORE, 0, now);
    else if (!(s_st.flags & BTL_HELLO_UPDATE))
        dropped("the second chip takes no updates now");
    else if (!U.forced && !(i->flags & BTL_INFO_RELEASE))
        dropped("the second chip runs a development build now");
    else if (!U.forced && memcmp(i->app_sha, U.from_sha, 8))
        dropped("the second chip runs another firmware now");
}

/* After its restart: what the chip runs says how it went. Kept only as
 * otadata has it -- VALID: a firmware that runs with no keep behind it
 * (started with nothing else to start) is no kept one, for the record nor
 * for the release gate's line; no word in 3 minutes ends that try UNKNOWN. */
static void after_info(const btl_upd_info_t *i)
{
    if (!memcmp(i->app_sha, U.b.app_sha, 8)) {
        if (i->state == BTL_RUN_TRIAL)      pub(BT_UPD_TRIAL, 100);    /* KEEP is on its way (on_info) */
        else if (i->state == BTL_RUN_VALID) kept();
    } else if (i->back != BTL_BACK_NONE && !memcmp(i->back_sha, U.b.app_sha, 8)) {
        went_back(i->back);
    }
    /* Anything else: the firmware before, with nothing said of this one yet. */
}

/* UPD_INFO: what the chip runs, and what went back. */
static void on_info(const uint8_t *p, uint16_t n)
{
    if (n < sizeof(btl_upd_info_t)) return;     /* a later chip's may be longer: these come first */
    btl_upd_info_t i;
    memcpy(&i, p, sizeof i);
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    s_upd.chip = i;
    s_upd.info = true;
    taskEXIT_CRITICAL(&s_lock);
    /* On trial, the chip goes back by itself two minutes after its start
     * unless a knob says KEEP: this one says so whatever it is doing, so any
     * firmware of the knob's with this sender keeps a chip it can hear. */
    if (i.state == BTL_RUN_TRIAL && now - U.t_keep >= KEEP_EVERY_US) {
        U.t_keep = now;
        send(BTL_UPD_KEEP, NULL, 0);
    }
    if (U.ph == U_IDLE) return;
    const bool ours      = !memcmp(i.app_sha, U.b.app_sha, 8);
    const bool ours_back = i.back != BTL_BACK_NONE && !memcmp(i.back_sha, U.b.app_sha, 8);
    switch (U.ph) {
    case U_AFTER:
        after_info(&i);
        break;
    case U_END:
        /* Restarted into it, its DONE lost -- or into it and back already. */
        if (ours || (U.restarted && ours_back)) {
            switched(now);
            after_info(&i);
        } else if (U.restarted) {
            stopped(BTL_UPD_WHY_RESTARTED, false, now);
        }
        break;
    default:                                    /* WAIT, BEGIN, SEND */
        if (U.ended && (ours || ours_back)) {
            /* An END before this went through after all (stopped while the
             * chip checked it). */
            if (U.ph != U_WAIT) upd_abort(BTL_UPD_WHY_NONE);
            switched(now);
            after_info(&i);
        } else if (ours) {
            if (U.ph != U_WAIT) upd_abort(BTL_UPD_WHY_NONE);
            dropped("the second chip runs it already");
        } else if (U.ph == U_WAIT) {
            premise(&i, now);
        }
        break;
    }
}

/* The chip's ACK on its way: progress. */
static void progress(uint32_t next, int64_t now)
{
    if (next <= U.acked || next > U.b.size) return;
    U.acked = next;
    if (U.next < next) U.next = next;           /* an answer slow to come, after a go-back */
    U.t_progress = now;
    pub(BT_UPD_SENDING, (uint8_t)((uint64_t)next * 100 / U.b.size));
}

/* UPD_STATUS: the chip's answer to BEGIN, DATA and END. Any other moment's
 * is an answer to frames of a try already over. */
static void on_status(const uint8_t *p, uint16_t n)
{
    if (n < sizeof(btl_upd_status_t)) return;
    btl_upd_status_t s;
    memcpy(&s, p, sizeof s);
    const int64_t now = esp_timer_get_time();
    switch (U.ph) {
    case U_BEGIN:
        if (s.state == BTL_UPD_READY || s.state == BTL_UPD_ACK) {
            U.ph       = U_SEND;
            U.next     = U.acked = s.state == BTL_UPD_ACK && s.next <= U.b.size ? s.next : 0;
            U.t0       = U.t_progress = U.t_back = now;
            U.resends  = 0;
            pub(BT_UPD_SENDING, (uint8_t)((uint64_t)U.acked * 100 / U.b.size));
            ESP_LOGI(TAG, "second chip: sending %s (%lu bytes) to replace %s", U.ver, (unsigned long)U.b.size,
                     s_upd.from);
        } else if (s.state == BTL_UPD_REFUSED) {
            refused(s.why, now);
        } else if (s.state == BTL_UPD_FAILED) {
            failed(BT_UPD_FAILED, s.why, s.err, now);
        } else if (s.state == BTL_UPD_STOPPED) {
            stopped(s.why, false, now);
        }
        break;
    case U_SEND:
        if (s.state == BTL_UPD_ACK)          progress(s.next, now);
        else if (s.state == BTL_UPD_FAILED)  failed(BT_UPD_FAILED, s.why, s.err, now);
        else if (s.state == BTL_UPD_STOPPED) stopped(s.why, false, now);
        break;
    case U_END:
        if (U.restarted) break;                 /* restarted: its INFO says (on_info) */
        if (s.state == BTL_UPD_DONE)         switched(now);
        else if (s.state == BTL_UPD_FAILED)  failed(BT_UPD_FAILED, s.why, s.err, now);
        else if (s.state == BTL_UPD_STOPPED) stopped(s.why, false, now);
        break;
    default:
        break;
    }
}

/* The chip restarted -- its HELLO, asking: a transfer it had is gone. After
 * END it may have restarted into the image, its DONE lost: its INFO says. */
static void upd_chip_started(int64_t now)
{
    if (U.ph == U_BEGIN || U.ph == U_SEND) {
        stopped(BTL_UPD_WHY_RESTARTED, false, now);
    } else if (U.ph == U_END && !U.restarted) {
        U.restarted = true;
        U.t_restart = now;
    }
}

/* An image handed over (bt_link_update_start), taken in: from here on
 * held, the page's phase with it. */
static void take(void)
{
    char from[17];
    taskENTER_CRITICAL(&s_lock);
    uint8_t     *img    = s_hand.img;
    const size_t len    = s_hand.len;
    const bool   forced = s_hand.forced;
    if (img) {
        memcpy(U.b.sha256, s_hand.sha, sizeof U.b.sha256);
        memcpy(U.b.app_sha, img + IMG_APP_SHA, sizeof U.b.app_sha);
        memset(U.b.version, 0, sizeof U.b.version);
        memcpy(U.b.version, img + IMG_VERSION, strnlen((const char *)img + IMG_VERSION, sizeof U.b.version));
        memcpy(U.from_sha, s_upd.chip.app_sha, sizeof U.from_sha);
        s_upd.phase   = BT_UPD_WAITING;
        s_upd.percent = 0;
        memcpy(s_upd.to, U.b.version, sizeof U.b.version);
        s_upd.to[sizeof U.b.version] = 0;
        memcpy(s_upd.to_sha, U.b.app_sha, sizeof s_upd.to_sha);
        strlcpy(s_upd.from, s_st.version, sizeof s_upd.from);
        s_upd.forced = forced;
        strlcpy(from, s_upd.from, sizeof from);
    }
    s_hand.img = NULL;
    taskEXIT_CRITICAL(&s_lock);
    if (!img) return;
    U.img    = img;
    U.forced = forced;
    U.b.size = (uint32_t)len;
    memcpy(U.ver, U.b.version, sizeof U.b.version);
    U.ver[sizeof U.b.version] = 0;
    U.quiet_since = U.retry_at = 0;
    U.ended = U.done = U.restarted = false;
    U.fails  = 0;
    U.busy   = false;
    U.t_busy = 0;
    U.ph     = U_WAIT;
    char a[17], z[17];
    ESP_LOGI(TAG, "second chip: %s [%s], %lu bytes%s, held to replace %s [%s] at a quiet moment", U.ver,
             hex8(a, U.b.app_sha), (unsigned long)len, forced ? ", forced" : "", from, hex8(z, U.from_sha));
    /* The caller looked at the chip; this is the chip as it is now. */
    if (!memcmp(s_upd.chip.app_sha, U.b.app_sha, 8)) dropped("the second chip runs it already");
    else                                             premise(&s_upd.chip, esp_timer_get_time());
}

static void begin(int64_t now)
{
    U.ph        = U_BEGIN;
    U.said      = 1;
    U.t_said    = now;
    U.done      = false;
    U.restarted = false;
    pub(BT_UPD_SENDING, 0);
    send(BTL_UPD_BEGIN, &U.b, sizeof U.b);
}

static void wait_pass(int64_t now, uint8_t stop, bool allow)
{
    const bool headset_stop = stop == BTL_UPD_WHY_HEADSET || stop == BT_UPD_WHY_PAGE;
    if (headset_stop) U.retry_at = now + HEADSET_AGAIN_US;
    /* Quiet: the caller says so, no over or call, the chip there and ready
     * for it, no headset or speaker connected, none looked for. One the chip
     * only calls breaks nothing: BEGIN waits for the call to end. */
    const bool quiet = allow && (!stop || headset_stop) && !U.busy && s_st.companion &&
                       (s_st.flags & BTL_HELLO_UPDATE) && s_upd.info && s_upd.chip.state != BTL_RUN_TRIAL &&
                       (U.forced || (s_upd.chip.flags & BTL_INFO_RELEASE)) &&
                       s_st.hs.link != BTL_LINK_CONNECTED && !s_st.hs.scanning && s_st.hs.audio == BTL_AUDIO_NONE;
    if (!quiet) {
        U.quiet_since = 0;
        return;
    }
    if (!U.quiet_since) U.quiet_since = now;
    if (now - U.quiet_since < QUIET_US || now < U.retry_at || s_st.hs.link != BTL_LINK_IDLE) return;
    begin(now);
}

static void begin_pass(int64_t now)
{
    if (now - U.t_said < BEGIN_AGAIN_US) return;
    if (U.said >= SAY_TIMES) {
        upd_abort(BTL_UPD_WHY_NONE);            /* in case it opened one, its answers lost */
        failed(BT_UPD_FAILED, BT_UPD_WHY_NO_ANSWER, 0, now);
        return;
    }
    U.said++;
    U.t_said = now;
    send(BTL_UPD_BEGIN, &U.b, sizeof U.b);
}

static void send_pass(int64_t now)
{
    if (U.acked >= U.b.size) {
        /* With what this task's 4 kB stack (PSRAM) never used: the sender's
         * lines and words go deeper than the link's own did. */
        const int64_t ms = (now - U.t0) / 1000;
        ESP_LOGI(TAG, "second chip: it took all %lu bytes in %lu.%lu s (%lu resend%s); it checks the image "
                      "and restarts; link stack %u bytes never used", (unsigned long)U.b.size,
                 (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 100), (unsigned long)U.resends,
                 U.resends == 1 ? "" : "s", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        U.ph     = U_END;
        U.ended  = true;
        U.said   = 1;
        U.t_said = now;
        pub(BT_UPD_CHECKING, 100);
        const uint32_t size = U.b.size;
        send(BTL_UPD_END, &size, sizeof size);
        return;
    }
    if (now - U.t_progress >= STALL_US) {
        upd_abort(BTL_UPD_WHY_NONE);
        failed(BT_UPD_FAILED, BT_UPD_WHY_STALLED, 0, now);
        return;
    }
    /* A frame lost on the wire: the chip waits for it, answering what came
     * after with its next. From there again. */
    if (U.next > U.acked && now - U.t_progress >= GO_BACK_US && now - U.t_back >= GO_BACK_US) {
        U.next   = U.acked;
        U.t_back = now;
        U.resends++;
    }
    while (U.next < U.b.size && U.next - U.acked < BTL_UPD_WINDOW * BTL_UPD_CHUNK) {
        /* Into the UART's ring only what fits now: never a wait holding s_tx,
         * which the microphone's hook and the page's buttons take too. */
        size_t room = 0;
        if (uart_get_tx_buffer_free_size(LINK_UART, &room) != ESP_OK || room < 7 + 4 + BTL_UPD_CHUNK) break;
        const uint32_t k = U.b.size - U.next < BTL_UPD_CHUNK ? U.b.size - U.next : BTL_UPD_CHUNK;
        memcpy(s_dp, &U.next, 4);
        memcpy(s_dp + 4, U.img + U.next, k);
        if (!send(BTL_UPD_DATA, s_dp, (uint16_t)(4 + k))) break;
        U.next += k;
    }
}

static void end_pass(int64_t now)
{
    if (U.restarted) {
        if (now - U.t_restart >= RESTART_US) stopped(BTL_UPD_WHY_RESTARTED, false, now);
        return;
    }
    if (now - U.t_said < END_AGAIN_US) return;
    if (U.said >= SAY_TIMES) {
        upd_abort(BTL_UPD_WHY_NONE);
        failed(BT_UPD_FAILED, BT_UPD_WHY_NO_ANSWER, 0, now);
        return;
    }
    U.said++;
    U.t_said = now;
    const uint32_t size = U.b.size;
    send(BTL_UPD_END, &size, sizeof size);
}

/* Each pass of link_task: an image handed over, a stop asked for, the next
 * step of the one on its way. */
static void upd_pass(int64_t now)
{
    taskENTER_CRITICAL(&s_lock);
    const uint8_t stop  = s_stop_req;
    const bool    fresh = s_allow_us && now - s_allow_us < ALLOW_US;
    const bool    allow = fresh && s_quiet;
    s_stop_req = 0;
    taskEXIT_CRITICAL(&s_lock);
    if (U.ph == U_IDLE) take();
    if (U.ph == U_IDLE) return;
    if (s_busy_cb && now - U.t_busy >= BUSY_EVERY_US) {
        U.t_busy = now;
        U.busy   = s_busy_cb();
    }
    switch (U.ph) {
    case U_WAIT:
        wait_pass(now, stop, allow);
        break;
    case U_BEGIN:
    case U_SEND:
    case U_END:
        /* Until the chip has switched to it, anything else comes first. */
        if (stop)                 stopped(stop, true, now);
        else if (U.busy)          stopped(BUSY_WHY, true, now);
        else if (!fresh)          stopped(BT_UPD_WHY_HELD, true, now);
        else if (U.ph == U_BEGIN) begin_pass(now);
        else if (U.ph == U_SEND)  send_pass(now);
        else                      end_pass(now);
        break;
    case U_AFTER:
        if (now - U.t_done >= AFTER_US) {
            char t[96];
            snprintf(t, sizeof t, "%s: no word from the chip in 3 minutes after its restart", U.ver);
            ended(BT_UPD_UNKNOWN, BTL_UPD_WHY_NONE, t, now);
        }
        break;
    }
}

/* Quick reads while an image goes over: the chip's answer to the last
 * frames sent would sit 20 ms in the UART, and the transfer with it. */
static bool upd_quick(void)
{
    return U.ph == U_BEGIN || U.ph == U_SEND || U.ph == U_END;
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
        const uint8_t flags = n >= 2 ? p[1] : 0;
        taskENTER_CRITICAL(&s_lock);
        const bool first = !s_st.companion;
        s_st.companion = true;
        s_st.proto     = n ? p[0] : 0;
        s_st.flags     = flags;
        strlcpy(s_st.version, v, sizeof s_st.version);
        /* Restarted: what it said of its firmware before is old news. */
        if (ask) s_upd.info = false;
        /* The knob's VOLUME, for a speaker's own: said again (av_pass). */
        s_av.sent = 0xFF;
        taskEXIT_CRITICAL(&s_lock);
        if (first || ask) ESP_LOGI(TAG, "the companion %s: protocol %u, %s%s%s%s", ask ? "started" : "answers",
                                   n ? p[0] : 0, v, (flags & BTL_HELLO_UPDATE) ? ", takes updates" : "",
                                   (flags & BTL_HELLO_SPEAKERS) ? ", plays to speakers" : "",
                                   (flags & BTL_HELLO_AV_VOLUME) ? ", sets their volume" : "");
        if (ask) {
            hello(false);
            upd_chip_started(now);
        }
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
            /* Only from a headset that is there: the second chip's firmware
             * changes by itself now, and none of it, faulty or not, keys the
             * radio -- nor answers a call -- without one. A speaker's own
             * hands-free link may send its buttons too: they key nothing. */
            taskENTER_CRITICAL(&s_lock);
            const bool spk   = s_st.hs.kind == BTL_KIND_SPEAKER;
            const bool there = s_st.hs.link == BTL_LINK_CONNECTED && !spk;
            if (there) s_st.presses++;
            taskEXIT_CRITICAL(&s_lock);
            if (spk)         ESP_LOGI(TAG, "a speaker's button: not taken -- a speaker keys nothing");
            else if (!there) ESP_LOGW(TAG, "a headset's button, with no headset connected: not taken");
        }
        break;
    case BTL_EVT_VOLUME:
        /* The headset's own gains, as it changes them. A headset that mutes
         * its microphone says so as a gain of 0 -- the Jabras do. A
         * speaker's hands-free link may say them too: nothing is muted. */
        if (n >= 2) {
            taskENTER_CRITICAL(&s_lock);
            const bool was_muted = s_st.hs.mic == 0;
            const bool spk       = s_st.hs.kind == BTL_KIND_SPEAKER;
            s_st.hs.spk = p[0];
            s_st.hs.mic = p[1];
            taskEXIT_CRITICAL(&s_lock);
            if (!spk && (p[1] == 0) != was_muted)
                ESP_LOGI(TAG, "headset microphone %s", p[1] == 0 ? "muted" : "live");
            ESP_LOGD(TAG, "%s volume %u, microphone %u", spk ? "speaker" : "headset", p[0], p[1]);
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
    case BTL_UPD_STATUS:
        on_status(p, n);
        break;
    case BTL_UPD_INFO:
        on_info(p, n);
        break;
    default:
        ESP_LOGD(TAG, "frame 0x%02x, %u bytes", type, n);
        break;
    }
}

/* The knob's VOLUME to a companion that sets a speaker's own with it (its
 * hello's AV_VOLUME), which keeps it for the speaker that takes it: after
 * its hello, and as it changes -- the dial turning, its latest no oftener
 * than AV_EVERY_US -- and the same again every AV_AGAIN_US while a speaker
 * takes it, in case one was lost on the wire. Not while a turn of the
 * speaker's own waits for the knob's loop: that is the VOLUME now. */
static void av_pass(int64_t now)
{
    taskENTER_CRITICAL(&s_lock);
    const bool can   = s_st.companion && (s_st.flags & BTL_HELLO_AV_VOLUME);
    const bool takes = can && s_st.hs.link == BTL_LINK_CONNECTED && s_st.hs.kind == BTL_KIND_SPEAKER &&
                       (s_st.hs.av & BTL_AV_TAKES);
    const uint8_t v    = s_av.knob;
    const bool    news = v != s_av.sent;
    const bool    go   = can && v <= 100 && s_av.take == 0xFF &&
                         ((news && now - s_av.t_sent >= AV_EVERY_US) || (takes && now - s_av.t_sent >= AV_AGAIN_US));
    if (go) {
        if (news) s_av.t_dial = now;
        s_av.sent   = v;
        s_av.t_sent = now;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (go) send(BTL_CMD_AV_VOLUME, &v, 1);
}

static void link_task(void *arg)
{
    (void)arg;
    uint8_t *chunk = heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    int64_t t_hello = 0, t_ping = 0;
    uint32_t seq = 0;
    for (;;) {
        const int n = uart_read_bytes(LINK_UART, chunk, 512, pdMS_TO_TICKS(upd_quick() ? 2 : 20));
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
            /* Its own firmware, likewise: what it runs, when it has not said
             * since its start -- and while that is on trial, or an update's
             * outcome is awaited, for the KEEP it needs (on_info). */
            if (((s_st.flags & BTL_HELLO_UPDATE) && (!s_upd.info || s_upd.chip.state == BTL_RUN_TRIAL)) ||
                U.ph == U_AFTER)
                send(BTL_UPD_ASK, NULL, 0);
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
            s_upd.info = false;
            s_av.sent  = 0xFF;
            s_av.take  = 0xFF;
            s_av.known = false;
            taskEXIT_CRITICAL(&s_lock);
            static const btl_state_t none;
            device(&none);
            if (upd_quick()) stopped(BT_UPD_WHY_GONE, true, now);
        }
        av_pass(now);
        upd_pass(now);
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
    const bool c = s_st.companion && s_st.hs.link == BTL_LINK_CONNECTED && s_st.hs.kind != BTL_KIND_SPEAKER;
    taskEXIT_CRITICAL(&s_lock);
    return c;
}

bool bt_link_speaker_connected(void)
{
    taskENTER_CRITICAL(&s_lock);
    const bool c = s_st.companion && s_st.hs.link == BTL_LINK_CONNECTED && s_st.hs.kind == BTL_KIND_SPEAKER;
    taskEXIT_CRITICAL(&s_lock);
    return c;
}

bool bt_link_speaker_audio(void) { return s_spk_audio; }

/* A speaker whose companion says nothing of its delay: about what an A2DP
 * sink keeps, with the companion's own buffer and the stream's tick. */
#define SPK_DELAY_MS 250

uint32_t bt_link_speaker_delay_ms(void)
{
    if (!s_spk_audio) return 0;
    taskENTER_CRITICAL(&s_lock);
    const uint32_t d = s_st.hs.delay_ms;
    taskEXIT_CRITICAL(&s_lock);
    return d ? d : SPK_DELAY_MS;
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
    const bool m = s_st.companion && s_st.hs.link == BTL_LINK_CONNECTED && s_st.hs.kind != BTL_KIND_SPEAKER &&
                   s_st.hs.mic == 0;
    taskEXIT_CRITICAL(&s_lock);
    return m;
}

int bt_link_battery(void)
{
    taskENTER_CRITICAL(&s_lock);
    const bool known = s_st.companion && s_st.hs.link == BTL_LINK_CONNECTED && s_st.hs.batt != BTL_BATT_NONE &&
                       s_st.hs.batt_pct <= 100;
    const int pct = known ? s_st.hs.batt_pct : -1;
    taskEXIT_CRITICAL(&s_lock);
    return pct;
}

int bt_link_found(btl_found_t *out, int max)
{
    taskENTER_CRITICAL(&s_lock);
    const int n = s_nfound < max ? s_nfound : max;
    memcpy(out, s_found, (size_t)n * sizeof *out);
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

/* The page's Bluetooth buttons: someone at the headsets and speakers, which
 * come first -- an update of the second chip going steps aside. */
void bt_link_scan(uint8_t seconds)
{
    upd_stop_req(BT_UPD_WHY_PAGE);
    taskENTER_CRITICAL(&s_lock);
    s_nfound = 0;
    taskEXIT_CRITICAL(&s_lock);
    send(BTL_CMD_SCAN, &seconds, 1);
}

void bt_link_want_audio(bool on)
{
    if (s_want_audio == on) return;
    s_want_audio = on;
    /* A call ringing in, or dialled: an update of the second chip steps aside. */
    if (on) upd_stop_req(BT_UPD_WHY_CALL);
    send_audio();               /* a companion not there yet hears it with the hello */
}

void bt_link_connect(const uint8_t bda[6])
{
    upd_stop_req(BT_UPD_WHY_PAGE);
    send(BTL_CMD_CONNECT, bda, 6);
}

void bt_link_disconnect(void)
{
    upd_stop_req(BT_UPD_WHY_PAGE);
    send(BTL_CMD_DISCONNECT, NULL, 0);
}

void bt_link_forget(const uint8_t bda[6])
{
    upd_stop_req(BT_UPD_WHY_PAGE);
    send(BTL_CMD_FORGET, bda, 6);
}

bool bt_link_set_kind(const uint8_t bda[6], uint8_t kind)
{
    taskENTER_CRITICAL(&s_lock);
    const bool can = s_st.companion && (s_st.flags & BTL_HELLO_SPEAKERS);
    taskEXIT_CRITICAL(&s_lock);
    if (!can) return false;                     /* a companion before speakers: it knows headsets only */
    upd_stop_req(BT_UPD_WHY_PAGE);
    uint8_t p[7];
    memcpy(p, bda, 6);
    p[6] = kind == BTL_KIND_SPEAKER ? BTL_KIND_SPEAKER : BTL_KIND_HEADSET;
    return send(BTL_CMD_KIND, p, sizeof p);
}

void bt_link_set_volume(uint8_t vol)
{
    taskENTER_CRITICAL(&s_lock);
    s_av.knob = vol > 100 ? 100 : vol;
    taskEXIT_CRITICAL(&s_lock);
}

bool bt_link_take_volume(uint8_t *vol)
{
    taskENTER_CRITICAL(&s_lock);
    const uint8_t t = s_av.take;
    if (t != 0xFF) {
        s_av.knob = t;                  /* the VOLUME from now on: nothing to send back */
        s_av.take = 0xFF;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (t == 0xFF || !vol) return false;
    *vol = t;
    return true;
}

uint8_t bt_link_speaker_volume(void)
{
    taskENTER_CRITICAL(&s_lock);
    const bool    spk = s_st.companion && s_st.hs.link == BTL_LINK_CONNECTED && s_st.hs.kind == BTL_KIND_SPEAKER;
    const uint8_t av  = s_st.hs.av;
    taskEXIT_CRITICAL(&s_lock);
    return !spk                    ? BT_VOL_NONE
           : (av & BTL_AV_SET)     ? BT_VOL_KNOB
           : (av & BTL_AV_REFUSED) ? BT_VOL_REFUSED
           : (av & BTL_AV_TAKES)   ? BT_VOL_ASKING
                                   : BT_VOL_OWN;
}

bool bt_link_take_ptt(void)
{
    const uint32_t p = s_st.presses;
    if (p == s_ptt_taken) return false;
    s_ptt_taken = p;
    return true;
}

void bt_link_update_status(bt_link_upd_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_lock);
    *out = s_upd;
    taskEXIT_CRITICAL(&s_lock);
}

esp_err_t bt_link_update_start(uint8_t *img, size_t len, const uint8_t sha256[32], bool forced)
{
    if (!img || !sha256 || len <= IMG_APP_SHA + 8) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_lock);
    const bool ok = s_tx && s_st.companion && (s_st.flags & BTL_HELLO_UPDATE) && s_upd.info &&
                    s_upd.phase == BT_UPD_IDLE && !s_hand.img;
    if (ok) {
        s_hand.img    = img;
        s_hand.len    = len;
        s_hand.forced = forced;
        memcpy(s_hand.sha, sha256, sizeof s_hand.sha);
    }
    taskEXIT_CRITICAL(&s_lock);
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool bt_link_update_holding(void)
{
    taskENTER_CRITICAL(&s_lock);
    const bool h = s_upd.phase != BT_UPD_IDLE || s_hand.img;
    taskEXIT_CRITICAL(&s_lock);
    return h;
}

void bt_link_update_allow(bool quiet)
{
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    s_allow_us = now;
    s_quiet    = quiet;
    taskEXIT_CRITICAL(&s_lock);
}

void bt_link_update_busy_cb(bool (*busy)(void)) { s_busy_cb = busy; }

void bt_link_update_stop(uint8_t why)
{
    if (why) upd_stop_req(why);
}

bool bt_link_update_blocked(const uint8_t app_sha[8], char *why, size_t cap)
{
    taskENTER_CRITICAL(&s_lock);
    const uint8_t cls  = s_upd.chip.back;
    const bool    back = back_real(cls) && !memcmp(s_upd.chip.back_sha, app_sha, 8);
    const bool    last = !back && s_upd.block && !memcmp(s_upd.last_sha, app_sha, 8);
    if (last && why && cap) strlcpy(why, s_upd.text, cap);
    const bt_link_upd_record_t r = s_upd.rec;
    const bool rec = !back && !last && s_upd.remembered && !memcmp(r.sha8, app_sha, 8) &&
                     (r.result != BT_UPD_NONE || r.tries >= BT_UPD_TRIES);
    taskEXIT_CRITICAL(&s_lock);
    if (back && why && cap) snprintf(why, cap, "it went back on the second chip: %s", back_words(cls));
    if (rec && why && cap) {
        if (r.result == BT_UPD_WENT_BACK)
            snprintf(why, cap, "it went back on the second chip: %s", back_words(r.why));
        else if (r.result == BT_UPD_REFUSED)
            snprintf(why, cap, "the second chip refused it: %s", why_words(r.why));
        else if (r.result != BT_UPD_NONE)
            snprintf(why, cap, "it failed on the second chip: %s", why_words(r.why));
        else
            snprintf(why, cap, "the second chip restarted into it %u times and kept it none of them",
                     (unsigned)r.tries);
    }
    return back || last || rec;
}

void bt_link_update_record(const bt_link_upd_record_t *rec)
{
    taskENTER_CRITICAL(&s_lock);
    s_upd.remembered = rec != NULL;
    if (rec) s_upd.rec = *rec;
    else     memset(&s_upd.rec, 0, sizeof s_upd.rec);
    taskEXIT_CRITICAL(&s_lock);
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
