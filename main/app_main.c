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

#include "audio_in.h"
#include "audio_out.h"
#include "board.h"
#include "board_pins.h"
#include "drv2605.h"
#include "gpio_scan.h"
#include "hal_encoder.h"
#include "net_prov.h"
#include "tci_client.h"
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
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"
#include "ptt_fsm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "vfo";

static void log_cpu(void);

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
        ui_ask_knob_moved();         /* turning the knob answers "update?" no */
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

        tune.step_hz = atomic_load(&s_step_hz);
        uint8_t mult = accel_update(&accel, detents, now_ms);
        if (accel.v_detents > v_peak) v_peak = accel.v_detents;

        /* ONE frequency, not two. The client owns the optimistic value because
         * it also has to survive reconciliation against the radio; the haptics
         * read it back so rollover and decimation fire on the frequency the
         * operator is actually on. Keeping a second local copy let the two
         * drift 10 MHz apart during the first real-radio test. */
        int64_t before = tci_tune_by(0, 1, tune.step_hz);
        int64_t after  = tci_tune_by(detents, mult, tune.step_hz);
        tune.f_display = after;        /* keep the local step model in step */

        /* Tuning is silent by design; see the note above. `before` and
         * `after` remain wired up so the band-edge signal can hook in here
         * without restructuring anything. */
        (void)before; (void)after;
    }
}

/* The TCI client declares this weak so it stays free of a haptic dependency.
 * This is where the haptic channel is actually spent: PTT state, a rejected
 * tune, link loss. Never tuning -- the knob has real detents of its own. */
void haptic_hook(uint8_t effect, uint8_t prio)
{
    (void)prio;
    if (effect) drv2605_fire(&s_drv, effect);
}

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
        case 'u': ESP_LOGI(TAG, "console: unkey");  tci_ptt_unkey();  break;
        case 'p': ESP_LOGI(TAG, "console: forcing pong-stale abort");
                  tci_ptt_force_abort(PTT_AB_PONG_STALE); break;
        case 'd': ESP_LOGI(TAG, "console: forcing link-down abort");
                  tci_ptt_force_abort(PTT_AB_LINK_DOWN);  break;
        case 'r': ui_cycle_rotation();
                  ESP_LOGI(TAG, "rotation -> %u degrees", ui_rotation() * 90u);
                  break;
        case 's': {
            tci_status_t st; tci_get_status(&st);
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

static void ui_task(void *arg)
{
    (void)arg;
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(50));   /* 20 Hz is plenty */

        int32_t req = ui_take_step_request();
        if (req) {
            atomic_store(&s_step_hz, req);
            tci_set_step(req);
            ESP_LOGI(TAG, "step -> %ld Hz", (long)req);
            drv2605_fire(&s_drv, 26);      /* confirm the tap landed */
        }
        ui_commit_t c;
        if (ui_take_commit(&c)) {
            if (c.have_mode) {
                ESP_LOGI(TAG, "mode -> %s", c.mode);
                tci_set_mode(c.mode);
            }
            if (c.have_filter) {
                ESP_LOGI(TAG, "filter -> %ld..%ld",
                         (long)c.filt_lo, (long)c.filt_hi);
                tci_set_filter(c.filt_lo, c.filt_hi);
            }
            if (c.have_rit) {
                ESP_LOGI(TAG, "rit -> %+ld", (long)c.rit_hz);
                tci_set_rit(c.rit_hz);
            }
            if (c.have_freq) {
                ESP_LOGI(TAG, "band -> %lld", (long long)c.freq_hz);
                tci_goto_freq(c.freq_hz);
            }
            drv2605_fire(&s_drv, 7);        /* soft bump: value committed */
        }

        /* Volume and mic gain live in the UI -- the dial's editors and the
         * configuration page both set them there -- and nothing passed them
         * on: the audio stayed at its defaults, 40 and 100, whatever the dial
         * showed, and a level turned on the dial was never saved. Apply them
         * as they change, and save them once they have settled; the save is
         * debounced because NVS wear is real and the knob turns fast. */
        {
            static uint8_t applied_vol = 0xFF, applied_mic = 0xFF;
            static int64_t changed_at;
            const uint8_t vol = ui_volume(), mic = ui_mic_gain();
            if (vol != applied_vol || mic != applied_mic) {
                audio_out_set_volume(vol);
                audio_in_set_gain(mic);
                applied_vol = vol;
                applied_mic = mic;
                changed_at  = esp_timer_get_time();
            }
            if (changed_at && esp_timer_get_time() - changed_at > 2000000) {
                net_prov_save_audio(vol, mic);  /* no-op when unchanged */
                changed_at = 0;
            }
        }

        if (ui_take_ptt_tap()) {
            ESP_LOGI(TAG, "PTT tapped");
            tci_ptt_toggle();
        }

        tci_status_t st;
        tci_get_status(&st);

        /* SWR alarm: the motor runs for as long as SWR stays above 2.5 on the
         * air. A bad match is not something to find out later from the glass.
         * Readings count only with real forward power (in a speech pause the
         * figure is noise), and the alarm holds a second past the last bad one
         * so the pauses between words do not chop it up. Real-time mode, so it
         * is one continuous buzz rather than a string of effects; a click
         * fired meanwhile is simply not heard. */
        {
            static bool    s_swr_buzz;
            static int64_t s_swr_bad_us;
            const int64_t  now_us = esp_timer_get_time();
            if (st.tx && st.tx_fwd_w >= 1.0f && st.tx_swr > 2.5f)
                s_swr_bad_us = now_us;
            const bool want = st.tx && s_swr_bad_us &&
                              now_us - s_swr_bad_us < 1000000;
            if (want != s_swr_buzz) {
                s_swr_buzz = want;
                if (want) {
                    drv2605_rtp_begin(&s_drv);
                    drv2605_rtp_write(&s_drv, 0x70);   /* ~88% of full drive */
                    ESP_LOGW(TAG, "SWR alarm: %.1f", (double)st.tx_swr);
                } else {
                    drv2605_rtp_end(&s_drv);
                    ESP_LOGI(TAG, "SWR alarm over");
                }
            }
        }

        /* AetherSDR owns the band plan and every other transmit precondition.
         * The protocol gives no reason for a refusal -- only trx:false -- so
         * the banner and the refusal haptic are all the operator gets. Hold a
         * refusal on screen for 3 s; it is otherwise a single frame. */
        static uint32_t s_seen_refusals;
        const bool link_ok = (st.link == TCI_LINK_READY ||
                              st.link == TCI_LINK_DEGRADED);
        static int64_t  s_warn_until;
        const char     *warn = NULL;
        int64_t nowms = esp_timer_get_time() / 1000;

        if (st.ptt_refusals != s_seen_refusals) {
            s_seen_refusals = st.ptt_refusals;
            s_warn_until    = nowms + 3000;
        }
        if (nowms < s_warn_until)                    warn = "TX REFUSED";
        else if (atomic_load(&s_flip_hint))          warn = "FLIP USB-C";
        else if (!(st.link == TCI_LINK_READY ||
                   st.link == TCI_LINK_DEGRADED))    warn = "NO LINK";
        else if (st.slice_locked)                    warn = "VFO LOCKED";
        else if (!(st.permit & PERMIT_TX_ENABLE))    warn = "TX DISABLED";

        ui_state_t u = {
            .freq_hz       = st.f_display,
            .step_hz       = atomic_load(&s_step_hz),
            .mode          = st.mode,
            .filt_lo       = st.filt_lo,
            .filt_hi       = st.filt_hi,
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
            .link_ok       = link_ok,
            .slice_locked  = st.slice_locked,
            .may_key       = (st.permit == PERMIT_ALL),
            .warn          = warn,
        };
        ui_update(&u);
    }
}

/* Does anything answer on this address and port? A plain TCP connect, used to
 * choose a transport.
 *
 * "Is the USB netif up?" is the wrong question: it comes up as soon as the
 * cable has power, including a charger with no computer behind it, and
 * esp_netif's DHCP *server* raises no event when it hands out a lease. The
 * only honest test is whether AetherSDR actually answers over the cable. */
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

/* Picks the transport for the TCI link, cable first.
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
        if (host_answers(usb_net_host(), cfg->tci_port, 500)) {
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
    /* Only now is WiFi worth its memory. */
    static bool wifi_started;
    if (!wifi_started) {
        wifi_started = true;
        esp_err_t werr = net_prov_wifi_start();
        if (werr != ESP_OK)
            ESP_LOGE(TAG, "wifi     FAILED: %s -- continuing offline",
                     esp_err_to_name(werr));
        return NULL;                       /* give it a moment to associate */
    }
    if (!net_prov_is_connected()) return NULL;
    if (net_prov_resolve(ip, iplen) != ESP_OK) {
        ESP_LOGE(TAG, "  cannot resolve %s", cfg->tci_host);
        return NULL;
    }
    ESP_LOGI(TAG, "--- transport: WiFi (%s -> %s) ---", cfg->tci_host, ip);
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
    if (!ui_ask_update(o->available, o->running)) return false;   /* no dial */
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
     * refuses until boot_ok_cb confirms it, 20 s into the boot -- so a yes
     * given that early waits for it rather than failing. */
    esp_ota_img_states_t trial;
    for (int i = 0; i < 120 &&
         esp_ota_get_state_partition(esp_ota_get_running_partition(),
                                     &trial) == ESP_OK &&
         trial == ESP_OTA_IMG_PENDING_VERIFY; i++)
        vTaskDelay(pdMS_TO_TICKS(250));
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

static void net_task(void *arg)
{
    (void)arg;
    const vfo_cfg_t *cfg = net_prov_cfg();
    char ip[32] = { 0 };
    bool started = false;

    for (;;) {
        if (!started) {
            /* No restart here, however long this takes. Nothing has been lost
             * yet, and pick_transport() already re-runs the whole choice on
             * every pass, so a reboot only arrives back at this same question.
             * It used to reboot after three minutes anyway: with AetherSDR not
             * running the knob restarted every 3 min 12 s, all day, and never
             * stayed up long enough to dim. */
            bool via_usb = false;
            const char *host = pick_transport(cfg, ip, sizeof ip, &via_usb);
            /* On WiFi the release server is in reach: see if there is
             * anything newer, once, before TCI takes the RAM an install
             * would need. */
            static bool update_checked;
            if (host && !via_usb && !update_checked) {
                update_checked = true;
                boot_update_check();
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
                ESP_LOGI(TAG, "--- M13 TCI client --- ws://%s:%u "
                              "(free internal %u, largest DMA %u)",
                         host, (unsigned)cfg->tci_port,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
                if (tci_client_start(host, cfg->tci_port) == ESP_OK) {
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
        /* No computer on the S3's side of the cable: the plug is the wrong way
         * round, or it is a charger -- the knob cannot tell which, only that
         * nobody is there, so it says what would fix it. With WiFi configured
         * that is a few seconds' hint while WiFi takes over; without, there is
         * nothing else the knob can do, so the hint stays up. A computer that
         * is there but slow to set the adapter up never sees it: it sends
         * frames, and usb_net_task counts those, not the adapter. */
        {
            static int64_t flip_since;
            const int64_t now = esp_timer_get_time();
            const bool nobody = !started && atomic_load(&s_cable) == CABLE_NONE;
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
            else
                snprintf(info, sizeof info, "USB   %s\nWiFi  %s\nsetup  http://%s",
                         usb[0]  ? usb  : "-",
                         wifi[0] ? wifi : "-",
                         usb[0] ? usb : (wifi[0] ? wifi : "-"));
            ui_set_netinfo(info);
        }

        {   /* Transmitting counts as use, however long the over runs. */
            tci_status_t ds;
            tci_get_status(&ds);
            ui_dim_tick(ds.tx || ds.ptt_state != PTT_IDLE);
        }

        if (started) {
            tci_status_t st;
            tci_get_status(&st);

            /* A periodic check falls due: start it only while the radio is
             * idle and internal RAM has room, or it would compete with the
             * very session it is running beside. Until then it stays due. */
            const bool idle = !st.tx && st.ptt_state == PTT_IDLE;
            if (ota_check_due() && idle &&
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= 16 * 1024 &&
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= 6 * 1024 &&
                ota_start_check(false) == ESP_OK)
                ota_clear_due();

            /* A check -- periodic, a slow one from boot, or the configuration
             * page's "Check now" -- found something newer: ask, but never
             * while transmitting or with an editor open. A yes restarts the
             * knob, which then installs at boot; see boot_update_check(). */
            static bool asking;
            ota_status_t o;
            ota_get_status(&o);
            if (!asking && o.checks != s_ota_seen) {
                if (!(o.newer && o.phase == OTA_IDLE)) {
                    s_ota_seen = o.checks;          /* nothing to ask */
                } else if (idle && !ui_edit_active()) {
                    s_ota_seen = o.checks;
                    asking = ui_ask_update(o.available, o.running);
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
                    tci_status_t now;
                    tci_get_status(&now);
                    if (now.ptt_state != PTT_IDLE) {
                        tci_ptt_unkey();
                        for (int i = 0; i < 50 && now.ptt_state != PTT_IDLE; i++) {
                            vTaskDelay(pdMS_TO_TICKS(100));
                            tci_get_status(&now);
                        }
                    }
                    if (now.ptt_state != PTT_IDLE) {
                        ESP_LOGE(TAG, "update not started: PTT would not go idle");
                        ui_updating_hide();
                    } else {
                        ESP_LOGW(TAG, "update accepted -- restarting to install "
                                      "it before TCI starts");
                        s_update_on_boot = UPDATE_ON_BOOT;
                        vTaskDelay(pdMS_TO_TICKS(300));
                        esp_restart();
                    }
                }
            }

            /* No restart when the link drops, either. The client reconnects on
             * its own, backing off to 8 s, and the radio is already safe:
             * AetherSDR unkeys a client that disconnects. A reboot after 60 s
             * down bought nothing but a knob that restarted every time
             * AetherSDR was closed. Unplugging the cable to move it is a power
             * cycle anyway, so the transport still gets chosen afresh then. */
            static const char *L[] = { "down", "connecting", "greeting",
                                       "READY", "degraded" };
            ESP_LOGI(TAG,
                "[TCI] %-10s f=%lld srv=%lld %s %ld..%ld s=%.0fdBm%s ptt=%s | "
                "conn=%u close=%u send=%u echo=%u recon=%u rej=%u unk=%u%s%s",
                L[st.link], (long long)st.f_display, (long long)st.f_server,
                st.mode, (long)st.filt_lo, (long)st.filt_hi,
                (double)st.smeter_dbm, st.slice_locked ? " LOCK" : "",
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
        bring_up("mic", audio_in_init);
        /* The saved levels, even with no display to carry them. */
        audio_out_set_volume(net_prov_volume());
        audio_in_set_gain(net_prov_mic_gain());
    }

    /* Declare the boot healthy once we have been up a while. Anything that
     * panics before this leaves the counter raised and edges us toward safe
     * mode on the next attempt. */
    const esp_timer_create_args_t ok = { .callback = boot_ok_cb, .name = "bootok" };
    esp_timer_handle_t okt;
    if (esp_timer_create(&ok, &okt) == ESP_OK)
        esp_timer_start_once(okt, 20 * 1000 * 1000);

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
        xTaskCreatePinnedToCore(net_task, "net_sup", 4096, NULL, 3, NULL, 0);
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
    else
        xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 2, NULL, 0);
    if (have_ui)
        xTaskCreatePinnedToCore(ui_task, "ui", 5120, NULL, 4, NULL, 1);
    /* Core 1 is the "feel" core: knob, haptics, touch and LVGL. Core 0 is
     * reserved for WiFi and lwIP, whose burst timing we cannot control. */
    xTaskCreatePinnedToCore(encoder_task, "enc_input", 4096, NULL, 15, NULL, 1);

    ESP_LOGI(TAG, "--- up: board=%d panel=%d touch=%d ui=%d %s---",
             have_board, have_panel, have_touch, have_ui,
             safe ? "SAFE MODE " : "");
#endif
}
