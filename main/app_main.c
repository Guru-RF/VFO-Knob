/* VFO-Knob bring-up.
 *
 *   M1  memory baseline        -- decides the LVGL draw-buffer budget
 *   M2  board facts            -- which I2C devices exist, DRV2605 identity
 *   M3  actuator type          -- ERM vs LRA; moves every haptic threshold
 *   M4  display                -- init table, geometry, backlight
 *   M5  effect vocabulary      -- the detent ladder must be felt, not logged
 *   M6  knob input             -- pulses per detent, bounce, and both directions
 *   M8  click per detent
 *   M12 WiFi + NVS + mDNS      -- associate, resolve the AetherSDR host
 *   M13 TCI client             -- greeting, anti-echo, knob moves the radio       -- the first thing that feels like the product,
 *                                 reached with no graphics and no network
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>

#include "audio_in.h"
#include "audio_out.h"
#include "board.h"
#include "bt_link.h"
#if VFO_RADIO_PHONE
#include "phone_client.h"
#endif
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sd_cache.h"
#include "cJSON.h"
#include "board_pins.h"
#include "drv2605.h"
#include "gpio_scan.h"
#include "hal_encoder.h"
#include "net_prov.h"
#include "radio.h"
#include "hal_touch.h"
#include "panel.h"
#include "ui.h"
#include "usb_net.h"
#include "netlog.h"
#include "ota.h"
#include "webcfg.h"

#include "lwip/sockets.h"
#include "vfo_tune.h"

#include "esp_chip_info.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
/* Web SDRs as a second receiver: the Icom, Xiegu and FlexRadio firmwares. */
#if VFO_HAS_SDR
#include "sdr_rx.h"
#endif
/* The ubersdr firmware's receiver: spots, voices, SSTV. */
#if VFO_RADIO_UBERSDR
#include "uber.h"
#endif
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"
#include "ptt_fsm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "vfo";

/* The firmware picker, asked for with a finger held on the meter arc and a
 * yes: installed at boot, before the radio takes the RAM, as an update is
 * (see UPDATE_ON_BOOT). RTC memory survives the deliberate restart. */
#define PICKER_ON_BOOT 0x50494b52u               /* "PIKR" */
RTC_NOINIT_ATTR static uint32_t s_picker_on_boot;
static bool     s_picker_accepted;

/* The timer that declares this boot healthy, 20 s in: see boot_ok_cb(). */
static esp_timer_handle_t s_boot_ok_t;
/* ...and it has: this image is confirmed. */
static volatile bool s_boot_ok;

/* Declare the boot healthy now, on the operator's yes on the dial, which is
 * proof enough that it works. A freshly installed image is on trial until the
 * timer says otherwise: restarted before then, the bootloader puts the one
 * before it back -- a firmware picker accepted 16 s after an install came up
 * in the old firmware and installed nothing -- and until then it cannot start
 * another install either. The timer's own task does the flash writes, as it
 * would have; this waits until the image reads as confirmed, 2 s at most. */
static void boot_ok_now(void)
{
    if (!s_boot_ok_t || !esp_timer_is_active(s_boot_ok_t)) return;   /* said already */
    esp_timer_stop(s_boot_ok_t);
    esp_timer_start_once(s_boot_ok_t, 1);
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    for (int i = 0; i < 40 && esp_ota_get_state_partition(run, &st) == ESP_OK &&
                    st == ESP_OTA_IMG_PENDING_VERIFY; i++)
        vTaskDelay(pdMS_TO_TICKS(50));
}

static void log_cpu(void);

/* The firmware, as the address card names it: its radio, and its version --
 * "UberSDR 1.14.0", or "... dev" for a build that is not a release. Which
 * firmware a knob runs is the first question about it. */
static void firmware_line(char *out, size_t cap)
{
#if VFO_RADIO_ICOM
    const char *name = "Icom";
#elif VFO_RADIO_XIEGU
    const char *name = "Xiegu";
#elif VFO_RADIO_SVXCONNECT
    const char *name = "SVXConnect";
#elif VFO_RADIO_PHONE
    const char *name = "Telephone";
#elif VFO_RADIO_MULTIFLEX
    const char *name = "FlexRadio";
#elif VFO_RADIO_UBERSDR
    const char *name = "UberSDR";
#elif VFO_RADIO_SETUP
    const char *name = "Setup";
#else
    const char *name = "AetherSDR";
#endif
    const char *v = esp_app_get_description()->version;
    if (*v == 'v') v++;
    const int n = (int)strcspn(v, "-");
    snprintf(out, cap, "%s %.*s%s", name, n, v, v[n] ? " dev" : "");
}

/* The link is the USB cable: one computer, or one radio, at its far end. */
static volatile bool s_on_usb;
/* The WiFi driver started: once, whoever starts it (pick_transport, or the
 * WiFi setup when no network is known). */
static bool s_wifi_started;
/* The radio is a receiver (radio_status_t.rx_only), as of the last status:
 * nothing on the knob keys it. */
static bool s_rx_only;
#if VFO_RADIO_PHONE
static bool s_call_ringing;       /* the telephone rings: buzzed, with the ring */
static volatile bool s_in_call;   /* calling, ringing or talking: the dial is the volume */
#endif
/* The setup firmware's filling of the SD card with every firmware published:
 * an install stops it first -- one download at a time. */
static volatile bool s_fill_stop, s_fill_running;

#if !VFO_RADIO_SETUP && !VFO_RADIO_SVXCONNECT && !VFO_RADIO_PHONE
/* Another radio, chosen with a swipe up: in use from the next boot, and the
 * knob restarts into it at once -- the clients have no restart path. Never
 * while transmitting. The boot is confirmed first: a restart inside its first
 * 20 s would count against the image, and three would mean safe mode.
 *
 * `i` counts the configured radios first, then those the client found for
 * itself (radio_found_count: the FlexRadio firmware's, on the LAN by their
 * broadcast -- one chosen joins the configured ones -- and SmartLink's). */
static void switch_radio(int i)
{
    static net_radio_t r;
    const int nd = net_prov_radio_count();
    const int cur = radio_found_active() >= 0 ? nd + radio_found_active() : net_prov_radio_active();
    char name[24] = "";
    if (i == cur) return;
    if (i < nd) {
        if (!net_prov_radio_get(i, &r)) return;
        strlcpy(name, r.name[0] ? r.name : net_prov_host_shown(r.host), sizeof name);
    } else if (!radio_found_get(i - nd, name, sizeof name)) {
        return;
    }
    if (radio_on_air()) {
        ESP_LOGW(TAG, "radio not switched: on the air");
        return;
    }
    ESP_LOGW(TAG, "switching to %s (%s)", name, i < nd ? r.host
                                          : i < nd + radio_found_lan() ? "found on the LAN"
                                          : radio_found_via());
    ui_switching(name);
    boot_ok_now();
    esp_err_t e;
    if (i < nd) {
        e = radio_found_use(-1);
        if (e == ESP_OK) e = net_prov_radio_activate(i);
    } else {
        e = radio_found_use(i - nd);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "radio not switched: the choice could not be saved");
        ui_updating_hide();
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}
#endif

#define DRV2605_REG_STATUS 0x00

static drv2605_t s_drv;

/* ---------------------------------------------------------------- M1 / M2 */

static void report_memory(void)
{
    ESP_LOGI(TAG, "--- M1 memory baseline ---");
    if (esp_psram_is_initialized())
        ESP_LOGI(TAG, "PSRAM: %u bytes", (unsigned)esp_psram_get_size());
    else
        ESP_LOGE(TAG, "PSRAM NOT INITIALISED - check SPIRAM_MODE_OCT");

    /* LVGL's flush buffers must be internal + DMA (PSRAM costs 2.5-4x) and
     * have to coexist with WiFi and lwIP. Under ~80 kB free with WiFi up means
     * dropping from 2 x 40 lines to 2 x 30. */
    ESP_LOGI(TAG, "free internal      : %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "free internal+DMA  : %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    ESP_LOGI(TAG, "largest DMA block  : %u", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    ESP_LOGI(TAG, "free SPIRAM        : %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void probe_i2c(void)
{
    ESP_LOGI(TAG, "--- M2 i2c scan (expect exactly 0x15 and 0x5A) ---");
    i2c_master_bus_handle_t bus = board_i2c();
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            const char *who = addr == BOARD_I2C_ADDR_TOUCH  ? " (CST816 touch)"
                            : addr == BOARD_I2C_ADDR_HAPTIC ? " (DRV2605 haptic)"
                                                            : " (UNEXPECTED)";
            ESP_LOGI(TAG, "  0x%02X ACK%s", addr, who);
            found++;
        }
    }
    if (found == 0)
        ESP_LOGE(TAG, "  nothing answered - check SDA=%d SCL=%d and pull-ups",
                 BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL);
}

/* -------------------------------------------------------------- M3 haptic */

static void haptic_bringup(void)
{
    ESP_LOGI(TAG, "--- M3 actuator + calibration ---");
    if (drv2605_init(&s_drv, board_i2c(), BOARD_I2C_ADDR_HAPTIC,
                     DRV_ACTUATOR_ERM) != ESP_OK) {
        ESP_LOGE(TAG, "  DRV2605 init failed");
        return;
    }
    /* Auto-calibration measures real back-EMF and fails against the wrong
     * actuator type, so a pass here IS the ERM/LRA answer -- more reliable
     * than the subjective open-loop listen test. Cache in NVS later so boots
     * skip the only slow step in haptic bring-up. */
    drv_calibration_t cal;
    esp_err_t err = drv2605_autocal(&s_drv, &cal);
    ESP_LOGI(TAG, "  ERM auto-cal: %s", esp_err_to_name(err));

    /* Library B, not A: the 5-15 ms brake against A's 20-40 ms is what turns
     * a buzz into a click, and A is out of spec in bidirectional closed loop. */
    drv2605_set_library(&s_drv, DRV_LIB_B);
}

/* ------------------------------------------------------------- M6 / M8 --- */

/* NO HAPTICS WHILE TUNING.
 *
 * The original design synthesised a click per detent, on the assumption the
 * knob might be detentless. It is not -- it has 30 real mechanical detents, so
 * a motor pulse on top of a detent you can already feel is redundant, and it
 * spends the haptic channel on the one event that needs it least.
 *
 * Haptics are reserved for what the operator CANNOT otherwise perceive:
 * PTT state, a rejected or clamped tune, link loss, and band edges when the
 * band plan lands. The velocity estimate below stays because acceleration
 * needs it.
 */

static int s_counts_per_detent = ENC_COUNTS_PER_DETENT_DEFAULT;

/* Step decade is chosen by tapping a frequency digit. One value, shared: the
 * knob reads it, the UI writes it. */
static _Atomic int32_t s_step_hz = 1000;

/* Set by net_task when there is no computer on the cable, and shown by
 * ui_task in the warning panel; see net_task for when and why. */
static atomic_bool s_flip_hint;
/* When the headset's PTT was last refused, its microphone muted (ms): the
 * banner says so for 3 s. */
static int64_t s_hs_refused_ms;
#if !VFO_RADIO_SETUP
/* The boom arm is the PTT and the headset came with it down: the slab asks
 * for it up before anything keys. */
static bool s_hs_raise;
#endif

#if CONFIG_VFO_USB_NET
/* How long a cable with a computer on it gets, from boot, to come up as a
 * network before WiFi is tried: until a little after it is due. */
#define USB_GRACE_US (((int64_t)CONFIG_VFO_USB_NET_DELAY_MS + 5000) * 1000)

/* What is on the S3's side of the USB-C cable, decided by usb_net_task
 * during the flash window. */
enum { CABLE_UNKNOWN = 0, CABLE_COMPUTER, CABLE_NONE };
static atomic_int s_cable;
#endif

static void encoder_task(void *arg)
{
    (void)arg;
    accel_t accel; accel_init(&accel);
    tune_t  tune;  tune_init(&tune, 14074000, 1000);

    TickType_t next       = xTaskGetTickCount();
    int32_t  residue      = 0;
    int64_t  idle_since   = esp_timer_get_time();
    int64_t  run_counts   = 0;
    bool     moving       = false;
    float    v_peak       = 0.0f;

    ESP_LOGI(TAG, "--- knob ready (tuning is silent by design) ---");

    for (;;) {
        /* Fixed 1 ms cadence. The contact-duration filter needs regular
         * samples, not edges, so there is nothing to be gained from an ISR. */
        vTaskDelayUntil(&next, pdMS_TO_TICKS(ENC_POLL_MS));
        int32_t delta = hal_encoder_poll();
        int64_t now_us = esp_timer_get_time();
        uint32_t now_ms = (uint32_t)(now_us / 1000);

        if (delta == 0) {
            if (moving && now_us - idle_since > 2000000) {
                moving = false;
                enc_stats_t st;
                hal_encoder_stats(&st);
                /* raw vs rejected is the bounce measurement: a lockout that is
                 * too short shows up as rejected ~= 0 with inflated counts, one
                 * that is too long shows up as lost detents at speed. */
                ESP_LOGI(TAG, "[M6] idle. run=%+lld  net=%+ld  peak=%.1f det/s | "
                              "raw A=%u B=%u | accepted A=%u B=%u | rejected=%u | "
                              "f=%lld Hz",
                         (long long)run_counts, (long)hal_encoder_count(),
                         (double)v_peak, (unsigned)st.raw_a, (unsigned)st.raw_b,
                         (unsigned)st.accepted_a, (unsigned)st.accepted_b,
                         (unsigned)st.rejected, (long long)tune.f_display);
                run_counts = 0; v_peak = 0.0f;
            }
            continue;
        }

        ui_note_activity();
        /* Turning the knob answers a question on the dial: "update?" no, and
         * the knob tunes on; "firmware?" yes, and the turn is spent on it. */
        if (ui_ask_knob_moved()) continue;
        if (!moving) { moving = true; run_counts = 0; }
        idle_since  = now_us;
        run_counts += delta;

        /* Quantise raw counts to detents, carrying the remainder so nothing is
         * lost across calls. */
        residue += delta;
        int32_t detents = residue / s_counts_per_detent;
        residue -= detents * s_counts_per_detent;
        if (detents == 0) continue;

        /* While a field editor is open the knob picks a value instead of
         * tuning. Selection on the precise rotary, commitment on the
         * imprecise touch. */
        if (ui_edit_active()) {
            ui_edit_rotate(detents);
            continue;
        }
#if VFO_RADIO_PHONE
        /* In a call the dial is the volume; the favourites wait for it to end. */
        if (s_in_call) {
            ui_volume_turn(detents);
            continue;
        }
#endif

        tune.step_hz = atomic_load(&s_step_hz);
        uint8_t mult = accel_update(&accel, detents, now_ms);
        if (accel.v_detents > v_peak) v_peak = accel.v_detents;

        /* ONE frequency, not two. The client owns the optimistic value because
         * it also has to survive reconciliation against the radio; the haptics
         * read it back so rollover and decimation fire on the frequency the
         * operator is actually on. Keeping a second local copy let the two
         * drift 10 MHz apart during the first real-radio test. */
        int64_t before = radio_tune_by(0, 1, tune.step_hz);
        int64_t after  = radio_tune_by(detents, mult, tune.step_hz);
        tune.f_display = after;        /* keep the local step model in step */

        /* Tuning is silent by design; see the note above. `before` and
         * `after` remain wired up so the band-edge signal can hook in here
         * without restructuring anything. */
        (void)before; (void)after;
    }
}

/* Every haptic goes through here, and none plays on the air: from the moment
 * a key is asked for until the radio is back on receive -- keyed by us or by
 * anyone -- the motor stays still. It sits millimetres from the microphone,
 * and a click or a buzz went out over the air with the operator's voice. */
static void haptic(uint8_t effect)
{
    if (!effect || radio_on_air()) return;
    drv2605_fire(&s_drv, effect);
}

/* The radio clients declare this weak so they stay free of a haptic
 * dependency. This is where their share of the haptic channel is spent: a
 * refused key, the unkey. Never tuning -- the knob has real detents of its
 * own. */
void haptic_hook(uint8_t effect, uint8_t prio)
{
    (void)prio;
    haptic(effect);
}

#if VFO_RADIO_SETUP
/* --- the firmwares onto the SD card, through the cable ----------------------
 *
 * tools/install-setup.sh's, as it provisions a knob: every firmware published,
 * pushed down the USB cable onto the card while the knob is in setup -- no
 * network needed where knobs are made. On the console, a line each way:
 *
 *   @card begin                          -> @card ready | @card none | @card closed
 *   @card put <radio> <size> <manifest>  and the image's <size> bytes
 *                                        -> @card ok <radio> | @card bad <radio> <why>
 *   @card index <size>                   and the index's bytes -> @card ok index
 *   @card end                            -> @card done <n>
 *
 * The bytes go CARD_BLOCK at a time, each block answered "@card k <bytes so
 * far>" before the next is sent: the console's driver drops what its receive
 * buffer cannot hold -- there is no flow control on the line -- and a block
 * always fits in it, however long the card takes over the one before. The
 * manifest is the release's, on one line; an image is kept only with its
 * sha256. The log is quiet meanwhile: the line is the script's.
 *
 * Only on the start the script's flashing gives the knob, the one that empties
 * the card (card_console_task); "@card closed" on any other. That receive
 * buffer is internal RAM -- the driver's ring, 16 kB -- and so is the session's
 * stack: kept beside the WiFi, they left the hotspot too little to send its
 * page, and phones showed it blank. */
#define CARD_BLOCK   8192
#define CARD_RX_RING (2 * CARD_BLOCK)
#define CARD_STACK   6144                 /* FAT and a sha256 on it */
#define CARD_WAIT_US (180 * 1000000LL)    /* for the script, or for its retry */
static volatile bool s_card_ready;      /* provisioning's emptying of it done */
static bool s_flashed_now;              /* tools/install-setup.sh wrote the knob just now */

/* A reply, on a line of its own: what the host reads first may be the end
 * of a log line the USB FIFO held while nobody was listening. */
static void card_say(const char *fmt, ...)
{
    char l[160] = "\n";
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(l + 1, sizeof l - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    n += 1;
    if (n > (int)sizeof l - 2) n = (int)sizeof l - 2;
    l[n++] = '\n';
    l[n] = 0;
    /* The way the log goes out, which is flushed: the driver's own write
     * left a short line in the chip until more came after it. */
    fputs(l, stdout);
    fflush(stdout);
}

/* A line, without its end; false after `ms` of nothing. */
static bool card_line(char *buf, size_t cap, int ms)
{
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(ms)) != 1) return false;
        if (c == '\n') break;
        if (c != '\r' && n + 1 < cap) buf[n++] = (char)c;
    }
    buf[n] = 0;
    return true;
}

static bool card_bytes(uint8_t *dst, size_t n)
{
    for (size_t got = 0; got < n; ) {
        const int k = usb_serial_jtag_read_bytes(dst + got, n - got, pdMS_TO_TICKS(5000));
        if (k <= 0) return false;
        got += (size_t)k;
    }
    return true;
}

/* One image off the line onto the card: true when it was kept. Its bytes are
 * read to the last whatever happens, or the line is out of step; *lost when
 * they stopped coming. */
static bool card_put(const char *radio, size_t size, const char *manifest, uint8_t *buf, size_t cap,
                     bool *lost, const char **why)
{
    char sha[72] = "";
    cJSON *m = cJSON_Parse(manifest);
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(m, "sha256"));
    if (s) strlcpy(sha, s, sizeof sha);
    cJSON_Delete(m);
    FILE *f = size && sha[0] && sdc_room(radio, size) ? sdc_image_create(radio) : NULL;
    *why = !sha[0] ? "no sha256 in its manifest" : "no room on the card";
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    bool wrote = f != NULL;
    for (size_t got = 0; got < size; ) {
        const size_t k = size - got > cap ? cap : size - got;
        if (!card_bytes(buf, k)) {
            *lost = true;
            *why  = "the line went quiet";
            wrote = false;
            break;
        }
        if (wrote && fwrite(buf, 1, k, f) != k) {
            wrote = false;
            *why  = "the card would not take it";
        }
        mbedtls_sha256_update(&c, buf, k);
        got += k;
        card_say("@card k %u", (unsigned)got);    /* the next block, please */
    }
    uint8_t d[32];
    char hex[65];
    mbedtls_sha256_finish(&c, d);
    mbedtls_sha256_free(&c);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    if (f && fclose(f) != 0) wrote = false;
    if (wrote && strcasecmp(hex, sha)) {
        wrote = false;
        *why  = "its sha256 is not the manifest's";
    }
    if (wrote && sdc_image_commit(radio, manifest) != ESP_OK) {
        wrote = false;
        *why  = "the card would not keep it";
    }
    if (!wrote) sdc_image_discard(radio);
    return wrote;
}

/* True when the script said it was done. */
static bool card_session(void)
{
    EXT_RAM_BSS_ATTR static char    line[1024];
    EXT_RAM_BSS_ATTR static uint8_t buf[CARD_BLOCK];
    esp_log_level_set("*", ESP_LOG_NONE);
    for (int i = 0; i < 300 && !s_card_ready; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!sdc_mount()) {
        card_say("@card none");
        esp_log_level_set("*", ESP_LOG_INFO);
        return true;
    }
    card_say("@card ready");
    int kept = 0;
    bool done = false;
    while (card_line(line, sizeof line, 20000)) {
        char radio[17];
        unsigned long size = 0;
        int at = 0;
        if (sscanf(line, "@card put %16s %lu %n", radio, &size, &at) == 2 && at > 0) {
            bool lost = false;
            const char *why = "";
            if (card_put(radio, size, line + at, buf, sizeof buf, &lost, &why)) {
                kept++;
                card_say("@card ok %s", radio);
            } else {
                card_say("@card bad %s %s", radio, why);
            }
            if (lost) break;
        } else if (sscanf(line, "@card index %lu", &size) == 1) {
            /* One block: an index is a few hundred bytes. */
            if (size >= sizeof buf || !card_bytes(buf, size)) break;
            buf[size] = 0;
            card_say(sdc_index_save((const char *)buf) == ESP_OK ? "@card ok index" : "@card bad index");
        } else if (!strcmp(line, "@card end")) {
            card_say("@card done %d", kept);
            done = true;
            break;
        }
    }
    sdc_unmount();
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGW(TAG, "provisioning: %d firmware%s onto the SD card through the cable", kept, kept == 1 ? "" : "s");
    sdc_log_state();
    return done;
}
#endif

/* Serial console. Touch does not exist yet, and every PTT fault path needs
 * exercising long before a real transmitter is involved. */
static void console_task(void *arg)
{
    (void)arg;
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "console unavailable");
        vTaskDelete(NULL);
    }
    /* NOTHING HERE MAY START A TRANSMISSION.
     *
     * The keying commands were removed after a stray byte on the port -- just
     * from opening and closing it -- was read as 't' and toggled PTT on a live
     * radio. A serial line is not a deliberate act by an operator, and PTT now
     * has a real button on the screen, so the console keeps only commands that
     * are safe to receive by accident: unkey and the aborts both STOP a
     * transmission, and the rest are read-only. */
    ESP_LOGI(TAG, "console: u=unkey  s=status  r=rotate");
    ESP_LOGI(TAG, "         p=abort:pong-stale  d=abort:link-down");

    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(200)) != 1) continue;
        switch (ch) {
        /* No key/toggle: see the banner above. Unkey stays -- it can only
         * ever make things safer. */
        case 'u': ESP_LOGI(TAG, "console: unkey");  radio_ptt_unkey();  break;
        case 'p': ESP_LOGI(TAG, "console: forcing pong-stale abort");
                  radio_ptt_force_abort(PTT_AB_PONG_STALE); break;
        case 'd': ESP_LOGI(TAG, "console: forcing link-down abort");
                  radio_ptt_force_abort(PTT_AB_LINK_DOWN);  break;
        case 'r': ui_cycle_rotation();
                  ESP_LOGI(TAG, "rotation -> %u degrees", ui_rotation() * 90u);
                  break;
#if VFO_RADIO_SETUP
        case '@': {
            /* tools/install-setup.sh's "@card begin", too late: only on the
             * start right after it wrote the knob (card_console_task). */
            char l[16] = "@";
            if (card_line(l + 1, sizeof l - 1, 1000) && !strcmp(l, "@card begin")) card_say("@card closed");
            break;
        }
#endif
        case 's': {
            radio_status_t st; radio_get_status(&st);
            ESP_LOGI(TAG, "ptt=%s rung=%u reason=%s permit=0x%03X%s "
                          "pong=%ldms refusals=%u",
                     ptt_state_name((ptt_state_t)st.ptt_state), st.ptt_rung,
                     ptt_abort_name((ptt_abort_t)st.ptt_reason),
                     (unsigned)st.permit,
                     st.permit == PERMIT_ALL ? " (may key)" : " (BLOCKED)",
                     (long)st.pong_age_ms, (unsigned)st.ptt_refusals);
            break;
        }
        default: break;
        }
    }
}

#if VFO_RADIO_SETUP
/* The console of the start right after tools/install-setup.sh wrote the knob:
 * the SD card's session, with the receive buffer a block needs, until the
 * script says it is done -- then a restart, the knob's first real start with
 * its internal RAM in one piece: given back after the session instead, the
 * buffer's hole was soon in pieces under the WiFi, and with 35 kB free there
 * was no 8 kB left for the install task's stack ("Download failed" on the
 * dial). Should the script not come, or not come back to try again, for three
 * minutes: the buffer and this stack go back to the WiFi, and the usual
 * console takes over. */
static void card_console_task(void *arg)
{
    (void)arg;
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = CARD_RX_RING;
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
        int64_t until = esp_timer_get_time() + CARD_WAIT_US;
        while (esp_timer_get_time() < until) {
            uint8_t ch;
            char l[16] = "@";
            if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(200)) != 1 || ch != '@') continue;
            if (!card_line(l + 1, sizeof l - 1, 1000) || strcmp(l, "@card begin")) continue;
            if (card_session()) {
                ESP_LOGW(TAG, "provisioning: done, restarting");
                vTaskDelay(pdMS_TO_TICKS(500));   /* the script's last answer out first */
                esp_restart();
            }
            until = esp_timer_get_time() + CARD_WAIT_US;
        }
        usb_serial_jtag_driver_uninstall();
    }
    ESP_LOGI(TAG, "provisioning: the SD card's console closed; free internal %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 2, NULL, 0);
    vTaskDelete(NULL);
}
#endif

/* The dial's memory states are the radio's, in the same order. */
_Static_assert((int)UI_MEM_OFF == (int)RADIO_MEM_OFF &&
               (int)UI_MEM_READING == (int)RADIO_MEM_READING &&
               (int)UI_MEM_READY == (int)RADIO_MEM_READY &&
               (int)UI_MEM_EMPTY == (int)RADIO_MEM_EMPTY,
               "ui_state_t.mem_state carries radio_mem_state_t");

/* The client's question, when it has one (radio_get_choice: the multiflex
 * firmware's "which station"), put on the dial and the answer given back.
 * An answer given is not asked again while the client has yet to take it:
 * that is one pass of this loop, and without this the question flickered
 * back up. */
#if VFO_RADIO_SETUP
__attribute__((unused))
#endif
static void ask_choice(const radio_status_t *st)
{
    static uint32_t asked_seq, answered_seq;
    static char titles[UI_CHOICES][12], names[UI_CHOICES][24];
    const int a = ui_take_choice();
    if (a >= 0) {
        ESP_LOGI(TAG, "choice -> %s %s", titles[a], names[a]);
        answered_seq = asked_seq;
        radio_choose((uint8_t)a);
        haptic(7);
    }
    if (!st->n_choices) {
        if (ui_choice_active()) ui_ask_choice(NULL, NULL, 0, 0);
        return;
    }
    if (st->choices_seq == answered_seq) return;
    if (ui_choice_active() && st->choices_seq == asked_seq) return;
    uint8_t n = 0;
    for (uint8_t i = 0; i < st->n_choices && n < UI_CHOICES; i++)
        if (radio_get_choice(i, titles[n], sizeof titles[n], names[n], sizeof names[n])) n++;
    asked_seq = st->choices_seq;
    ui_ask_choice(titles, names, n, st->choice_default);
}

/* Sound that a flash write would hold up: an over, or on the telephone any
 * call at all -- a ring and a ringback play too. */
static bool audio_busy(void)
{
#if VFO_RADIO_PHONE
    radio_status_t st;
    radio_get_status(&st);
    return st.call != RADIO_CALL_IDLE && st.call != RADIO_CALL_ENDED;
#else
    return radio_on_air();
#endif
}

#if VFO_RADIO_PHONE
/* A probe, for now: what holds the audio up. One sentinel per core wakes
 * every 10 ms above every task of ours and says so when it woke over 60 ms
 * late. Core 1's also keeps 640 ms of the run time of the tasks that matter,
 * so a hold the playback reports (audio_out's hook) comes with who ran
 * meanwhile. A tap in a call held the jack's audio for as long as the finger
 * was down (2026-10-01), with neither core held. Run times are read task by
 * task: uxTaskGetSystemState() measures every stack, interrupts off. */
static const char *const PROBE_NAMES[] = {
    "IDLE0", "IDLE1", "taskLVGL", "audio", "mic", "enc_input", "ui",
    "phone", "btlink", "wifi", "tiT", "sys_evt", "ipc0", "ipc1", "esp_timer",
};
#define PROBE_N    (sizeof PROBE_NAMES / sizeof PROBE_NAMES[0])
#define PROBE_RING 64
typedef struct {
    int64_t  at;
    uint32_t run[PROBE_N];
    uint8_t  lvgl_prio;            /* taskLVGL's priority then: above 4, lent */
} probe_row_t;
static TaskHandle_t  s_probe_h[PROBE_N];
static probe_row_t  *s_probe_ring;
static unsigned      s_probe_head;               /* the next row written */
static volatile bool s_hold_due;
static const char   *s_hold_where;
static int64_t       s_hold_since, s_hold_us, s_hold_gap;
static uint32_t      s_hold_done;

static void probe_hold(const char *where, int64_t since_us, int64_t held_us,
                       uint32_t dma_done, int64_t dma_gap_us)
{
    if (s_hold_due) return;                      /* one at a time */
    s_hold_where = where;
    s_hold_since = since_us;
    s_hold_us    = held_us;
    s_hold_done  = dma_done;
    s_hold_gap   = dma_gap_us;
    s_hold_due   = true;
}

static void probe_read(probe_row_t *r)
{
    r->at = esp_timer_get_time();
    for (unsigned i = 0; i < PROBE_N; i++) {
        TaskStatus_t st;
        if (!s_probe_h[i]) { r->run[i] = 0; continue; }
        vTaskGetInfo(s_probe_h[i], &st, pdFALSE, eRunning);
        r->run[i] = st.ulRunTimeCounter;
        if (i == 2) r->lvgl_prio = (uint8_t)st.uxCurrentPriority;   /* taskLVGL */
    }
}

/* Who ran from the row nearest before `since` until now, longest first. */
static void probe_report(char *line, size_t cap, int len, int64_t since)
{
    const probe_row_t *base = NULL;
    uint8_t lvgl_top = 0;
    for (unsigned k = 1; k <= PROBE_RING; k++) {
        const probe_row_t *r = &s_probe_ring[(s_probe_head + PROBE_RING - k) % PROBE_RING];
        if (!r->at) break;
        base = r;
        if (r->lvgl_prio > lvgl_top) lvgl_top = r->lvgl_prio;
        if (r->at <= since) break;
    }
    if (!base) return;
    probe_row_t now;
    probe_read(&now);
    if (now.lvgl_prio > lvgl_top) lvgl_top = now.lvgl_prio;
    if (len < (int)cap)
        len += snprintf(line + len, cap - len, "; LVGL up to priority %u", (unsigned)lvgl_top);
    uint32_t d[PROBE_N];
    for (unsigned i = 0; i < PROBE_N; i++) d[i] = now.run[i] - base->run[i];
    if (len < (int)cap)
        len += snprintf(line + len, cap - len, "; in %lld ms ran",
                        (long long)((now.at - base->at) / 1000));
    for (int k = 0; k < 8; k++) {
        unsigned best = PROBE_N;
        for (unsigned i = 0; i < PROBE_N; i++)
            if (d[i] >= 3000 && (best == PROBE_N || d[i] > d[best])) best = i;
        if (best == PROBE_N) break;
        if (len < (int)cap)
            len += snprintf(line + len, cap - len, " %s %lu", PROBE_NAMES[best],
                            (unsigned long)(d[best] / 1000));
        d[best] = 0;
    }
}

static void probe_task(void *arg)
{
    const int core = (int)(intptr_t)arg;
    if (core == 1) {
        s_probe_ring = heap_caps_calloc(PROBE_RING, sizeof *s_probe_ring, MALLOC_CAP_SPIRAM);
        if (s_probe_ring) audio_out_set_hold_hook(probe_hold);
    }
    int64_t t_prev = esp_timer_get_time(), t_said = 0, t_names = 0;
    char line[256];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const int64_t t = esp_timer_get_time();
        const int64_t held = t - t_prev;
        t_prev = t;
        if (core == 1 && s_probe_ring) {
            /* The tasks come up after this one: looked for once a second. */
            if (t - t_names > 1000000) {
                t_names = t;
                for (unsigned i = 0; i < PROBE_N; i++) {
                    if (s_probe_h[i]) continue;
                    if (i < 2) s_probe_h[i] = xTaskGetIdleTaskHandleForCore((BaseType_t)i);
                    else       s_probe_h[i] = xTaskGetHandle(PROBE_NAMES[i]);
                }
            }
            if (s_hold_due) {
                int len = snprintf(line, sizeof line, "%s held %lld ms", s_hold_where,
                                   (long long)(s_hold_us / 1000));
                if (s_hold_where[0] == 'j')
                    len += snprintf(line + len, sizeof line - len,
                                    ": %lu DMA buffers done, longest DMA gap %lld ms",
                                    (unsigned long)s_hold_done, (long long)(s_hold_gap / 1000));
                probe_report(line, sizeof line, len, s_hold_since);
                ESP_LOGW("probe", "%s", line);
                s_hold_due = false;
            }
        }
        if (held > 60000 && t - t_said > 5000000) {
            t_said = t;
            int len = snprintf(line, sizeof line, "core %d held %lld ms", core,
                               (long long)(held / 1000));
            if (core == 1 && s_probe_ring) probe_report(line, sizeof line, len, t - held);
            ESP_LOGW("probe", "%s", line);
        }
        if (core == 1 && s_probe_ring) {
            probe_read(&s_probe_ring[s_probe_head]);
            s_probe_head = (s_probe_head + 1) % PROBE_RING;
        }
    }
}
#endif

static void ui_task(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(50));   /* 20 Hz is plenty */

        {
            char line[128];
            if (ui_take_note(line, sizeof line)) ESP_LOGI("ui", "%s", line);
        }
        int32_t req = ui_take_step_request();
        if (req) {
            atomic_store(&s_step_hz, req);
            radio_set_step(req);
            ESP_LOGI(TAG, "step -> %ld Hz", (long)req);
            haptic(26);                    /* confirm the tap landed */
        }
        ui_commit_t c;
        if (ui_take_commit(&c)) {
            /* A live editor's updates come as the knob turns: logged only at
             * debug level, and felt through the detents, not the motor. */
            const esp_log_level_t lv = c.live ? ESP_LOG_DEBUG : ESP_LOG_INFO;
            if (c.have_mode) {
                ESP_LOGI(TAG, "mode -> %s", c.mode);
                radio_set_mode(c.mode);
            }
            if (c.have_filter) {
                ESP_LOG_LEVEL_LOCAL(lv, TAG, "filter -> %ld..%ld",
                         (long)c.filt_lo, (long)c.filt_hi);
                radio_set_filter(c.filt_lo, c.filt_hi);
            }
            if (c.have_filter_no) {
                ESP_LOG_LEVEL_LOCAL(lv, TAG, "filter -> FIL%u", (unsigned)c.filter_no);
                radio_select_filter(c.filter_no);
            }
            if (c.have_agc) {
                ESP_LOG_LEVEL_LOCAL(lv, TAG, "agc -> %s", c.agc);
                radio_set_agc(c.agc);
            }
            if (c.have_gain) {
                ESP_LOG_LEVEL_LOCAL(lv, TAG, "gain -> %d", c.gain);
                radio_set_gain(c.gain);
            }
            if (c.have_mem_group) {
                ESP_LOGI(TAG, "memory group -> %02u", (unsigned)c.mem_group);
                radio_memory_group(c.mem_group);
            }
            if (c.have_rx) {
                ESP_LOGI(TAG, "receiver -> %s", c.rx ? "SUB" : "MAIN");
                radio_select_rx(c.rx);
            }
            /* The radio as it stands, for the antennas' names and V/M. In
             * PSRAM: this task's stack and internal RAM are both short. */
            EXT_RAM_BSS_ATTR static radio_status_t cm;
            if (c.have_ant || c.have_tx_ant || c.have_vm) radio_get_status(&cm);
            if (c.have_ant) {
                char nm[12];
                if (radio_list_item(cm.ant_names, c.ant, nm, sizeof nm))
                    ESP_LOGI(TAG, "antenna -> %s", nm);
                else
                    ESP_LOGI(TAG, "antenna -> ANT%u%s", (unsigned)c.ant + 1, c.ant_rx ? "+RX" : "");
                radio_set_antenna(c.ant, c.ant_rx);
            }
            if (c.have_tx_ant) {
                char nm[12];
                radio_list_item(cm.tx_ant_names, c.tx_ant, nm, sizeof nm);
                ESP_LOGI(TAG, "TX antenna -> %s", nm[0] ? nm : "?");
                radio_set_tx_antenna(c.tx_ant);
            }
            switch (c.action) {
            case UI_ACT_TUNE:
                ESP_LOGI(TAG, "menu -> TUNE");
                radio_tune();
                break;
            case UI_ACT_ATU:
                ESP_LOGI(TAG, "menu -> ATU");
                radio_atu_tune();
                break;
            case UI_ACT_MEM:
                ESP_LOGI(TAG, "menu -> tuner memories %s", c.atu_mem ? "on" : "off");
                radio_atu_memories(c.atu_mem);
                break;
            default:
                break;
            }
            if (c.have_rit) {
                ESP_LOG_LEVEL_LOCAL(lv, TAG, "rit -> %+ld", (long)c.rit_hz);
                radio_set_rit(c.rit_hz);
            }
            if (c.have_freq) {
                ESP_LOGI(TAG, "band -> %lld", (long long)c.freq_hz);
                radio_goto_freq(c.freq_hz);
            }
            if (c.have_rf_gain) {
                ESP_LOG_LEVEL_LOCAL(lv, TAG, "rf gain -> %u%%", (unsigned)c.rf_gain_pct);
                radio_set_rf_gain(c.rf_gain_pct);
            }
            if (c.have_rf_power) {
                ESP_LOG_LEVEL_LOCAL(lv, TAG, "rf power -> %u%%", (unsigned)c.rf_power_pct);
                radio_set_rf_power(c.rf_power_pct);
            }
            if (c.have_tuner) {
                ESP_LOGI(TAG, "tuner -> %s", c.tuner_on ? "in the line" : "out");
                radio_set_tuner(c.tuner_on);
            }
            if (c.have_squelch) {
                ESP_LOG_LEVEL_LOCAL(lv, TAG, "squelch -> %u%%", (unsigned)c.squelch_pct);
                radio_set_squelch(c.squelch_pct);
            }
#if VFO_RADIO_UBERSDR
            if (c.have_spot) {
                ESP_LOG_LEVEL_LOCAL(lv, TAG, "spot -> %lu Hz %s", (unsigned long)c.spot_hz, c.spot_mode);
                uber_tune_to(c.spot_hz, c.spot_mode);
            }
#endif
#if VFO_HAS_SDR
            if (c.have_rxsrc) {
                ESP_LOGI(TAG, "rx -> %s", c.rxsrc < 0 ? "LOCAL" : "web SDR");
                sdr_rx_select(c.rxsrc);
            }
            if (c.have_balance) sdr_rx_set_balance(c.balance);
#endif
            /* V/M, last on the swipe down: into memory mode, or back to the
             * VFO -- only when it is not already that. */
            if (c.have_vm && c.vm_mem != (cm.mem_state != RADIO_MEM_OFF)) {
                ESP_LOGI(TAG, "V/M -> %s", c.vm_mem ? "memory mode" : "VFO, simplex");
                radio_memory_mode(c.vm_mem);
            }
#if !VFO_RADIO_SETUP && !VFO_RADIO_SVXCONNECT && !VFO_RADIO_PHONE
            if (c.have_radio) switch_radio(c.radio);
#endif
            if (!c.live) haptic(7);         /* soft bump: value committed */
        }

        /* Volume and mic gain live in the UI -- the dial's editors and the
         * configuration page both set them there -- and nothing passed them
         * on: the audio stayed at its defaults, 40 and 100, whatever the dial
         * showed, and a level turned on the dial was never saved. Apply them
         * as they change, and save them once they have settled; the save is
         * debounced because NVS wear is real and the knob turns fast. */
        {
            /* Two mic gains, the knob's own microphone's and a headset's:
             * the face shows and turns the one in use, and a headset come or
             * gone swaps them -- what the face had saved first, as the
             * outgoing one's. The other one is net_prov's, where the page
             * may have set it meanwhile. */
            static uint8_t applied_vol = 0xFF, applied_mic = 0xFF;
            static int64_t changed_at;
            static int     hs_was = -1;
            const bool hs = bt_link_headset_connected();
            if ((int)hs != hs_was) {
                if (hs_was >= 0) {
                    const uint8_t m = ui_mic_gain();
                    net_prov_set_audio(ui_volume(), hs_was ? net_prov_mic_gain() : m,
                                       hs_was ? m : net_prov_mic_gain_headset());
                    changed_at = esp_timer_get_time();
                }
                hs_was = hs;
                ui_set_levels(0xFF, hs ? net_prov_mic_gain_headset() : net_prov_mic_gain());
            }
            /* A Bluetooth speaker whose own volume is the knob's VOLUME: a
             * turn of its own buttons or knob is the VOLUME now -- the face,
             * the page and the save follow, as for a turn of the dial. */
            uint8_t spk_vol;
            if (bt_link_take_volume(&spk_vol)) ui_set_levels(spk_vol, 0xFF);
            const uint8_t vol = ui_volume(), mic = ui_mic_gain();
            if (vol != applied_vol || mic != applied_mic) {
                audio_out_set_volume(vol);
                audio_in_set_gain(mic);
                applied_vol = vol;
                applied_mic = mic;
                changed_at  = esp_timer_get_time();
            }
            /* ...and the VOLUME to such a speaker, which bt_link sends on as
             * it changes. */
            bt_link_set_volume(vol);
            net_prov_set_audio(vol, hs ? net_prov_mic_gain() : mic, hs ? mic : net_prov_mic_gain_headset());
            /* Into flash once they have settled -- and never on the air: a
             * flash write stops the audio's interrupts for up to ~100 ms, a
             * hole in a call or an over (heard as one, 2026-10-01). */
            if (changed_at && esp_timer_get_time() - changed_at > 2000000 && !audio_busy()) {
                net_prov_flush_audio();          /* no-op when unchanged */
                changed_at = 0;
            }
        }

#if VFO_RADIO_UBERSDR
        /* Someone at the knob: an UberSDR with an idle timer counts that. */
        {
            static uint32_t seen;
            const uint32_t use = ui_last_use();
            if (use != seen) {
                seen = use;
                uber_activity();
            }
        }
        /* The SSTV viewer: the picture it wants, and the one to show. */
        {
            static uint32_t shown;
            static uber_sstv_t pic;
            uint32_t gen;
            const int want = ui_sstv_wanted(&gen);
            uber_sstv_want(want, gen);
            if (uber_sstv_get(&pic, shown)) {
                /* The face too busy drawing to take it: the same picture on
                 * the next pass. Taken as shown, it was lost, and the viewer
                 * said "fetching..." for good (2026-10-03); its pixels stay
                 * put until it is. */
                const int r = ui_sstv_show(pic.px, pic.w, pic.h, pic.idx, pic.title, pic.caption,
                                           pic.failed);
                if (r >= 0) {
                    shown = pic.seq;
                    uber_sstv_shown(pic.seq, r > 0);
                } else {
                    static int64_t said;
                    const int64_t now = esp_timer_get_time();
                    if (now - said > 10000000) {
                        said = now;
                        ESP_LOGW(TAG, "SSTV: the face was busy, the picture waits a pass");
                    }
                }
            }
        }
#endif
        if (ui_take_ptt_tap() && !s_rx_only) {
#if !VFO_RADIO_PHONE && !VFO_RADIO_SETUP && !VFO_RX_ONLY
            /* With a headset connected, its microphone is the one on the air:
             * the glass keys as its button does, and not while that
             * microphone is muted -- that over would be a dead carrier --
             * with the same three clicks. Unkeying is never refused. */
            if (!radio_on_air() && bt_link_headset_muted()) {
                ESP_LOGW(TAG, "PTT refused: the headset's microphone is muted");
                s_hs_refused_ms = esp_timer_get_time() / 1000;
                haptic(12);                 /* triple click: refused */
            } else
#endif
            {
#if VFO_PTT_DRY_RUN
                /* A test build (-D VFO_PTT_DRY_RUN=1): what would have keyed
                 * or unkeyed the radio, logged and nothing more. */
                ESP_LOGW(TAG, "PTT tapped -- dry run, not sent to the radio");
#else
                ESP_LOGI(TAG, "PTT tapped");
                radio_ptt_toggle();
#endif
            }
        }
#if VFO_RADIO_PHONE
        /* A telephone: a headset's button answers a call ringing in and
         * hangs up any other, and its mute mutes the call -- the client reads
         * that itself. No boom arm as a PTT. A call coming in buzzes, with
         * the ring, every three seconds. */
        if (bt_link_take_ptt()) {
            /* A call ringing in: the headset's button answers it. Otherwise
             * it hangs up -- a call up, or one being made. */
            if (s_call_ringing) {
                ESP_LOGI(TAG, "headset button: answer");
                phone_answer();
            } else {
                ESP_LOGI(TAG, "headset button: hang up");
                radio_ptt_unkey();
            }
        }
        switch (ui_take_call_req()) {
        case 1: ESP_LOGI(TAG, "slab: answer");  phone_answer(); break;
        case 2: ESP_LOGI(TAG, "slab: decline"); phone_hangup(); break;
        default: break;
        }
        /* The keypad: a number to call, keys in a call, clicks out of one. */
        {
            char num[24];
            if (ui_take_dial(num, sizeof num)) {
                ESP_LOGI(TAG, "keypad: calling %s", num);
                if (!phone_dial(num)) haptic(12);        /* refused: not registered */
            }
            for (char k; (k = ui_take_dtmf()) != 0;) phone_dtmf(k);
            if (ui_take_key_clicks()) haptic(7);
            /* The history to the face whenever it changes; looked at once
             * the face has shown it. */
            static uint32_t hist_seq = UINT32_MAX;
            const uint32_t hs = phone_history_seq();
            if (hs != hist_seq) {
                phone_call_t *pc = heap_caps_malloc(sizeof *pc * PHONE_HIST_MAX, MALLOC_CAP_SPIRAM);
                ui_call_t    *uc = heap_caps_malloc(sizeof *uc * UI_CALLS_MAX, MALLOC_CAP_SPIRAM);
                if (pc && uc) {
                    int n = phone_history(pc, PHONE_HIST_MAX);
                    if (n > UI_CALLS_MAX) n = UI_CALLS_MAX;
                    for (int i = 0; i < n; i++) {
                        strlcpy(uc[i].number, pc[i].number, sizeof uc[i].number);
                        strlcpy(uc[i].name, pc[i].name, sizeof uc[i].name);
                        uc[i].when = pc[i].when;
                        uc[i].secs = pc[i].secs;
                        uc[i].kind = pc[i].kind;        /* PHONE_CALL_* is UI_CALL_* */
                    }
                    /* Not taken -- the face busy drawing, as it is the
                     * moment a call ends -- it goes again next time round. */
                    if (ui_set_calls(uc, (uint8_t)n)) hist_seq = hs;
                }
                free(pc);
                free(uc);
            }
            if (ui_take_calls_seen()) phone_history_seen();
        }
        {
            /* A call ringing in, felt as a phone's: the motor driven flat out
             * on the ring's own rhythm -- 400 ms, 200 rest, 400, then two
             * seconds -- as the face breathes green (ui.c). Not the clicks'
             * effects: short by design, at the 1.3 V the clicks are tuned to.
             * For the ring the coin motor gets its own 3 V, and the clicks
             * their voltage back after. */
            const uint8_t RING_RATED = 0x89;        /* 2.9 V: the overdrive clamp */
            static bool     ringing, on;
            static uint32_t t0;
            const uint32_t nowb = (uint32_t)(esp_timer_get_time() / 1000);
            if (s_call_ringing && !ringing) {
                ringing = true;
                on = false;
                t0 = nowb;
                drv2605_rtp_begin_at(&s_drv, RING_RATED);
            } else if (!s_call_ringing && ringing) {
                ringing = false;
                drv2605_rtp_end(&s_drv);
            }
            if (ringing) {
                const uint32_t p = (nowb - t0) % 3000;
                const bool want = p < 400 || (p >= 600 && p < 1000);
                if (want != on) {
                    on = want;
                    drv2605_rtp_write(&s_drv, want ? 0x7F : 0);
                }
            }
        }
#elif !VFO_RADIO_SETUP && !VFO_RX_ONLY
        /* A Bluetooth headset's call button is the PTT while one is
         * connected: a press keys, the next unkeys. Not with the headset's
         * microphone muted -- that over would be a dead carrier -- and then
         * the knob says so, with a radio refusal's three clicks. Unkeying is
         * never refused. */
        if (bt_link_take_ptt() && !s_rx_only) {
            if (!radio_on_air() && bt_link_headset_muted()) {
                ESP_LOGW(TAG, "headset PTT refused: its microphone is muted");
                s_hs_refused_ms = esp_timer_get_time() / 1000;
                haptic(12);                 /* triple click: refused */
            } else {
#if VFO_PTT_DRY_RUN
                ESP_LOGW(TAG, "headset PTT -- dry run, not sent to the radio");
#else
                ESP_LOGI(TAG, "headset PTT");
                radio_ptt_toggle();
#endif
                haptic(26);
            }
        }
        /* The boom arm as the PTT, where the operator chose it (the
         * configuration page): down -- the microphone live -- transmits, up
         * -- muted -- stops. Never by itself: a headset that comes, or a knob
         * that starts, with the boom down waits for it to go up once, and
         * the slab asks for that, in red. */
        {
            static bool armed, was_live;
            const bool conn = bt_link_headset_connected();
            const bool live = conn && !bt_link_headset_muted();
            const bool boom = bt_link_boom_ptt();
            if (!conn || !boom) armed = false;
            else if (!live)     armed = true;
            if (boom && conn && live != was_live) {
                if (live && armed && !radio_on_air() && !s_rx_only) {
#if VFO_PTT_DRY_RUN
                    ESP_LOGW(TAG, "boom down -- dry run, not sent to the radio");
#else
                    ESP_LOGI(TAG, "boom down: PTT");
                    radio_ptt_key();
#endif
                } else if (!live && radio_on_air()) {
                    ESP_LOGI(TAG, "boom up: unkeyed");
                    radio_ptt_unkey();
                }
            }
            was_live   = live;
            s_hs_raise = boom && conn && live && !armed && !s_rx_only;
        }
        /* Keyed with the headset's microphone, and the headset gone -- out of
         * reach, its battery flat: nobody can unkey from it any more, and the
         * over would go on in silence. Unkey. */
        {
            static bool hs_over;
            const bool hs = bt_link_headset_audio(), air = radio_on_air();
            if (!air) hs_over = false;
            else if (hs) hs_over = true;
            else if (hs_over) {
                hs_over = false;
                ESP_LOGW(TAG, "the headset went away on the air: unkeying");
                radio_ptt_unkey();
            }
        }
#endif

        /* The address card, up under a finger held on the S-meter: a click
         * says it can let go. */
        if (ui_take_card_shown()) haptic(1);   /* strong click */
        /* The antennas, up under a finger held on the slab: a buzz says the
         * slab has not keyed, and the editor is there. (Never on the air:
         * haptic() keeps the motor still then, and no hold opens it.) */
        if (ui_take_slab_hold()) {
            ESP_LOGI(TAG, "slab held: the antennas");
            haptic(14);                         /* strong buzz */
        }

#if !VFO_RADIO_SETUP
        /* The addresses up, and a finger held three seconds on the S-meter or
         * on them: the firmware picker? A buzz says it has been asked, since
         * the question comes up under that finger. A turn of the knob says
         * yes, and restarts the knob, which installs it at boot -- before the
         * radio takes the RAM an install needs, as an update does. */
        {
            static bool asking_picker;
            if (ui_take_picker_request() && !asking_picker) {
                ESP_LOGI(TAG, "firmware picker asked for");
                haptic(14);                 /* strong buzz: let go, and look */
                asking_picker = ui_ask_turn("FIRMWARE?", "turn the knob for the picker\n"
                                                         "tap to cancel; WiFi is kept");
            }
            if (asking_picker) {
                const int a = ui_take_update_answer();
                if (a) asking_picker = false;
                if (a > 0) {
                    if (radio_on_air()) radio_ptt_unkey();
                    for (int i = 0; i < 50 && radio_on_air(); i++) vTaskDelay(pdMS_TO_TICKS(100));
                    if (radio_on_air()) {
                        ESP_LOGE(TAG, "picker not started: still transmitting");
                    } else {
                        ESP_LOGW(TAG, "firmware picker accepted -- restarting to install it");
                        ui_updating_reboot();
                        boot_ok_now();
                        s_picker_on_boot = PICKER_ON_BOOT;
                        vTaskDelay(pdMS_TO_TICKS(300));
                        esp_restart();
                    }
                }
            }
        }
#endif

        /* The reflector face's lock and mute: toggles of what the reflector
         * client holds (svxconnect; no-ops on a radio). */
        {
            const bool lock = ui_take_lock_tap(), mute = ui_take_mute_tap();
            if (lock || mute) {
                static radio_status_t m;    /* this task's only; see st below */
                radio_get_status(&m);
                if (lock) {
                    ESP_LOGI(TAG, "talkgroup %s", m.tg_locked ? "unlocked" : "locked");
                    radio_tg_lock(!m.tg_locked);
                }
                if (mute) {
                    ESP_LOGI(TAG, "%s", m.muted ? "unmuted" : "muted");
                    radio_mute(!m.muted);
                }
                haptic(26);                 /* confirm the tap landed */
            }
        }
        /* Static: this task is the only one to run this, and three status
         * copies on its stack -- they have grown with every radio -- ran it
         * out of its 5 kB and crashed it. */
        static radio_status_t st;
        radio_get_status(&st);
        s_rx_only = st.rx_only;
#if VFO_RADIO_PHONE
        s_call_ringing = st.call == RADIO_CALL_IN;
        s_in_call = st.call == RADIO_CALL_OUT || st.call == RADIO_CALL_IN || st.call == RADIO_CALL_UP;
        /* A call lights the screen from here, the moment it rings, with the
         * motor's buzz and the green face: net_sup, which counts a call as
         * use too, comes round only every two seconds, and a tap on the dark
         * face meanwhile -- to see who is calling -- answers or declines.
         * No LVGL lock, and a backlight already lit is not written again. */
        if (s_in_call) ui_note_activity();
        ui_set_meters(phone_meters());
#endif
#if !VFO_RADIO_SETUP
        ask_choice(&st);        /* the setup firmware asks its own, directly */
#endif
#if VFO_HAS_SDR
        /* The web SDR follows the radio; retuned only when something moved. */
        if (st.link == RADIO_LINK_READY || st.link == RADIO_LINK_DEGRADED)
            sdr_rx_tune(st.f_display, st.mode, st.filt_lo, st.filt_hi);
        static sdr_status_t sd;
        sdr_rx_status(&sd);
        audio_out_sdr_mute(st.tx || st.ptt_state != PTT_IDLE);
#endif

        /* High SWR: on the glass and in the log, no longer on the motor. It
         * ran for as long as SWR stayed above 2.5, and on the air that buzz
         * went out through the microphone beside it. The readout turns red
         * instead. Readings count only with real forward power (in a speech
         * pause the figure is noise), and the alarm holds a second past the
         * last bad one so the pauses between words do not chop it up. */
        {
            static bool    s_swr_high;
            static int64_t s_swr_bad_us;
            const int64_t  now_us = esp_timer_get_time();
            if (st.tx && st.tx_fwd_w >= 1.0f && st.tx_swr > 2.5f)
                s_swr_bad_us = now_us;
            const bool high = st.tx && s_swr_bad_us &&
                              now_us - s_swr_bad_us < 1000000;
            if (high != s_swr_high) {
                s_swr_high = high;
                if (high) ESP_LOGW(TAG, "SWR high: %.1f", (double)st.tx_swr);
                else      ESP_LOGI(TAG, "SWR high: over");
            }
        }

        /* AetherSDR owns the band plan and every other transmit precondition.
         * The protocol gives no reason for a refusal -- only trx:false -- so
         * the banner and the refusal haptic are all the operator gets. Hold a
         * refusal on screen for 3 s; it is otherwise a single frame. */
        static uint32_t s_seen_refusals;
        const bool link_ok = (st.link == RADIO_LINK_READY ||
                              st.link == RADIO_LINK_DEGRADED);
        static int64_t  s_warn_until;
        const char     *warn = NULL;
        int64_t nowms = esp_timer_get_time() / 1000;

        if (st.ptt_refusals != s_seen_refusals) {
            s_seen_refusals = st.ptt_refusals;
            s_warn_until    = nowms + 3000;
        }
        /* The radio's notes ("ATU FAILED") get the same 3 s. */
        static uint32_t s_seen_note;
        static int64_t  s_note_until;
        if (st.note_seq != s_seen_note) {
            s_seen_note  = st.note_seq;
            s_note_until = nowms + 3000;
        }
        /* A refusal says why, where the radio gives a reason. */
        if (s_hs_refused_ms && nowms - s_hs_refused_ms < 3000)
                                                     warn = "HEADSET MUTED";
        else if (nowms < s_warn_until)               warn = st.tx_why[0] ? st.tx_why
                                                                         : "TX REFUSED";
        else if (nowms < s_note_until && st.note[0]) warn = st.note;
        else if (atomic_load(&s_flip_hint))          warn = "FLIP USB-C";
        else if (!(st.link == RADIO_LINK_READY ||
                   st.link == RADIO_LINK_DEGRADED))    warn = st.link_why[0] ? st.link_why : "NO LINK";
        else if (st.slice_locked)                    warn = "VFO LOCKED";
#if !VFO_RX_ONLY
        else if (!st.rx_only && !(st.permit & PERMIT_TX_ENABLE)) warn = "TX DISABLED";
#endif

        /* Static, in PSRAM: the face's state has grown with every radio, and
         * on this task's 5 kB stack it left a few hundred bytes to spare. */
        EXT_RAM_BSS_ATTR static ui_state_t u;
        u = (ui_state_t){
            .n_sstv        = -1,            /* no gallery: the ubersdr's says */
            .rx_only       = st.rx_only,
            .no_rit        = st.no_rit,
            .f_min         = st.f_min,
            .f_max         = st.f_max,
            .freq_hz       = st.f_display,
            .step_hz       = atomic_load(&s_step_hz),
            .mode          = st.mode,
            .filt_lo       = st.filt_lo,
            .filt_hi       = st.filt_hi,
            .filter_no     = st.filter_no,
            .have_gain     = st.have_gain,
            .gain          = st.gain,
            .gain_min      = st.gain_min,
            .gain_max      = st.gain_max,
            .gain_step     = st.gain_step,
            .has_memories  = st.has_memories,
            .mem_state     = st.mem_state,
            .mem_group     = st.mem_group,
            .mem_band      = st.mem_band,
            .mem_all       = st.mem_all,
            .mem_ch        = st.mem_ch,
            .mem_duplex    = st.mem_duplex,
            .mem_offset_hz = st.mem_offset_hz,
            .mem_tone_dhz  = st.mem_tone_dhz,
            .n_rx          = st.n_rx,
            .rx            = st.rx,
            .n_ant         = st.n_ant,
            .ant           = st.ant,
            .has_rx_ant    = st.has_rx_ant,
            .ant_rx        = st.ant_rx,
            .have_ant      = st.have_ant,
            .n_tx_ant      = st.n_tx_ant,
            .tx_ant        = st.tx_ant,
            .have_tx_ant   = st.have_tx_ant,
            .has_tune      = st.has_tune,
            .has_atu       = st.has_atu,
            .atu_mem       = st.atu_mem,
            .has_levels    = st.has_levels,
            .have_levels   = st.have_levels,
            .rf_gain_pct   = st.rf_gain_pct,
            .rf_power_pct  = st.rf_power_pct,
            .max_w         = st.max_w,
            .has_tuner     = st.has_tuner,
            .have_tuner    = st.have_tuner,
            .tuner_on      = st.tuner_on,
            .has_squelch   = st.has_squelch,
            .have_squelch  = st.have_squelch,
            .squelch_pct   = st.squelch_pct,
            .reflector     = st.reflector,
            .connecting    = (st.link == RADIO_LINK_CONNECTING ||
                              st.link == RADIO_LINK_GREETING),
            .tg            = st.tg,
            .talker_ms     = st.talker_ms,
            .tg_locked     = st.tg_locked,
            .muted         = st.muted,
            .rx_level_db   = st.rx_level_db,
            .rit_hz        = st.rit_hz,
            .smeter_dbm    = st.smeter_dbm,
            .tx_mic_dbm    = st.tx_mic_dbm,
            .tx_fwd_w      = st.tx_fwd_w,
            .tx_peak_w     = st.tx_peak_w,
            .tx_swr        = st.tx_swr,
            .tx_alc        = st.tx_alc,
            /* Follow the RADIO, not just our own PTT. MOX from the desktop,
             * another TCI client, or a foot switch all key the transmitter,
             * and a control head that shows RX while the rig is transmitting
             * is worse than useless. st.tx is the server's reported state. */
            /* ...but only while we can still SEE the radio. st.tx is the
             * server's last reported state, and a dead link freezes it: the
             * knob went on painting the transmit face, claiming the rig was
             * keyed, for as long as the socket took to fail. Claiming TX is
             * a claim about the radio, and with no link there is nothing
             * behind it -- the warning is the honest thing to show. Our own
             * PTT is aborted by the ladder long before this matters. */
            .tx            = (st.tx && link_ok),
            /* Only IDLE counts as "someone else". During our own RELEASING --
             * between sending trx:false and the confirmation arriving -- the
             * radio is still transmitting and the state is not PTT_ON, which
             * briefly and wrongly read as a remote transmission. */
            .tx_remote     = (st.tx && link_ok && st.ptt_state == PTT_IDLE),
            .keyed         = st.ptt_state != PTT_IDLE,
            .link_ok       = link_ok,
            .slice_locked  = st.slice_locked,
            .may_key       = (st.permit == PERMIT_ALL),
            .warn          = warn,
        };
        strlcpy(u.agc, st.agc, sizeof u.agc);
        u.have_snr = st.have_snr;
        u.snr_db   = st.snr_db;
        u.n_gain_names = st.n_gain_names < UI_GAIN_NAMES ? st.n_gain_names : UI_GAIN_NAMES;
        memcpy(u.gain_names, st.gain_names, sizeof u.gain_names);
        strlcpy(u.mem_name, st.mem_name, sizeof u.mem_name);
        strlcpy(u.ant_names, st.ant_names, sizeof u.ant_names);
        strlcpy(u.tx_ant_names, st.tx_ant_names, sizeof u.tx_ant_names);
        strlcpy(u.tg_name, st.tg_name, sizeof u.tg_name);
        strlcpy(u.talker, st.talker, sizeof u.talker);
        strlcpy(u.talker_info, st.talker_info, sizeof u.talker_info);
        strlcpy(u.last_talker, st.last_talker, sizeof u.last_talker);
        strlcpy(u.server, st.server, sizeof u.server);
        /* A telephone's call (the phone firmware; zeros on the others). */
        u.call    = st.call;
        u.call_ms = st.call_ms;
        u.call_hd = st.call_hd;
        u.n_fav   = st.n_fav;
        u.n_missed = st.n_missed;
        strlcpy(u.call_why, st.call_why, sizeof u.call_why);
        strlcpy(u.peer, st.peer, sizeof u.peer);
        strlcpy(u.peer_num, st.peer_num, sizeof u.peer_num);
        strlcpy(u.fav_num, st.fav_num, sizeof u.fav_num);
#if VFO_HAS_SDR
        u.n_sdr = (uint8_t)sdr_count();
        for (int i = 0; i < u.n_sdr && i < UI_SDR_MAX; i++) {
            sdr_cfg_t c;
            if (sdr_get(i, &c)) strlcpy(u.sdr_name[i], c.name[0] ? c.name : c.host, sizeof u.sdr_name[i]);
        }
        u.rxsrc         = (int8_t)sdr_rx_selected();
        u.sdr_streaming = sd.streaming;
        u.sdr_trouble   = sd.trouble;
        u.sdr_dbm       = sd.smeter_dbm;
        strlcpy(u.sdr_note, sd.note, sizeof u.sdr_note);
        u.balance       = sdr_rx_balance();
#else
        u.rxsrc = -1;
#endif
#if VFO_RADIO_UBERSDR
        /* The slab: the spots and voices on the band, a second at a time or
         * when they change; the swipe from the right, SSTV. */
        {
            static uint32_t spot_seq = 0xFFFFFFFF;
            static int64_t  spots_at, spots_f;
            EXT_RAM_BSS_ATTR static uber_spot_t sp[UBER_SPOTS];
            EXT_RAM_BSS_ATTR static ui_spot_t us[UI_SPOTS_MAX];
            uint32_t seq = 0;
            uber_spots(NULL, 0, &seq);
            const int64_t now = esp_timer_get_time();
            if (seq != spot_seq || now - spots_at > 1000000 || st.f_display != spots_f) {
                spot_seq = seq;
                spots_at = now;
                spots_f  = st.f_display;
                const int n = uber_spots(sp, UBER_SPOTS < UI_SPOTS_MAX ? UBER_SPOTS : UI_SPOTS_MAX, NULL);
                for (int i = 0; i < n; i++) {
                    strlcpy(us[i].call, sp[i].call, sizeof us[i].call);
                    us[i].hz = sp[i].hz;
                    strlcpy(us[i].mode, sp[i].mode, sizeof us[i].mode);
                    const unsigned a = sp[i].age_s;
                    char age[12] = "";
                    if (a >= 3600)   snprintf(age, sizeof age, "%uh", a / 3600);
                    else if (a >= 60) snprintf(age, sizeof age, "%um", a / 60);
                    else if (a)      snprintf(age, sizeof age, "%us", a);
                    us[i].heard = sp[i].heard;
                    if (sp[i].kind == 'C')
                        snprintf(us[i].what, sizeof us[i].what, "CW %u wpm %d dB%s%s",
                                 (unsigned)sp[i].wpm, sp[i].snr, age[0] ? "  " : "", age);
                    else if (sp[i].kind == 'V')
                        snprintf(us[i].what, sizeof us[i].what, "voice %d dB", sp[i].snr);
                    else if (sp[i].heard)
                        snprintf(us[i].what, sizeof us[i].what, "DX%s%s  heard %d dB",
                                 age[0] ? " " : "", age, sp[i].snr);
                    else
                        snprintf(us[i].what, sizeof us[i].what, "DX%s%s", age[0] ? "  " : "", age);
                }
                ui_set_spots(us, (uint8_t)n);
            }
            static uber_info_t in;          /* static: this stack is tight */
            uber_info(&in);
            u.has_spots = in.spots || in.voice;
            u.n_sstv    = (int16_t)uber_sstv_count();
            /* A guest's time left, at the slab's left end. */
            char why;
            u.left_s    = uber_time_left(&why);
            u.have_left = u.left_s >= 0;
            u.left_idle = why == 'I';
        }
#endif
#if !VFO_RADIO_SETUP && !VFO_RADIO_SVXCONNECT && !VFO_RADIO_PHONE
        /* The radios to choose from with a swipe up: not over the cable,
         * which reaches one computer or one radio. */
        {
            const int nd = s_on_usb ? 0 : net_prov_radio_count();
            const int nf = s_on_usb ? 0 : radio_found_count();
            u.n_radios        = (uint8_t)(nd + nf < UI_RADIOS_MAX ? nd + nf : UI_RADIOS_MAX);
            /* The configured ones on the LAN, and the found ones that are. */
            u.n_radios_direct = (uint8_t)(nd + (s_on_usb ? 0 : radio_found_lan()));
            strlcpy(u.radio_via, radio_found_via(), sizeof u.radio_via);
            u.radio_sel = (int8_t)(radio_found_active() >= 0 ? nd + radio_found_active()
                                                             : net_prov_radio_active());
            for (int i = 0; i < u.n_radios; i++) {
                static net_radio_t r;       /* static: this stack is tight */
                if (i < nd) {
                    /* A new knob's one entry, with no address yet: said so,
                     * beside a radio it found on the LAN. */
                    if (net_prov_radio_get(i, &r))
                        strlcpy(u.radio_name[i], r.name[0] ? r.name : r.host[0] ? net_prov_host_shown(r.host)
                                                                                : "NO ADDRESS",
                                sizeof u.radio_name[i]);
                } else {
                    radio_found_get(i - nd, u.radio_name[i], sizeof u.radio_name[i]);
                }
            }
        }
#endif
#if !VFO_RADIO_SETUP
        /* A Bluetooth headset: its logo on the slab while one is connected; a
         * speaker: a speaker there instead -- the knob's own microphone keys.
         * Everything else that keys or mutes asks bt_link_headset_*(), which
         * say no for a speaker. Either's battery beside it, where it reports
         * one. */
        {
            static bt_link_status_t b;      /* static: this stack is tight */
            bt_link_status(&b);
            const bool conn = b.companion && b.hs.link == BTL_LINK_CONNECTED;
            u.speaker       = conn && b.hs.kind == BTL_KIND_SPEAKER;
            u.headset       = conn && !u.speaker;
            u.headset_muted = u.headset && b.hs.mic == 0;
            u.headset_raise = u.headset && s_hs_raise;
            const int batt  = bt_link_battery();
            u.have_batt     = conn && batt >= 0;
            u.batt          = batt >= 0 ? (uint8_t)batt : 0;
        }
#endif
        /* The knob's own power, a reading a second (board_power_poll): its
         * battery on the face while it runs on it. In the log once a minute
         * -- the rail as read, smoothed, and the charge shown, a run-down's
         * curve -- and the moment it is plugged in or pulled out. */
        {
            static uint8_t said_src = KNOB_PWR_UNKNOWN;
            static int64_t said_us;
            board_power_poll();
            board_power_t pw;
            board_power_get(&pw);
            const int64_t now_us = esp_timer_get_time();
            if (pw.src != KNOB_PWR_UNKNOWN && (pw.src != said_src || now_us - said_us >= 60000000)) {
                const char *what = said_src == KNOB_PWR_UNKNOWN || pw.src == said_src ? ""
                                   : pw.src == KNOB_PWR_USB ? " -- plugged in" : " -- unplugged";
                if (pw.src == KNOB_PWR_USB)
                    ESP_LOGI(TAG, "[PWR] rail=%d mV: on USB%s", pw.mv, what);
                else
                    ESP_LOGI(TAG, "[PWR] rail=%d mV, %d smoothed: battery %d %%%s", pw.mv, pw.smooth_mv,
                             pw.pct, what);
                said_src = pw.src;
                said_us  = now_us;
            }
            u.knob_batt = pw.src == KNOB_PWR_BATTERY && pw.pct >= 0;
            u.knob_pct  = u.knob_batt ? (uint8_t)pw.pct : 0;
        }
        ui_update(&u);
    }
}

/* Does anything answer on this address and port? A plain TCP connect, used to
 * choose a transport.
 *
 * "Is the USB netif up?" is the wrong question: it comes up as soon as the
 * cable has power, including a charger with no computer behind it, and
 * esp_netif's DHCP *server* raises no event when it hands out a lease. The
 * only honest test is whether AetherSDR actually answers over the cable. (Not
 * the Xiegu's: its radio speaks UDP -- see pick_transport.) */
#if CONFIG_VFO_USB_NET && !VFO_RADIO_XIEGU
static bool host_answers(const char *ip, uint16_t port, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct timeval tv = { .tv_sec  =  timeout_ms / 1000,
                          .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = inet_addr(ip);
    bool ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}
#endif

/* Picks the transport for the radio link, cable first.
 *
 * The cable is preferred because it is the whole point of the USB build: the
 * machined case makes 2.4 GHz unreliable, and with both interfaces up the
 * routing table would send TCI over WiFi regardless -- the USB subnet does not
 * contain AetherSDR's LAN address, so the knob has to talk to the host's
 * cable-side address instead. WiFi is held back briefly so that a cable that
 * is merely slower to come up still wins. */
static const char *pick_transport(const vfo_cfg_t *cfg, char *ip, size_t iplen,
                                  bool *via_usb)
{
#if CONFIG_VFO_USB_NET
    if (usb_net_host_present()) {
#if VFO_RADIO_XIEGU
        /* The radio itself is on the cable -- the knob plugged into its USB
         * host port, as its network adapter -- at the address the knob's
         * DHCP server gives it. Its WFSERVER speaks UDP, so there is no TCP
         * port to knock on first; the client's own retries cover a server
         * not up yet. WiFi comes up beside the cable when one is set: the
         * radio has no browser, so the configuration page and the log are
         * reachable only from the LAN. It stays the default route, the USB
         * link's priority being below WiFi's; only the radio is on the cable. */
        if (!s_wifi_started && cfg->ssid[0]) {
            s_wifi_started = true;
            if (net_prov_wifi_start() != ESP_OK)
                ESP_LOGE(TAG, "wifi     FAILED beside the cable -- the radio carries on");
        }
        const char *radio = usb_net_host();
        if (!radio) return NULL;
        ESP_LOGI(TAG, "--- transport: USB cable, the radio at %s ---", radio);
        *via_usb = true;
        return radio;
#else
        if (host_answers(usb_net_host(), cfg->radio_port, 500)) {
            ESP_LOGI(TAG, "--- transport: USB cable (%s) ---", usb_net_host());
            *via_usb = true;
            return usb_net_host();
        }
        /* A computer is on the cable, so the cable IS the transport; AetherSDR
         * not being up yet is a temporary condition, not a reason to change
         * networks. Falling back here was actively harmful: WiFi's driver
         * takes the internal RAM the WebSocket client needs, so the knob ended
         * up on a network it could not open a socket on -- "Error create
         * websocket task", retrying forever with 9 kB free. Waiting costs
         * nothing; switching costs the link. */
        return NULL;
#endif
    }
    /* Give the cable until a little after it is due before settling for WiFi. */
    /* With no computer on the S3's side there is nothing to wait for. */
    if (atomic_load(&s_cable) != CABLE_NONE &&
        esp_timer_get_time() < USB_GRACE_US) return NULL;
#endif
    /* No network configured, nothing to join: the radio would only retry an
     * empty SSID every five seconds, on memory the board is short of. The
     * screen says what to do instead. */
    if (!cfg->ssid[0]) return NULL;
    /* Only now is WiFi worth its memory. Started once, whichever way it comes
     * up: a second start would register the driver and its handlers again. */
    if (!s_wifi_started) {
        s_wifi_started = true;
        esp_err_t werr = net_prov_wifi_start();
        if (werr != ESP_OK) {
            ESP_LOGE(TAG, "wifi     FAILED: %s -- continuing offline",
                     esp_err_to_name(werr));
            /* Said on the glass, or the face just shows NO LINK for ever. */
            ui_setup_show("NO WIFI", "The knob's WiFi\nwould not start.\nRestart the knob.");
        }
        return NULL;                       /* give it a moment to associate */
    }
    if (!net_prov_is_connected()) return NULL;
    /* A radio the client found itself (SmartLink): no address of ours. */
    if (radio_found_active() >= 0) {
        strlcpy(ip, radio_found_via(), iplen);
        ESP_LOGI(TAG, "--- transport: WiFi (%s) ---", ip);
        return ip;
    }
#if VFO_RADIO_PHONE
    /* The telephone's SIP account is its own (the configuration page's
     * Telephone section): the client looks its server up itself. */
    strlcpy(ip, "SIP", iplen);
    ESP_LOGI(TAG, "--- transport: WiFi (SIP) ---");
#elif VFO_RADIO_SVXCONNECT
    /* A reflector is named, and its name is looked up by the client itself:
     * an SRV record comes first, and says which host and port to use. */
    strlcpy(ip, cfg->radio_host, iplen);
    ESP_LOGI(TAG, "--- transport: WiFi (reflector %s) ---", cfg->radio_host);
#elif VFO_RADIO_UBERSDR
    /* An UberSDR by its name: through its tunnel TLS checks the certificate
     * against it, and the tunnel finds the receiver by it; on a LAN it is
     * reached in the clear. The client looks it up itself, and reads its
     * scheme -- https:// or http:// -- from the address, which is handed
     * over whole: a tunnel's name can be longer than ip[]. */
    if (!cfg->radio_host[0]) {
        static bool said;
        if (!said) ESP_LOGW(TAG, "  no receiver set: see the configuration page");
        said = true;
        return NULL;
    }
    ESP_LOGI(TAG, "--- transport: WiFi (UberSDR %s) ---", cfg->radio_host);
    return cfg->radio_host;
#else
    if (!cfg->radio_host[0]) {
        /* The multiflex firmware has no default: the radio's address is
         * given on the configuration page. Said once, not every retry. */
        static bool said;
        if (!said) ESP_LOGW(TAG, "  no radio address set: see the configuration page");
        said = true;
        return NULL;
    }
    if (net_prov_resolve(ip, iplen) != ESP_OK) {
        ESP_LOGE(TAG, "  cannot resolve %s", cfg->radio_host);
        return NULL;
    }
    ESP_LOGI(TAG, "--- transport: WiFi (%s -> %s) ---", cfg->radio_host, ip);
#endif
    return ip;
}

/* --- updates, asked on the dial ------------------------------------------
 *
 * On WiFi the knob can reach the release server itself, so it asks: once at
 * boot, before TCI starts, and again whenever the periodic check finds
 * something newer. A tap on the question installs; anything else -- ten
 * seconds, the knob, a tap elsewhere -- lets the dial carry on.
 *
 * Installing needs internal RAM that a live TCI session on WiFi does not
 * leave (the image is verified with RSA and written with an internal stack),
 * so a yes given mid-session restarts the knob and installs at boot, before
 * TCI is started. The flag lives in RTC memory: it survives that restart, and
 * a power cut clears it rather than leaving an update pending. */
#define UPDATE_ON_BOOT 0x55504454u               /* "UPDT" */
RTC_NOINIT_ATTR static uint32_t s_update_on_boot;
/* Taken from the RTC flag once, first thing in app_main(): a yes covers the
 * very next boot and no later one, however that boot turns out. */
static bool     s_update_accepted;
static uint32_t s_ota_seen;                     /* last check acted upon */

/* Waits up to `ms` for a check started now; false if it is not back yet. A
 * check still out keeps going, and net_task asks about its result later. */
static bool check_now(ota_status_t *o, int ms)
{
    ota_get_status(o);
    const uint32_t before = o->checks;
    if (ota_start_check(false) != ESP_OK) return false;
    for (int t = 0; t < ms; t += 200) {
        vTaskDelay(pdMS_TO_TICKS(200));
        ota_get_status(o);
        if (o->checks != before) return true;
    }
    return false;
}

static bool ask_update(const ota_status_t *o)
{
    if (!ui_ask_update(o->available, o->running, false)) return false;   /* no dial */
    /* The dial answers no by itself after ten seconds; this bound only
     * matters if there is no dial to ask on. */
    for (int i = 0; i < 120; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        const int a = ui_take_update_answer();
        if (a) return a > 0;
    }
    return false;
}

/* Downloads, verifies and installs, then restarts into it. Returns only if
 * that failed, with the dial back as it was. */
static void install_update(void)
{
    ESP_LOGW(TAG, "installing the update the operator accepted");
    ui_updating_show();
    /* An image still on trial cannot start another update -- esp_ota_begin()
     * refuses until boot_ok_cb confirms it, 20 s into the boot. The yes has
     * confirmed it; should that not have taken, wait for the timer rather
     * than fail. */
    boot_ok_now();
    esp_ota_img_states_t trial;
    for (int i = 0; i < 120 &&
         esp_ota_get_state_partition(esp_ota_get_running_partition(),
                                     &trial) == ESP_OK &&
         trial == ESP_OTA_IMG_PENDING_VERIFY; i++)
        vTaskDelay(pdMS_TO_TICKS(250));
    /* The check that found it may still be on the worker, looking up the
     * second chip's firmware after it -- 8 s a stalled read, and once more
     * on a new connection: an install starts once it is done. */
    for (int i = 0; i < 120 && ota_busy(); i++) vTaskDelay(pdMS_TO_TICKS(250));
    ota_status_t o;
    ota_get_status(&o);
    const uint32_t before = o.checks;
    if (ota_start_check(true) == ESP_OK) {
        /* Finished when the worker has counted its run -- not when the phase
         * looks settled, which it also does before the worker has started. */
        do {
            vTaskDelay(pdMS_TO_TICKS(250));
            ota_get_status(&o);
            ui_updating_progress(o.percent);
        } while (o.checks == before);
        if (o.phase == OTA_DONE_REBOOT_NEEDED) {
            ui_updating_result(true, "Restarting");
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        }
    }
    ui_updating_result(false, "Update failed");
    vTaskDelay(pdMS_TO_TICKS(2500));
    ui_updating_hide();
}

/* Another radio's firmware, installed now: the firmware picker (the setup
 * firmware) from a radio's, or a radio's from the picker. The settings stay,
 * in NVS. Returns only if it failed, with the reason on the dial. */
static void install_switch(const char *radio)
{
    ESP_LOGW(TAG, "installing the %s firmware", radio);
    ui_updating_show();
    s_fill_stop = true;
    for (int i = 0; i < 40 && s_fill_running; i++) vTaskDelay(pdMS_TO_TICKS(250));
    /* Chosen on the dial, so the running image works: confirmed now, where it
     * used to sit at 0% until the timer did it. The wait is for the timer,
     * should that not have taken: an image on trial cannot start an install. */
    boot_ok_now();
    esp_ota_img_states_t trial;
    for (int i = 0; i < 120 &&
         esp_ota_get_state_partition(esp_ota_get_running_partition(),
                                     &trial) == ESP_OK &&
         trial == ESP_OTA_IMG_PENDING_VERIFY; i++)
        vTaskDelay(pdMS_TO_TICKS(250));
    ota_status_t o;
    ota_get_status(&o);
    const uint32_t before = o.checks;
    const esp_err_t se = ota_start_switch(radio);
    if (se == ESP_OK) {
        do {
            vTaskDelay(pdMS_TO_TICKS(250));
            ota_get_status(&o);
            ui_updating_progress(o.percent);
        } while (o.checks == before);
        if (o.phase == OTA_DONE_REBOOT_NEEDED) {
            ui_updating_result(true, "Restarting");
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        }
        ESP_LOGE(TAG, "firmware switch failed: %s", o.message);
    } else {
        /* Not started: its task's 8 kB stack is internal RAM, in one piece. */
        ESP_LOGE(TAG, "firmware switch did not start: %s (free internal %u, largest %u)",
                 esp_err_to_name(se), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    ui_updating_result(false, "Download failed");
    vTaskDelay(pdMS_TO_TICKS(2500));
    ui_updating_hide();
}

/* At boot on WiFi, before TCI starts: the one moment there is RAM to spare.
 * A plain look is bounded to a few seconds, so a LAN with no way out costs
 * TCI little; an install already accepted gets as long as it needs. */
static void boot_update_check(void)
{
    const bool accepted = s_update_accepted;
    s_update_accepted = false;
    if (!accepted && !net_prov_ota_hours()) return;   /* checking is off */
    if (accepted) ui_updating_show();
    ota_status_t o;
    const bool done = check_now(&o, accepted ? 60000 : 6000);
    if (done) s_ota_seen = o.checks;                /* dealt with here */
    const bool avail = done && o.newer && o.phase == OTA_IDLE;
    if (accepted) {
        if (avail) {
            install_update();
            return;
        }
        /* Never drop an operator's yes without a word. */
        ui_updating_result(false, done && o.phase == OTA_UP_TO_DATE
                                      ? "No update found"
                                      : "Update server unreachable");
        vTaskDelay(pdMS_TO_TICKS(2500));
        ui_updating_hide();
        return;
    }
    if (avail && ask_update(&o)) install_update();
}

#if VFO_RADIO_SETUP
/* --- the setup firmware ----------------------------------------------------
 *
 * For a knob with no computer to set it up from: its WiFi from a phone, then
 * the firmware for its radio, chosen on the dial and installed. The WiFi
 * stays for the firmware installed, which finds it where every firmware keeps
 * it, and a radio's firmware comes back here from its address card: a finger
 * held three seconds on the S-meter or the card, and a turn of the knob.
 *
 *  1. The network already stored, if there is one: 25 s to join it.
 *  2. Otherwise the knob's own hotspot, VFOKnob, open. A phone that joins it
 *     is sent to the WiFi page by itself (a captive portal), chooses the
 *     network, gives the password, and sees the knob join it.
 *  3. Online: the firmwares published for the knob, one a detent on the dial
 *     with its version. A tap on the panel installs one and restarts into it;
 *     the last choice, WIFI, goes back to 2 for another network.
 */
#define SETUP_AP_NAME "VFOKnob"

/* Only for when the release server's index cannot be had: the radios this
 * build knew of. The index is what counts -- see setup_pick(). */
static const struct { const char *radio, *name; } FIRMWARES[] = {
    { "aethersdr",  "AetherSDR" },
    { "icom",       "Icom"      },
    { "multiflex",  "FlexRadio" },
    { "ubersdr",    "UberSDR"   },
    { "svxconnect", "SVXConnect" },
    { "phone",      "Telephone" },
};

static void setup_wifi_page(void)
{
    net_prov_hold_station(true);
    net_prov_ap_start(SETUP_AP_NAME);
    ui_setup_show("WIFI SETUP", "Join the WiFi network\n" SETUP_AP_NAME "\nwith your phone, then\nchoose your network on\nthe page that opens.");
    net_join_t shown = NET_JOIN_IDLE;
    for (;;) {
        char ssid[33], why[48], msg[160];
        const net_join_t st = net_prov_join_state(ssid, sizeof ssid, why, sizeof why);
        if (st != shown) {
            shown = st;
            if (st == NET_JOIN_TRYING) {
                snprintf(msg, sizeof msg, "Joining\n%s", ssid);
                ui_setup_show("WIFI SETUP", msg);
            } else if (st == NET_JOIN_FAILED) {
                snprintf(msg, sizeof msg, "Could not join\n%s:\n%s\nTry again on the phone.", ssid, why);
                ui_setup_show("WIFI SETUP", msg);
            } else if (st == NET_JOIN_OK) {
                net_prov_join_keep();
                snprintf(msg, sizeof msg, "Connected to\n%s", ssid);
                ui_setup_show("WIFI SETUP", msg);
                /* Time for the phone's page to say so before the hotspot goes. */
                vTaskDelay(pdMS_TO_TICKS(8000));
                net_prov_ap_stop();
                net_prov_hold_station(false);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* The firmwares published for the knob, one a detent, each with its version;
 * the last choice sets up the WiFi again. Returns when that one is chosen.
 *
 * The list is the release server's (firmware/index.json, written by
 * tools/release.sh), not this build's: a radio whose firmware is published
 * after this one was made is offered too. */
/* Every firmware published onto the SD card, in the background while the
 * list is up -- this one too, for the way back to it from a radio's. */
#define FILL_MAX (UI_CHOICES + 2)
static char s_fill[FILL_MAX][16];
static int  s_nfill;

static void card_fill_task(void *arg)
{
    (void)arg;
    int ok = 0;
    for (int i = 0; i < s_nfill && !s_fill_stop; i++)
        if (ota_cache(s_fill[i], &s_fill_stop) == ESP_OK) ok++;
    ESP_LOGI(TAG, "SD card: %d of %d firmwares on it%s", ok, s_nfill, s_fill_stop ? " (stopped)" : "");
    sdc_log_state();
    s_fill_running = false;
    vTaskDelete(NULL);
}

static void setup_pick(void)
{
    static char titles[UI_CHOICES][12], names[UI_CHOICES][24], radios[UI_CHOICES][16];
    static bool filling;
    for (;;) {
        ui_setup_show("FIRMWARE", "Looking up\nthe firmwares...");
        uint8_t n = 0;
        char *idx = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        /* No update server: what the SD card holds, from its copy of the
         * index -- it installs from the card. */
        bool card = false;
        esp_err_t ie = idx ? ota_fetch_index(idx, 4096) : ESP_ERR_NO_MEM;
        if (ie != ESP_OK && idx && ota_card_index(idx, 4096) == ESP_OK) {
            ie   = ESP_OK;
            card = true;
        }
        if (ie == ESP_OK) {
            cJSON *root = cJSON_Parse(idx);
            const cJSON *f;
            if (!card) s_nfill = 0;
            cJSON_ArrayForEach(f, cJSON_GetObjectItem(root, "firmwares")) {
                const char *r  = cJSON_GetStringValue(cJSON_GetObjectItem(f, "radio"));
                const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(f, "name"));
                const char *v  = cJSON_GetStringValue(cJSON_GetObjectItem(f, "version"));
                /* The second chip's firmware is no radio's, and is never in
                 * this list (tools/release.sh): should it ever be, it is
                 * neither offered nor installed here. */
                if (r && strcmp(r, "companion") == 0) continue;
                if (!card && r && s_nfill < FILL_MAX) strlcpy(s_fill[s_nfill++], r, sizeof s_fill[0]);
                /* Not this one: it is what is running. */
                if (!r || !v || strcmp(r, "setup") == 0 || n >= UI_CHOICES - 1) continue;
                char cv[16];
                if (card && !ota_card_has(r, cv, sizeof cv)) continue;
                strlcpy(titles[n], "INSTALL", sizeof titles[n]);
                snprintf(names[n], sizeof names[n], "%s %s", nm && *nm ? nm : r, card ? cv : v);
                strlcpy(radios[n], r, sizeof radios[n]);
                n++;
            }
            /* The second chip's firmware onto the card too, under its own
             * key: the radio's firmware installed from here finds it there
             * and hands it to the chip. This firmware never sends it. */
            if (!card && cJSON_GetObjectItem(root, "companion") && s_nfill < FILL_MAX)
                strlcpy(s_fill[s_nfill++], "companion", sizeof s_fill[0]);
            cJSON_Delete(root);
            if (card) ESP_LOGW(TAG, "no update server: the SD card's firmwares");
        }
        free(idx);
        if (!card && n && !filling && !s_fill_running) {
            /* Its stack in PSRAM: TLS and the card, never the flash. The
             * install that stops it needs 8 kB of internal RAM in one piece. */
            EXT_RAM_BSS_ATTR static StackType_t fill_stack[8192];
            static StaticTask_t fill_tcb;
            filling        = true;
            s_fill_stop    = false;
            s_fill_running = xTaskCreateStaticPinnedToCore(card_fill_task, "cardfill", sizeof fill_stack,
                                                           NULL, 2, fill_stack, &fill_tcb, 0) != NULL;
        }
        if (!n) {
            ESP_LOGW(TAG, "no firmware index: the radios this build knows");
            for (size_t i = 0; i < sizeof FIRMWARES / sizeof FIRMWARES[0] && n < UI_CHOICES - 1; i++) {
                char ver[16];
                if (ota_fetch_version(FIRMWARES[i].radio, ver, sizeof ver) != ESP_OK) continue;
                strlcpy(titles[n], "INSTALL", sizeof titles[n]);
                snprintf(names[n], sizeof names[n], "%s %s", FIRMWARES[i].name, ver);
                strlcpy(radios[n], FIRMWARES[i].radio, sizeof radios[n]);
                n++;
            }
        }
        if (!n) {
            ui_setup_show("FIRMWARE", "None found.\nIs the network online?\nTrying again.");
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }
        strlcpy(titles[n], "WIFI", sizeof titles[n]);
        strlcpy(names[n], "Set up again", sizeof names[n]);
        radios[n++][0] = 0;
        /* The chooser first: the text then goes under its panel. */
        ui_ask_choice(titles, names, n, 0);
        ui_setup_show("FIRMWARE", card ? "From the SD card:\nturn to your radio,\nthen tap to install."
                                       : "Turn to your radio,\nthen tap to install.");
        int a;
        while ((a = ui_take_choice()) < 0) {
            net_prov_tick();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!radios[a][0]) return;                /* the WiFi, again */
        install_switch(radios[a]);                /* returns only if it failed */
    }
}

static void setup_task(void *arg)
{
    (void)arg;
    const vfo_cfg_t *cfg = net_prov_cfg();
    ui_setup_show("VFO-KNOB", "Starting");
    /* Provisioned just now (tools/install-setup.sh): the SD card emptied --
     * the board's demo off it -- this once, and never otherwise. The script
     * then puts the firmwares on it, through the cable (card_session). */
    if (s_flashed_now) {
        ui_setup_show("VFO-KNOB", "Emptying the\nSD card");
        const esp_err_t e = sdc_format();
        ESP_LOGW(TAG, "provisioning: the SD card %s",
                 e == ESP_OK ? "emptied" : e == ESP_ERR_NOT_FOUND ? "is not there" : esp_err_to_name(e));
        ui_setup_show("VFO-KNOB", "Starting");
    }
    s_card_ready = true;
    if (net_prov_wifi_start() != ESP_OK) {
        ui_setup_show("VFO-KNOB", "WiFi would not start.");
        vTaskDelete(NULL);
    }
    bool online = false;
    if (cfg->ssid[0]) {
        char msg[80];
        snprintf(msg, sizeof msg, "Joining\n%s", cfg->ssid);
        ui_setup_show("WIFI", msg);
        for (int i = 0; i < 250 && !net_prov_is_connected(); i++) vTaskDelay(pdMS_TO_TICKS(100));
        online = net_prov_is_connected();
    }
    for (;;) {
        if (!online) setup_wifi_page();
        setup_pick();
        online = false;                           /* WIFI chosen: set it up again */
    }
}
/* The radios' supervisor, and the question it relays, have nothing to do
 * here: kept compiled, not run. */
#define RADIO_ONLY_FN __attribute__((unused))
#else
#define RADIO_ONLY_FN
#endif

/* --- no WiFi in reach: the knob's own hotspot ------------------------------
 *
 * The setup firmware's WiFi setup, in every firmware: with none of the
 * knob's networks joined WIFI_SETUP_AFTER_US after WiFi started -- or none
 * known at all -- up come the VFOKnob hotspot and its page, and WIFI SETUP
 * over the face. The station goes on trying the networks it knows meanwhile,
 * whenever no phone is on the hotspot. A known network in reach, or one given
 * on the phone -- kept beside the others -- and the hotspot goes, the face
 * comes back, and the radio after it. Never while the cable is the way. */
#define WIFI_SETUP_AFTER_US (25 * 1000 * 1000)
#define WIFI_SETUP_AP       "VFOKnob"

/* True while the WiFi setup is up: the net task waits on it. */
RADIO_ONLY_FN static bool wifi_setup(void)
{
    static bool       on;
    static int64_t    down_since, done_at;
    static net_join_t shown;
    const int64_t     now = esp_timer_get_time();
    char ssid[33], why[48], msg[160];

    net_prov_tick();
    bool cable = s_on_usb;
#if CONFIG_VFO_USB_NET
    /* A computer on the cable is the way -- or, until it is due, may be. */
    cable = cable || usb_net_host_present();
    if (!on && atomic_load(&s_cable) != CABLE_NONE && now < USB_GRACE_US) return false;
#endif
    if (!on) {
        if (cable || net_prov_is_connected()) {
            down_since = 0;
            return false;
        }
        if (!down_since) down_since = now;
        const bool none = net_prov_wifi_count() == 0;
        if (!none && (!s_wifi_started || now - down_since < WIFI_SETUP_AFTER_US)) return false;
        if (!s_wifi_started) {
            s_wifi_started = true;
            if (net_prov_wifi_start() != ESP_OK) {
                ESP_LOGE(TAG, "wifi     FAILED: no WiFi setup either");
                ui_setup_show("NO WIFI", "The knob's WiFi\nwould not start.\nRestart the knob.");
                return false;
            }
        }
        if (net_prov_ap_start(WIFI_SETUP_AP) != ESP_OK) return false;
        on      = true;
        shown   = NET_JOIN_IDLE;
        done_at = 0;
        ESP_LOGW(TAG, "%s: WiFi setup on the knob's own network, " WIFI_SETUP_AP,
                 none ? "no WiFi network known" : "none of the knob's WiFi networks in reach");
        ui_setup_show("WIFI SETUP", none
            ? "Join the WiFi network\n" WIFI_SETUP_AP "\nwith your phone, then\nchoose your network on\nthe page that opens."
            : "None of its networks\nis in reach. Join\n" WIFI_SETUP_AP " with your\nphone to add one,\nor wait: it keeps looking.");
        return true;
    }
    if (cable) {
        /* A computer came onto the cable after all. */
        net_prov_ap_stop();
        ui_setup_hide();
        on = false;
        down_since = 0;
        return false;
    }
    const net_join_t js = net_prov_join_state(ssid, sizeof ssid, why, sizeof why);
    if (js != shown) {
        shown = js;
        if (js == NET_JOIN_TRYING) {
            snprintf(msg, sizeof msg, "Joining\n%s", ssid);
            ui_setup_show("WIFI SETUP", msg);
        } else if (js == NET_JOIN_FAILED) {
            snprintf(msg, sizeof msg, "Could not join\n%s:\n%s\nTry again on the phone.", ssid, why);
            ui_setup_show("WIFI SETUP", msg);
        } else if (js == NET_JOIN_OK) {
            net_prov_join_keep();
            snprintf(msg, sizeof msg, "Connected to\n%s", ssid);
            ui_setup_show("WIFI SETUP", msg);
            done_at = now + 8000000;        /* the phone's page says so first */
        }
    }
    if (net_prov_is_connected() && !done_at) {
        /* One of the networks it knows, in reach after all. */
        snprintf(msg, sizeof msg, "Connected to\n%s", net_prov_wifi_now());
        ui_setup_show("WIFI SETUP", msg);
        done_at = now + 2000000;
    }
    if (done_at && now >= done_at) {
        net_prov_ap_stop();
        ui_setup_hide();
        on = false;
        down_since = 0;
        done_at = 0;
        ESP_LOGI(TAG, "WiFi setup done: on \"%s\"", net_prov_wifi_now());
        return false;
    }
    return true;
}

/* --- the second chip's firmware --------------------------------------------
 *
 * The one update the knob installs without asking (ota.h, bt_link.h): the
 * second chip's firmware, a release newer than the chip's, sent at a quiet
 * moment. ota.c finds it -- with the knob's own check, or on the SD card --
 * and fetches it into PSRAM: in the boot window, before the radio's client
 * starts and takes the internal RAM a download needs, or later beside the
 * session while there is RAM to spare. bt_link sends it. What the knob
 * remembers of each keeps one that failed from coming back: never after a
 * real failure, and no more than BT_UPD_TRIES restarts into it after
 * harmless ones -- the power gone during its trial, a knob that restarted
 * and never said KEEP. */

/* The record (bt_link_upd_record_t) in NVS, btlink/comp, a few writes a
 * release: from the supervisor alone, its stack internal, and never during
 * an over or a call -- a flash write holds the audio up. */
typedef struct __attribute__((packed)) {
    uint8_t sha8[8];
    uint8_t tries, result, why;
    char    ver[16];
} comp_disk_t;
/* The supervisor's alone, in PSRAM: internal RAM is what a download, and
 * the radio's own TLS, run short of. */
EXT_RAM_BSS_ATTR static bt_link_upd_record_t s_comp_rec;
static bool     s_comp_have, s_comp_dirty;
EXT_RAM_BSS_ATTR static uint32_t s_comp_dones, s_comp_seq;   /* bt_link's counters, as last seen */
EXT_RAM_BSS_ATTR static int64_t  s_comp_fetched_at;          /* the last fetch of its image asked for */

static const char *hex16(char out[17], const uint8_t b[8])
{
    for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", b[i]);
    return out;
}

RADIO_ONLY_FN static void comp_record_load(void)
{
    nvs_handle_t h;
    comp_disk_t  d;
    size_t       n = sizeof d;
    if (nvs_open("btlink", NVS_READONLY, &h) != ESP_OK) return;
    const bool ok = nvs_get_blob(h, "comp", &d, &n) == ESP_OK && n == sizeof d;
    nvs_close(h);
    if (!ok) return;
    memset(&s_comp_rec, 0, sizeof s_comp_rec);
    memcpy(s_comp_rec.sha8, d.sha8, sizeof d.sha8);
    s_comp_rec.tries  = d.tries;
    s_comp_rec.result = d.result;
    s_comp_rec.why    = d.why;
    memcpy(s_comp_rec.ver, d.ver, sizeof d.ver);
    s_comp_have = true;
    bt_link_update_record(&s_comp_rec);
    char a[17];
    ESP_LOGI(TAG, "second chip: the knob remembers %s [%s]: the chip restarted into it %u time%s%s",
             s_comp_rec.ver, hex16(a, s_comp_rec.sha8), (unsigned)d.tries, d.tries == 1 ? "" : "s",
             d.result ? "; never to go again" : "");
}

/* bt_link's results, as they come: each restart of the chip into an image is
 * a try of it; a keep forgets it; a result no retry mends keeps it from ever
 * going again. A new image's first try, or its end, takes the record over.
 * Said to bt_link at once, to NVS when it may be written. */
static void comp_record_note(const bt_link_upd_t *u)
{
    bool changed = false;
    if (u->dones != s_comp_dones) {
        const uint32_t k = u->dones - s_comp_dones;
        s_comp_dones = u->dones;
        if (!s_comp_have || memcmp(s_comp_rec.sha8, u->to_sha, 8)) {
            memset(&s_comp_rec, 0, sizeof s_comp_rec);
            memcpy(s_comp_rec.sha8, u->to_sha, 8);
            strlcpy(s_comp_rec.ver, u->to, sizeof s_comp_rec.ver);
            s_comp_have = true;
        }
        s_comp_rec.tries = (uint8_t)(s_comp_rec.tries + k > 255 ? 255 : s_comp_rec.tries + k);
        changed = true;
    }
    if (u->seq != s_comp_seq) {
        s_comp_seq = u->seq;
        if (u->result == BT_UPD_KEPT) {
            /* Kept: the tries go -- but one never to go again stays so. */
            if (s_comp_have && (s_comp_rec.result == BT_UPD_NONE || !memcmp(s_comp_rec.sha8, u->last_sha, 8))) {
                s_comp_have = false;
                changed = true;
            }
        } else if (u->block) {
            if (!s_comp_have || memcmp(s_comp_rec.sha8, u->last_sha, 8)) {
                memset(&s_comp_rec, 0, sizeof s_comp_rec);
                memcpy(s_comp_rec.sha8, u->last_sha, 8);
                strlcpy(s_comp_rec.ver, !memcmp(u->last_sha, u->to_sha, 8) ? u->to : "?", sizeof s_comp_rec.ver);
                s_comp_have = true;
            }
            s_comp_rec.result = u->result;
            s_comp_rec.why    = u->why;
            changed = true;
        }
        /* Anything else -- stopped, refused for now, dropped -- is no try. */
    }
    if (!changed) return;
    bt_link_update_record(s_comp_have ? &s_comp_rec : NULL);
    s_comp_dirty = true;
}

static void comp_record_save(bool busy)
{
    static bool said;
    if (!s_comp_dirty || busy) return;
    nvs_handle_t h;
    esp_err_t e = nvs_open("btlink", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        if (s_comp_have) {
            comp_disk_t d = { .tries = s_comp_rec.tries, .result = s_comp_rec.result, .why = s_comp_rec.why };
            memcpy(d.sha8, s_comp_rec.sha8, sizeof d.sha8);
            /* NUL-padded, the rest of d zeroed; 16 characters may fill it. */
            memcpy(d.ver, s_comp_rec.ver, strnlen(s_comp_rec.ver, sizeof d.ver));
            e = nvs_set_blob(h, "comp", &d, sizeof d);
        } else {
            e = nvs_erase_key(h, "comp");
            if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        }
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e == ESP_OK) {
        s_comp_dirty = said = false;
    } else if (!said) {
        said = true;
        ESP_LOGW(TAG, "second chip: what the knob remembers of its updates not written: %s", esp_err_to_name(e));
    }
}

/* The knob keeps the chip up to date: it is there, has said what it runs,
 * takes updates, runs a release, and has settled -- no firmware on trial.
 * Otherwise it is left alone, which is said once for each firmware. */
static bool comp_auto(const bt_link_status_t *st, const bt_link_upd_t *u)
{
    EXT_RAM_BSS_ATTR static char said[33];
    if (!st->companion) return false;
    const bool takes = st->flags & BTL_HELLO_UPDATE;
    if (takes && !u->info) return false;          /* what it runs: not said yet */
    const bool release = takes && (u->chip.flags & BTL_INFO_RELEASE);
    if (release) return u->chip.state != BTL_RUN_TRIAL;
    if (strcmp(said, st->version) != 0) {
        strlcpy(said, st->version, sizeof said);
        /* Two calls: the log pastes its format in as it stands. */
        if (!takes) ESP_LOGW(TAG, "second chip: %s takes no updates -- it needs the bench once", st->version);
        else        ESP_LOGW(TAG, "second chip: %s is a development build: not updated automatically", st->version);
    }
    return false;
}

/* The offer is for this chip: newer than what it runs -- never older -- not
 * its own image, and nothing held against it: the chip's word, the last
 * result, the record. */
static bool comp_wanted(const ota_comp_offer_t *o, const bt_link_status_t *st, const bt_link_upd_t *u)
{
    return o->known && ota_is_newer(o->version, st->version) && memcmp(o->app_sha, u->chip.app_sha, 8) != 0 &&
           !bt_link_update_blocked(o->app_sha, NULL, 0);
}

/* What the chip runs and what there is for it, said when either changes. */
static void comp_say(const bt_link_status_t *st, const bt_link_upd_t *u, const ota_comp_offer_t *o)
{
    EXT_RAM_BSS_ATTR static uint8_t chip[8], app[8];
    static bool    said, known, card;
    if (said && !memcmp(chip, u->chip.app_sha, 8) && known == o->known && card == o->from_card &&
        !memcmp(app, o->app_sha, 8))
        return;
    said  = true;
    known = o->known;
    card  = o->from_card;
    memcpy(chip, u->chip.app_sha, 8);
    memcpy(app, o->app_sha, 8);
    char a[17];
    hex16(a, u->chip.app_sha);
    if (!o->known)
        ESP_LOGI(TAG, "second chip: %s [%s], release; no firmware for it found yet", st->version, a);
    else if (ota_is_newer(o->version, st->version))
        ESP_LOGI(TAG, "second chip: %s [%s], release; the %s has %s", st->version, a,
                 o->from_card ? "SD card" : "server", o->version);
    else
        ESP_LOGI(TAG, "second chip: %s [%s], release; %s is the latest", st->version, a, o->version);
}

/* In the boot window, once, before the radio's client starts: the one moment
 * with internal RAM to spare for a download -- about 11 s once a release --
 * or for the card's copy. Nothing new costs nothing. The image waits in
 * PSRAM, handed over by second_chip() and sent at a quiet moment. `wifi`:
 * the radio's link is WiFi, whose check just now did or did not reach the
 * update server; on the cable, the card's copy only. */
RADIO_ONLY_FN static void second_chip_boot(bool wifi)
{
    EXT_RAM_BSS_ATTR static bt_link_status_t st;
    EXT_RAM_BSS_ATTR static bt_link_upd_t    u;
    EXT_RAM_BSS_ATTR static ota_comp_offer_t o;
    /* Its word on what it runs: there by now, a second or two after
     * power-on -- unless it has none to give, or no chip answers, which
     * 5 s after this one started is none at all: no wait every boot for a
     * knob without the companion firmware. */
    for (int i = 0; i < 30; i++) {
        bt_link_status(&st);
        bt_link_update_status(&u);
        if (u.info || (st.companion && !(st.flags & BTL_HELLO_UPDATE)) ||
            (!st.companion && esp_timer_get_time() > 5 * 1000000LL))
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!comp_auto(&st, &u)) return;
    ota_companion_offer(&o);
    /* The boot check reached the server, and its look for the chip's
     * firmware rides on after it, on its connection: a moment more. */
    if (wifi && o.looked && o.route && net_prov_ota_hours() != 0 && ota_busy()) {
        for (int i = 0; i < 30 && ota_busy(); i++) vTaskDelay(pdMS_TO_TICKS(200));
        ota_companion_offer(&o);
    }
    /* Only with the automatic check on: off, the knob looks for nothing on
     * the network for the chip either -- the card's copy still goes. */
    const bool online = wifi && o.route && net_prov_ota_hours() != 0;
    if (!online || !o.known) {
        ota_companion_card_look();
        ota_companion_offer(&o);
    }
    comp_say(&st, &u, &o);
    if (!comp_wanted(&o, &st, &u) || ota_companion_fetch(online, false) != ESP_OK) return;
    s_comp_fetched_at = esp_timer_get_time();
    int t = 0;
    for (; t < 30000 && ota_companion_busy(); t += 200) vTaskDelay(pdMS_TO_TICKS(200));
    if (ota_companion_busy()) {
        /* Never beside the session: a fetch at a quiet moment does better.
         * Waited for, too: the read under way may take 8 s more, with no
         * RAM floors, and the client about to start wants that RAM. */
        ESP_LOGW(TAG, "second chip: its firmware not fetched in %d s: stopped, for the radio's client", t / 1000);
        ota_companion_stop("its time was up");
        for (int i = 0; i < 50 && ota_companion_busy(); i++) vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* Every pass of the supervisor: whether this is a quiet moment to send the
 * second chip its firmware -- this image confirmed and up a minute, the
 * radio idle with no question on the dial, no over or call, no install or
 * upload of this chip's own; the headset's part bt_link knows best, and
 * keeps. Said every pass: a pass that does not come -- an install, the WiFi
 * setup -- stops a transfer by its silence. Then what the knob remembers of
 * it, an image fetched to hand over, and -- the boot window having missed
 * it, or the offer new since -- a fetch now, after two quiet minutes, at
 * most once in half an hour and three times a boot, with internal RAM to
 * spare, and stopped by an over or a call. `idle`: the radio idle, no
 * question on the dial; one with no client yet is. `session`: the radio's
 * client runs. */
static void second_chip(bool idle, bool session)
{
    EXT_RAM_BSS_ATTR static bt_link_status_t st;
    EXT_RAM_BSS_ATTR static bt_link_upd_t    u;
    EXT_RAM_BSS_ATTR static ota_comp_offer_t o;
    EXT_RAM_BSS_ATTR static int fetches;
    EXT_RAM_BSS_ATTR static bool    fetching;       /* a fetch of ours runs */
    EXT_RAM_BSS_ATTR static int64_t spaced;         /* s_comp_fetched_at before it */
    EXT_RAM_BSS_ATTR static int64_t quiet_since;    /* idle, with no over or call, since; 0: not now */
    const int64_t now  = esp_timer_get_time();
    const bool    busy = audio_busy();
    bt_link_update_allow(s_boot_ok && now >= 60 * 1000000LL && idle && !busy && !ota_writing());
    if (!idle || busy)     quiet_since = 0;
    else if (!quiet_since) quiet_since = now;

    /* A fetch of ours, beside the radio's session, steps aside for an over
     * or a call as the transfer it feeds does: stopped at its next read, and
     * neither counted nor spaced -- it goes again at the next quiet two
     * minutes, which it waits for as the transfer does: never a download
     * begun over and over between the overs of a contact. */
    if (fetching && !ota_companion_busy()) fetching = false;
    if (fetching && busy) {
        fetching = false;
#if VFO_RADIO_PHONE
        const bool stopped = ota_companion_stop("a call came first -- again after two quiet minutes");
#else
        const bool stopped = ota_companion_stop("an over came first -- again after two quiet minutes");
#endif
        if (stopped) {
            fetches--;
            s_comp_fetched_at = spaced;
        }
    }

    bt_link_status(&st);
    bt_link_update_status(&u);
    comp_record_note(&u);
    comp_record_save(busy);
    const bool keep_up = comp_auto(&st, &u);
    ota_companion_offer(&o);
    if (keep_up) comp_say(&st, &u, &o);

    size_t   len = 0;
    uint8_t  sha[32];
    uint8_t *img = ota_companion_take(&len, sha);
    if (img) {
        /* Fetched for the chip as it was then: still for it now? */
        char    ver[33] = "?";
        uint8_t app[8];
        const char *no = !ota_companion_image_ok(img, len, ver, sizeof ver, app) ? "not a second-chip firmware"
                       : !keep_up                                ? "the second chip is not to be updated now"
                       : !ota_is_newer(ver, st.version) || !memcmp(app, u.chip.app_sha, 8)
                                                                 ? "the second chip runs it, or a newer one"
                       : bt_link_update_blocked(app, NULL, 0)    ? "it is not to go again"
                       : NULL;
        if (!no && bt_link_update_start(img, len, sha, false) != ESP_OK)
            no = "an update of the second chip is held or going";
        if (no) {
            ESP_LOGW(TAG, "second chip: %s not handed over: %s", ver, no);
            free(img);
        }
        return;
    }
    /* Two quiet minutes -- which keeps it out of the first two as well,
     * while the radio's client makes its connection. */
    if (!keep_up || !quiet_since || now - quiet_since < 120 * 1000000LL || fetches >= 3 ||
        (s_comp_fetched_at && now - s_comp_fetched_at < 30 * 60 * 1000000LL) || bt_link_update_holding() ||
        ota_companion_busy() || ota_writing() || !comp_wanted(&o, &st, &u) ||
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 16 * 1024 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 6 * 1024)
        return;
    /* The server only while the automatic check is on, and its last look
     * reached it; the card's copy always. */
    if (ota_companion_fetch(o.route && net_prov_ota_hours() != 0, session) == ESP_OK) {
        fetches++;
        fetching = true;
        spaced = s_comp_fetched_at;
        s_comp_fetched_at = now;
        ESP_LOGI(TAG, "second chip: fetching %s for it%s", o.version, session ? ", beside the radio's session" : "");
    }
}

/* ota.c's, just before this chip's flash is written -- an install, a switch,
 * an upload: the second chip's update steps aside. This chip's comes first. */
RADIO_ONLY_FN static void second_chip_step_aside(void)
{
    bt_link_update_stop(BTL_UPD_WHY_KNOB);
}

RADIO_ONLY_FN static void net_task(void *arg)
{
    (void)arg;
    const vfo_cfg_t *cfg = net_prov_cfg();
    char ip[32] = { 0 };
    bool started = false;

    for (;;) {
        /* The firmware picker asked for, and the setup firmware on the SD
         * card: back to it without a network. Without, it waits for WiFi. */
        static bool picker_card;
        if (s_picker_accepted && !picker_card) {
            picker_card = true;
            if (ota_card_has("setup", NULL, 0)) {
                s_picker_accepted = false;
                install_switch("setup");          /* returns only if it failed */
            }
        }
        if (wifi_setup()) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (!started) {
            /* No restart here, however long this takes. Nothing has been lost
             * yet, and pick_transport() already re-runs the whole choice on
             * every pass, so a reboot only arrives back at this same question.
             * It used to reboot after three minutes anyway: with AetherSDR not
             * running the knob restarted every 3 min 12 s, all day, and never
             * stayed up long enough to dim. */
            bool via_usb = false;
            const char *host = pick_transport(cfg, ip, sizeof ip, &via_usb);
            if (host) s_on_usb = via_usb;
            /* The firmware picker, asked for and accepted before the restart
             * this boot came from: from WiFi, which has a way out -- the
             * cable does not. */
            if (s_picker_accepted && (net_prov_is_connected() || via_usb)) {
                s_picker_accepted = false;
                if (via_usb) {
                    ui_updating_show();
                    ui_updating_result(false, "Needs WiFi");
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    ui_updating_hide();
                } else {
                    install_switch("setup");
                }
            }
            /* On WiFi the release server is in reach: see if there is
             * anything newer, once, before TCI takes the RAM an install
             * would need. */
            static bool update_checked;
            if (host && !via_usb && !update_checked) {
                update_checked = true;
                boot_update_check();
            }
            /* The second chip's firmware, the same once a boot -- on the
             * cable too, from the SD card -- before the client starts. */
            static bool chip_checked;
            if (host && !chip_checked) {
                chip_checked = true;
                second_chip_boot(!via_usb);
            }
            if (host && via_usb) {
                /* Hand the radio's internal RAM back before asking for the
                 * client's transmit stack. Both transports up leaves too
                 * little for it, and the failure is silent in the worst way:
                 * the link connects and receives, and the knob will not tune. */
                esp_err_t werr = net_prov_wifi_stop();
                ESP_LOGI(TAG, "  wifi stopped for USB transport (%s); "
                              "free internal %u -> largest %u",
                         esp_err_to_name(werr),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(
                             MALLOC_CAP_INTERNAL));
            }
            if (host) {
                ESP_LOGI(TAG, "--- %s client --- %s:%u "
                              "(free internal %u, largest DMA %u)",
                         radio_link_name(), host, (unsigned)cfg->radio_port,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
                if (radio_start(host, cfg->radio_port, cfg->radio_user,
                                cfg->radio_pass) == ESP_OK) {
                    started = true;
                } else {
                    /* Usually means internal RAM was too tight to spawn the
                     * WebSocket task. Retrying is right: memory pressure is
                     * transient, and giving up leaves the knob permanently
                     * deaf with no indication why. */
                    ESP_LOGE(TAG, "  client failed to start; retrying");
                }
            }
        }

#if CONFIG_VFO_USB_NET
        /* No computer on the S3's side of the cable, and power on it -- the
         * rail says so: the plug is the wrong way round, or it is a charger.
         * The knob cannot tell those two apart, only that nobody is there, so
         * it says what would fix the first. With WiFi configured that is a
         * few seconds' hint while WiFi takes over; without, there is nothing
         * else the knob can do, so the hint stays up. On its battery there
         * is no cable at all, and nothing to turn over: it says nothing, and
         * goes to WiFi -- or, knowing none, puts up its WiFi setup. While the
         * rail's first readings settle it waits; with no reading at all, the
         * ADC down, it says it as it always did. A computer that is there but
         * slow to set the adapter up never sees it: it sends frames, and
         * usb_net_task counts those, not the adapter. */
        {
            static int64_t flip_since;
            const int64_t now = esp_timer_get_time();
            board_power_t pw;
            board_power_get(&pw);
            const bool cable_power = pw.src == KNOB_PWR_USB || (pw.src == KNOB_PWR_UNKNOWN && pw.mv < 0);
            const bool nobody = !started && atomic_load(&s_cable) == CABLE_NONE && cable_power;
            if (!nobody)          flip_since = 0;
            else if (!flip_since) flip_since = now;
            atomic_store(&s_flip_hint,
                         nobody && (!cfg->ssid[0] ||
                                    now - flip_since < 8 * 1000 * 1000));
        }
#endif

        {   /* Keep the tap-to-show address card current. */
            char usb[20] = { 0 }, wifi[20] = { 0 }, info[128];
            esp_netif_ip_info_t a;
            esp_netif_t *n = esp_netif_get_handle_from_ifkey("ETH_DEF");
            if (n && esp_netif_get_ip_info(n, &a) == ESP_OK && a.ip.addr)
                snprintf(usb, sizeof usb, IPSTR, IP2STR(&a.ip));
            n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (n && esp_netif_get_ip_info(n, &a) == ESP_OK && a.ip.addr)
                snprintf(wifi, sizeof wifi, IPSTR, IP2STR(&a.ip));
            if (atomic_load(&s_flip_hint))
                snprintf(info, sizeof info, "No computer on this side of\n"
                                            "the cable. Turn the USB-C\n"
                                            "plug over%s",
                         cfg->ssid[0] ? ", or wait for WiFi." : ".");
            else {
                /* The firmware, the knob's own power under it -- nothing
                 * while its readings settle -- then its addresses. */
                char fw[40], pwr[16] = "";
                firmware_line(fw, sizeof fw);
                board_power_t pw;
                board_power_get(&pw);
                if (pw.src == KNOB_PWR_USB)
                    snprintf(pwr, sizeof pwr, "on USB power\n");
                else if (pw.src == KNOB_PWR_BATTERY && pw.pct >= 0)
                    snprintf(pwr, sizeof pwr, "battery %d %%\n", pw.pct);
                snprintf(info, sizeof info, "%s\n%sUSB   %s\nWiFi  %s\nsetup  http://%s", fw, pwr,
                         usb[0]  ? usb  : "-",
                         wifi[0] ? wifi : "-",
                         usb[0] ? usb : (wifi[0] ? wifi : "-"));
            }
            ui_set_netinfo(info);
        }

        {   /* Transmitting counts as use, however long the over runs; so
             * does a call, from its first ring to its end: a phone that
             * rings on a dark screen shows nobody who is calling, and one
             * that goes dark mid-call has its HANG UP under the tap meant
             * to wake it. (Static, in PSRAM, as st below: the status grows
             * with every radio, and this task's stack does not.) */
            EXT_RAM_BSS_ATTR static radio_status_t ds;
            radio_get_status(&ds);
            ui_dim_tick(ds.tx || ds.ptt_state != PTT_IDLE || ds.call == RADIO_CALL_IN
                        || ds.call == RADIO_CALL_OUT || ds.call == RADIO_CALL_UP);
        }

        /* For the second chip's firmware: the radio idle, and no question
         * on the dial -- one with no client started yet is. */
        bool quiet_radio = true;
        if (started) {
            EXT_RAM_BSS_ATTR static radio_status_t st;
            radio_get_status(&st);

            /* A periodic check falls due: start it only while the radio is
             * idle and internal RAM has room, or it would compete with the
             * very session it is running beside. Until then it stays due.
             * The telephone never transmits: idle there is no call ringing,
             * being made or up. Else the check's TLS would run beside the
             * call, and its question -- which takes the next tap on the
             * face, wherever it lands -- could come up over a call ringing
             * in, and take the tap meant for ANSWER. (Every other firmware's
             * call stays idle.) */
            const bool idle = !st.tx && st.ptt_state == PTT_IDLE &&
                              (st.call == RADIO_CALL_IDLE || st.call == RADIO_CALL_ENDED);
            if (ota_check_due() && idle &&
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= 16 * 1024 &&
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= 6 * 1024 &&
                ota_start_check(false) == ESP_OK)
                ota_clear_due();

            /* A check -- periodic, a slow one from boot, or the configuration
             * page's "Check now" -- found something newer: ask, but never
             * while transmitting, in a call or with an editor open. A yes
             * restarts the knob, which then installs at boot; see
             * boot_update_check(). */
            static bool asking;
            ota_status_t o;
            ota_get_status(&o);
            if (!asking && o.checks != s_ota_seen) {
                if (!(o.newer && o.phase == OTA_IDLE)) {
                    s_ota_seen = o.checks;          /* nothing to ask */
                } else if (idle && !ui_edit_active()) {
                    s_ota_seen = o.checks;
                    asking = ui_ask_update(o.available, o.running, true);
                }
            }
            if (asking) {
                const int a = ui_take_update_answer();
                if (a) asking = false;
                if (a > 0) {
                    /* The dial has already swapped to the update screen, so
                     * PTT is out of reach. Make sure ours is idle before the
                     * restart: a reset while keyed would leave the radio
                     * transmitting until AetherSDR noticed the socket gone. */
                    radio_status_t now;
                    radio_get_status(&now);
                    if (now.ptt_state != PTT_IDLE) {
                        radio_ptt_unkey();
                        for (int i = 0; i < 50 && now.ptt_state != PTT_IDLE; i++) {
                            vTaskDelay(pdMS_TO_TICKS(100));
                            radio_get_status(&now);
                        }
                    }
                    if (now.ptt_state != PTT_IDLE) {
                        ESP_LOGE(TAG, "update not started: PTT would not go idle");
                        ui_updating_hide();
                    } else {
                        ESP_LOGW(TAG, "update accepted -- restarting to install "
                                      "it before TCI starts");
                        boot_ok_now();
                        s_update_on_boot = UPDATE_ON_BOOT;
                        vTaskDelay(pdMS_TO_TICKS(300));
                        esp_restart();
                    }
                }
            }
            quiet_radio = idle && !asking;

            /* No restart when the link drops, either. The client reconnects on
             * its own, backing off to 8 s, and the radio is already safe:
             * AetherSDR unkeys a client that disconnects. A reboot after 60 s
             * down bought nothing but a knob that restarted every time
             * AetherSDR was closed. Unplugging the cable to move it is a power
             * cycle anyway, so the transport still gets chosen afresh then. */
            static const char *L[] = { "down", "connecting", "greeting",
                                       "READY", "degraded" };
            char memtag[40] = "";
            if (st.reflector) {
                /* A reflector has a talkgroup and a talker, not a frequency. */
                ESP_LOGI(TAG,
                    "[%s] %-10s TG %lu%s%s%s%s rx=%.0fdBFS ptt=%s | "
                    "conn=%u close=%u send=%u%s%s",
                    radio_link_name(), L[st.link], (unsigned long)st.tg,
                    st.tg_name[0] ? " " : "", st.tg_name,
                    st.talker[0] ? " talking: " : "", st.talker,
                    (double)st.rx_level_db, ptt_state_name((ptt_state_t)st.ptt_state),
                    (unsigned)st.connects, (unsigned)st.closes, (unsigned)st.sends,
                    st.last_close[0] ? " last_close=" : "", st.last_close);
            } else if (st.mem_state != RADIO_MEM_OFF && st.mem_all)
                snprintf(memtag, sizeof memtag, " MEM %02u%s%s", (unsigned)st.mem_ch,
                         st.mem_name[0] ? " " : "", st.mem_name);
            else if (st.mem_state != RADIO_MEM_OFF)
                snprintf(memtag, sizeof memtag, " MEM %02u/%02u%s%s",
                         (unsigned)st.mem_group, (unsigned)st.mem_ch,
                         st.mem_name[0] ? " " : "", st.mem_name);
            if (!st.reflector) ESP_LOGI(TAG,
                "[%s] %-10s f=%lld srv=%lld %s %ld..%ld s=%.0fdBm%s%s ptt=%s | "
                "conn=%u close=%u send=%u echo=%u recon=%u rej=%u unk=%u%s%s",
                radio_link_name(), L[st.link],
                (long long)st.f_display, (long long)st.f_server,
                st.mode, (long)st.filt_lo, (long)st.filt_hi,
                (double)st.smeter_dbm, st.slice_locked ? " LOCK" : "", memtag,
                ptt_state_name((ptt_state_t)st.ptt_state),
                (unsigned)st.connects, (unsigned)st.closes,
                (unsigned)st.sends, (unsigned)st.echoes,
                (unsigned)st.reconciles, (unsigned)st.rejects,
                (unsigned)st.unknown_cmds,
                st.last_close[0] ? " last_close=" : "", st.last_close);

            audio_in_stats_t m;
            audio_in_stats(&m);
            if (audio_in_active() || m.blocks)
                ESP_LOGI(TAG, "[MIC] %s blocks=%u starved=%u overruns=%u peak=%.2f",
                         audio_in_active() ? "LIVE" : "idle",
                         (unsigned)m.blocks, (unsigned)m.starved,
                         (unsigned)m.overruns, (double)m.peak);

            static uint32_t last_chronos;
            if (st.chronos != last_chronos) {
                last_chronos = st.chronos;
                /* The WebSocket task's lowest free stack so far: it answers
                 * every TX_CHRONO from inside its receive loop. */
                TaskHandle_t ws = xTaskGetHandle("websocket_task");
                /* Each core's idle share since the last line. The task
                 * watchdog once reset the knob mid-over because core 1 never
                 * went idle for five seconds; this shows how close that is. */
                static uint32_t idle_prev[2];
                static int64_t  idle_t;
                const int64_t   t_now = esp_timer_get_time();
                unsigned        idle_pct[2] = { 0, 0 };
                for (int c = 0; c < 2; c++) {
                    const uint32_t v = ulTaskGetIdleRunTimeCounterForCore(c);
                    if (idle_t && t_now > idle_t)
                        idle_pct[c] = (unsigned)((uint64_t)(v - idle_prev[c]) * 100u /
                                                 (uint64_t)(t_now - idle_t));
                    idle_prev[c] = v;
                }
                idle_t = t_now;
                ESP_LOGI(TAG, "[TXA] chrono=%u sent=%u failed=%u skipped=%u "
                              "max_send=%ums ws_stack_free=%u idle=%u/%u%%",
                         (unsigned)st.chronos, (unsigned)st.txa_sent,
                         (unsigned)st.txa_failed, (unsigned)st.txa_skipped,
                         (unsigned)(st.txa_max_us / 1000),
                         ws ? (unsigned)uxTaskGetStackHighWaterMark(ws) : 0u,
                         idle_pct[0], idle_pct[1]);
                log_cpu();
            } else {
                static unsigned cpu_ticks;          /* a receive baseline */
                if (++cpu_ticks % 15 == 0) log_cpu();
            }

            audio_stats_t a;
            audio_out_stats(&a);
            if (a.frames || a.dropped)
                ESP_LOGI(TAG, "[AUD] frames=%u dropped=%u underruns=%u | "
                              "%u Hz fmt=%u ch=%u vol=%u",
                         (unsigned)a.frames, (unsigned)a.dropped,
                         (unsigned)a.underruns, (unsigned)a.sample_rate,
                         (unsigned)a.format, (unsigned)a.channels,
                         (unsigned)ui_volume());
        }
        second_chip(quiet_radio, started);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

#if CONFIG_VFO_PANEL_SELFTEST
static void selftest_task(void *arg)
{
    (void)arg;
    for (int pass = 1; pass <= 3; pass++) {
        ESP_LOGI(TAG, "=== M4 display self-test, pass %d of 3 ===", pass);
        panel_selftest();
    }
    ESP_LOGI(TAG, "=== M4 self-test finished ===");
    vTaskDelete(NULL);
}
#endif

/* Bring one subsystem up, timed, and never fatally.
 *
 * ESP_ERROR_CHECK aborts, and an abort during init is a reboot loop -- which
 * on this board also stops the USB-JTAG enumerating, so the device simply
 * vanishes and cannot even be reflashed until it is physically unplugged.
 * A knob that boots and says "no display" is strictly better than one that
 * disappears. The timing is logged because a stall during init is otherwise
 * invisible: the watchdog fires and you cannot tell which step was in
 * progress. */
static bool bring_up(const char *what, esp_err_t (*fn)(void))
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = fn();
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (err == ESP_OK) {
        if (ms > 200) ESP_LOGW(TAG, "%-8s ok (%d ms -- slow)", what, ms);
        else          ESP_LOGI(TAG, "%-8s ok (%d ms)", what, ms);
        return true;
    }
    ESP_LOGE(TAG, "%-8s FAILED after %d ms: %s -- continuing without it",
             what, ms, esp_err_to_name(err));
    return false;
}

#if CONFIG_VFO_USB_NET
/* Deferred so that every boot has a flashing window -- see the comment at the
 * call site. */
static void usb_net_task(void *arg)
{
    (void)arg;
    /* Is there a computer on this side of the cable at all? The ROM's serial
     * port has been on the lines since reset, so a computer has long been
     * sending it frames by now; two seconds of silence means nobody is there.
     * Then TinyUSB is not started at all: its task, its netif and the holes
     * they leave in internal RAM are exactly what WiFi cannot spare -- with
     * them, the WebSocket client could not even create its task. */
    const int64_t t0 = esp_timer_get_time();
    if (!usb_net_probe_host(2000)) {
        ESP_LOGW(TAG, "no computer on the USB cable (plug the wrong way round, "
                      "or a charger) -- USB networking not started");
        atomic_store(&s_cable, CABLE_NONE);
        vTaskDelete(NULL);
    }
    atomic_store(&s_cable, CABLE_COMPUTER);
    /* A computer is there, so the flash window matters: keep all of it. */
    const int64_t left_ms = CONFIG_VFO_USB_NET_DELAY_MS -
                            (esp_timer_get_time() - t0) / 1000;
    if (left_ms > 0) vTaskDelay(pdMS_TO_TICKS(left_ms));
    if (bring_up("usb-net", usb_net_init))
        netlog_on_reboot(usb_net_prepare_reboot);
    vTaskDelete(NULL);
}
#endif

/* The busiest tasks since the last call, with the core each is pinned to
 * (@0, @1, or none). The idle figures say how close a core is to starving;
 * this says who is eating it. */
static void log_cpu(void)
{
    enum { MAX_T = 32, TOP = 6 };
    static TaskStatus_t *ts;
    static struct { TaskHandle_t h; uint32_t rt; } prev[MAX_T];
    static UBaseType_t nprev;
    static uint32_t    prev_total;
    if (!ts) ts = heap_caps_malloc(MAX_T * sizeof *ts, MALLOC_CAP_SPIRAM);
    if (!ts) return;

    uint32_t    total = 0;
    UBaseType_t n = uxTaskGetSystemState(ts, MAX_T, &total);
    uint32_t    d[MAX_T];
    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t was = 0;
        for (UBaseType_t j = 0; j < nprev; j++)
            if (prev[j].h == ts[i].xHandle) { was = prev[j].rt; break; }
        d[i] = ts[i].ulRunTimeCounter - was;
    }
    const uint32_t dt = total - prev_total;
    const bool     first = (prev_total == 0);
    for (UBaseType_t i = 0; i < n; i++) {
        prev[i].h  = ts[i].xHandle;
        prev[i].rt = ts[i].ulRunTimeCounter;
    }
    nprev      = n;
    prev_total = total;
    if (first || !dt) return;

    char   line[200] = "";
    size_t o = 0;
    for (int k = 0; k < TOP && o < sizeof line; k++) {
        int best = -1;
        for (UBaseType_t i = 0; i < n; i++)
            if (d[i] && (best < 0 || d[i] > d[best])) best = (int)i;
        if (best < 0) break;
        const BaseType_t core = xTaskGetCoreID(ts[best].xHandle);
        o += snprintf(line + o, sizeof line - o, " %s%s=%u%%",
                      ts[best].pcTaskName,
                      core == 0 ? "@0" : core == 1 ? "@1" : "",
                      (unsigned)((uint64_t)d[best] * 100u / dt));
        d[best] = 0;
    }
    ESP_LOGI(TAG, "[CPU]%s", line);

    /* And the tasks nearest the end of their stacks, in bytes never used:
     * an overflow is otherwise only ever found by the crash. In `line`
     * again, with a bitmask: this runs on the supervisor's own small stack. */
    uint32_t shown = 0;
    line[0] = 0;
    o = 0;
    for (int k = 0; k < 4 && o < sizeof line; k++) {
        int low = -1;
        for (UBaseType_t i = 0; i < n; i++)
            if (!(shown & (1u << i)) &&
                (low < 0 || ts[i].usStackHighWaterMark < ts[low].usStackHighWaterMark))
                low = (int)i;
        if (low < 0) break;
        shown |= 1u << low;
        o += snprintf(line + o, sizeof line - o, " %s=%u", ts[low].pcTaskName,
                      (unsigned)ts[low].usStackHighWaterMark);
    }
    ESP_LOGI(TAG, "[STK]%s", line);
}

/* The USB build has no console, so a panic's backtrace is printed to nobody.
 * The core dump in flash survives the restart, though: say what it holds, so
 * the log port shows which task died and where. The addresses decode with
 * addr2line against the ELF of the build that crashed. */
static void report_stored_crash(void)
{
    if (esp_core_dump_image_check() != ESP_OK) return;     /* none stored */
    /* The dump stays in flash until the next crash replaces it. Only a boot
     * that follows a crash is reporting a new one; anything else would repeat
     * an old crash on every start, as though it had just happened. */
    const esp_reset_reason_t why = esp_reset_reason();
    if (why != ESP_RST_PANIC && why != ESP_RST_TASK_WDT &&
        why != ESP_RST_INT_WDT && why != ESP_RST_WDT) {
        ESP_LOGI(TAG, "an older crash dump is stored: GET /api/coredump");
        return;
    }
    esp_core_dump_summary_t *s = heap_caps_calloc(1, sizeof *s, MALLOC_CAP_SPIRAM);
    if (!s) return;
    if (esp_core_dump_get_summary(s) == ESP_OK) {
        ESP_LOGE(TAG, "stored crash: task \"%s\" pc=0x%08" PRIx32 " cause=%" PRIu32
                      " vaddr=0x%08" PRIx32 " elf=%.16s",
                 s->exc_task, s->exc_pc, s->ex_info.exc_cause,
                 s->ex_info.exc_vaddr, (const char *)s->app_elf_sha256);
        char   bt[16 * 11 + 1] = "";
        size_t o = 0;
        for (uint32_t i = 0; i < s->exc_bt_info.depth && i < 16; i++)
            o += snprintf(bt + o, sizeof bt - o, " 0x%08" PRIx32, s->exc_bt_info.bt[i]);
        ESP_LOGE(TAG, "stored crash backtrace:%s%s", bt,
                 s->exc_bt_info.corrupted ? " (corrupted)" : "");
    }
    free(s);
}

static void boot_ok_cb(void *arg)
{
    (void)arg;
    net_prov_boot_ok();
    /* Same moment, same meaning: this boot looks healthy. If the running image
     * arrived over the air it is on trial until now, and the bootloader will
     * put the previous one back if we never get here. */
    ota_mark_valid();
    s_boot_ok = true;
}

void app_main(void)
{
    /* Before anything else: the USB networking build has no serial console at
     * all, so without this there is no way to see an init failure. */
    netlog_init();
#if CONFIG_VFO_USB_NET
    /* Before anything else, and unconditionally: a previous run may have left
     * the USB PHY on the OTG controller, and that choice survives a reset. */
    usb_net_release_phy();
#endif

    /* A yes to "update?" restarts the knob with this flag set; it counts for
     * this boot only, and only after that deliberate restart -- power-on
     * leaves RTC memory as garbage that merely might match. */
    s_update_accepted = s_update_on_boot == UPDATE_ON_BOOT &&
                        esp_reset_reason() == ESP_RST_SW;
    s_update_on_boot = 0;
    s_picker_accepted = s_picker_on_boot == PICKER_ON_BOOT &&
                        esp_reset_reason() == ESP_RST_SW;
    s_picker_on_boot = 0;

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "VFO-Knob | ESP32-S3 rev%d.%d, %d core(s), reset=%d",
             chip.revision / 100, chip.revision % 100, chip.cores,
             (int)esp_reset_reason());
    report_stored_crash();

#if CONFIG_VFO_GPIO_SCAN
    bring_up("nvs", net_prov_init);
    bring_up("board", board_init);
    xTaskCreatePinnedToCore(gpio_scan_task, "enctest", 4096, NULL, 5, NULL, 1);
    return;
#else
    bring_up("nvs", net_prov_init);
#if VFO_RADIO_SETUP
    /* tools/install-setup.sh's one-time mark in the settings it wrote. */
    s_flashed_now = net_prov_take_once("sdwipe");
#endif
    /* net_prov_init() brings up esp_netif, so the log server can bind now. */
    netlog_start();
    /* Configuration page. Started before the transport is chosen so it is
     * reachable even when nothing else comes up -- which is exactly when
     * someone needs to correct an SSID or a host address. */
    bring_up("webcfg", webcfg_start);
    ota_init();
    ota_set_interval(net_prov_ota_hours());

    /* Three failed boots in a row: come up with the bare minimum so the device
     * stays usable and flashable while the cause is found. */
    const bool safe = net_prov_boot_count() >= 3;
    if (safe)
        ESP_LOGE(TAG, "SAFE MODE after %u boots -- audio and WiFi disabled",
                 net_prov_boot_count());

    bool have_board = bring_up("board", board_init);
    if (have_board) { report_memory(); probe_i2c(); haptic_bringup(); }
    bring_up("power", board_power_init);

    bool have_panel = bring_up("panel", panel_init);
    bool have_touch = bring_up("touch", hal_touch_init);

    ui_set_levels(net_prov_volume(), net_prov_mic_gain());
    bool have_ui = have_panel && bring_up("ui", ui_init);
    /* Building the face is the deepest thing the main task does. */
    ESP_LOGI(TAG, "main stack after ui: %u of %u bytes never used",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)CONFIG_ESP_MAIN_TASK_STACK_SIZE);
    /* After ui_init, not before: this reaches into LVGL, and the version that
     * called it up beside ota_init() crashed on boot -- caught by the OTA
     * rollback, which put the previous image back. */
    if (have_ui) ui_dim_set_minutes(net_prov_dim_min(), net_prov_blank_min());

    bring_up("knob", hal_encoder_init);

    if (!safe) {
        bring_up("audio-out", audio_out_init);
#if !VFO_RX_ONLY
        /* A receiver has no use for the microphone, nor its DMA's RAM. */
        bring_up("mic", audio_in_init);
#endif
#if VFO_HAS_SDR
        bring_up("web SDR", sdr_rx_init);
#endif
#if !VFO_RADIO_SETUP
        /* The second chip, for a Bluetooth headset. An update of its own
         * firmware steps aside for an over, or a call, and for this chip's
         * own update; what the knob remembers of the last is read back. */
        bring_up("bt link", bt_link_init);
        bt_link_update_busy_cb(audio_busy);
        ota_set_flash_hook(second_chip_step_aside);
        comp_record_load();
#endif
        /* The saved levels, even with no display to carry them. */
        audio_out_set_volume(net_prov_volume());
        audio_in_set_gain(net_prov_mic_gain());
    }

    /* Declare the boot healthy once we have been up a while. Anything that
     * panics before this leaves the counter raised and edges us toward safe
     * mode on the next attempt. */
    const esp_timer_create_args_t ok = { .callback = boot_ok_cb, .name = "bootok" };
    if (esp_timer_create(&ok, &s_boot_ok_t) == ESP_OK)
        esp_timer_start_once(s_boot_ok_t, 20 * 1000 * 1000);

    bool usb_net_on = false;
    /* USB networking comes up even in SAFE MODE, deliberately.
     *
     * Safe mode used to skip it along with WiFi and audio, which made the
     * device unreachable by the only channel it has: no USB network means no
     * configuration page and no log, and the knob can then only be recovered
     * over the ROM serial port. That happened for real -- a server outage made
     * the link-down guard restart the knob repeatedly, the boot counter passed
     * three, and safe mode then removed the very thing needed to look at it.
     *
     * Safe mode is for shedding what might have caused a crash loop, and this
     * is not that: it is local, it needs no radio and no credentials, and the
     * touch-held escape below still disables it if TinyUSB itself is the
     * problem. Observability is the last thing to drop, not the first. */
#if CONFIG_VFO_USB_NET
        /* KEEPING THE DEVICE FLASHABLE. Read before shortening the delay.
         *
         * Installing TinyUSB takes the USB PHY away from USB-Serial-JTAG, so
         * the serial port the ROM put up at reset disappears -- and esptool
         * has nothing left to open. The only strap that would force ROM
         * download mode is GPIO0, which on this board is also the audio mux
         * and sits behind a single button in the CNC case. Flipping the
         * Type-C plug does NOT rescue it either: that orientation reaches the
         * board's other chip (an ESP32 behind a CH340), not the S3.
         *
         * So the firmware has to leave its own way back in. The ROM enumerates
         * USB-Serial-JTAG on every reset regardless of what we do, and it
         * stays up until we take the PHY. Waiting a few seconds first means
         * esptool can always connect and reset the chip into download mode --
         * no button, no disassembly, on every boot. Enumeration on the host
         * takes ~300-500 ms, so the window has to be seconds, not milliseconds:
         * an immediate install lost the race every single time.
         *
         * Holding a finger on the screen through boot skips USB networking
         * altogether and keeps the console, which is the second way out. */
        touch_sample_t t0s;
        hal_touch_get(&t0s);
        ESP_LOGI(TAG, "usb-net gate: touch.pressed=%d", (int)t0s.pressed);
        if (t0s.pressed) {
            ESP_LOGW(TAG, "touch held at boot -- skipping USB networking, "
                          "serial console stays available");
        } else {
            /* Claim the pads now (so the console does not grab them) but do
             * the actual install late. */
            usb_net_on = true;
            ESP_LOGW(TAG, "usb-net starts in %d ms -- serial flash window open",
                     CONFIG_VFO_USB_NET_DELAY_MS);
            xTaskCreatePinnedToCore(usb_net_task, "usbnet", 4096, NULL, 5, NULL, 0);
        }
#endif
    /* The held-finger check above was the touch sampler's only use. Stop it,
     * so LVGL is the controller's only reader. */
    if (have_touch) hal_touch_stop();

    if (!safe) {
        /* WiFi is started by net_task, and only if the cable turns out not to
         * be an option. Starting it here unconditionally was the root of a
         * deadlock: the driver takes the internal RAM the WebSocket client
         * needs, so a knob that fell back to WiFi could not open a socket at
         * all -- and could not be updated out of that state either, because
         * the OTA path needs the same memory. The bug blocked its own fix. */
        /* The supervisor's stack. 4 kB never was enough: the svxconnect
         * firmware's ran with 204 bytes to spare and overflowed once at boot;
         * the multiflex firmware's overflowed, and at 5 kB still came within
         * 84 bytes of the end; the AetherSDR firmware's ran within 56 bytes
         * of it ([STK] in the log) and overflowed joining WiFi once its TLS
         * went to software AES (sdkconfig.usbnet). Joining WiFi runs some of
         * the WPA code on this stack. Every radio's firmware gets 6 kB. */
#if VFO_RADIO_SETUP
        /* TLS, for the firmwares' versions, on its own stack. */
        xTaskCreatePinnedToCore(setup_task, "setup", 10240, NULL, 3, NULL, 0);
#else
        xTaskCreatePinnedToCore(net_task, "net_sup", 6144, NULL, 3, NULL, 0);
#endif
    }

    /* The console and USB networking cannot coexist: both want the USB pads.
     * usb_serial_jtag_driver_install() re-enables the USJ peripheral, which
     * takes the internal PHY back from the OTG controller -- silently, and
     * milliseconds after usb_net_init() reported success. Everything on the
     * device looks healthy; the host simply never sees the network adapter.
     * When the touch-held escape skipped USB networking, the console is still
     * the right thing to have, so this is conditional rather than compiled out. */
    if (usb_net_on)
        ESP_LOGI(TAG, "console off: USB pads belong to USB networking");
#if VFO_RADIO_SETUP
    else if (s_flashed_now)
        xTaskCreatePinnedToCore(card_console_task, "console", CARD_STACK, NULL, 2, NULL, 0);
#endif
    else
        xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 2, NULL, 0);
    if (have_ui)
        xTaskCreatePinnedToCore(ui_task, "ui", 5120, NULL, 4, NULL, 1);
    /* Core 1 is the "feel" core: knob, haptics, touch and LVGL. Core 0 is
     * reserved for WiFi and lwIP, whose burst timing we cannot control. */
    xTaskCreatePinnedToCore(encoder_task, "enc_input", 4096, NULL, 15, NULL, 1);
#if VFO_RADIO_PHONE
    for (int c = 0; c < 2; c++)
        xTaskCreatePinnedToCoreWithCaps(probe_task, c ? "probe1" : "probe0", 4096,
                                        (void *)(intptr_t)c, 22, NULL, c, MALLOC_CAP_SPIRAM);
#endif

    ESP_LOGI(TAG, "--- up: board=%d panel=%d touch=%d ui=%d %s---",
             have_board, have_panel, have_touch, have_ui,
             safe ? "SAFE MODE " : "");
#endif
}
