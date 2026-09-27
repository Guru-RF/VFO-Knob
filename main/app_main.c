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
#include "vfo_tune.h"

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"
#include "ptt_fsm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "vfo";

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
    ESP_LOGI(TAG, "console: t=toggle PTT  k=key  u=unkey  s=status");
    ESP_LOGI(TAG, "         p=abort:pong-stale  d=abort:link-down  o=TOT 30s");
    ESP_LOGI(TAG, "         r=cycle screen rotation");

    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(200)) != 1) continue;
        switch (ch) {
        case 't': ESP_LOGI(TAG, "console: toggle"); tci_ptt_toggle(); break;
        case 'k': ESP_LOGI(TAG, "console: key");    tci_ptt_key();    break;
        case 'u': ESP_LOGI(TAG, "console: unkey");  tci_ptt_unkey();  break;
        case 'p': ESP_LOGI(TAG, "console: forcing pong-stale abort");
                  tci_ptt_force_abort(PTT_AB_PONG_STALE); break;
        case 'd': ESP_LOGI(TAG, "console: forcing link-down abort");
                  tci_ptt_force_abort(PTT_AB_LINK_DOWN);  break;
        case 'o': ESP_LOGI(TAG, "console: TOT -> 30 s (FSM minimum)");
                  tci_set_tot_ms(30000); break;
        case 'r': ui_cycle_rotation();
                  ESP_LOGI(TAG, "rotation -> %u degrees", ui_rotation() * 90u);
                  break;
        case 's': {
            tci_status_t st; tci_get_status(&st);
            ESP_LOGI(TAG, "ptt=%s rung=%u reason=%s tot=%ums permit=0x%03X%s "
                          "pong=%ldms refusals=%u",
                     ptt_state_name((ptt_state_t)st.ptt_state), st.ptt_rung,
                     ptt_abort_name((ptt_abort_t)st.ptt_reason),
                     (unsigned)st.tot_remain_ms, (unsigned)st.permit,
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

        if (ui_take_ptt_tap()) {
            ESP_LOGI(TAG, "PTT tapped");
            tci_ptt_toggle();
        }

        tci_status_t st;
        tci_get_status(&st);

        /* AetherSDR owns the band plan and every other transmit precondition.
         * The protocol gives no reason for a refusal -- only trx:false -- so
         * the banner and the refusal haptic are all the operator gets. Hold a
         * refusal on screen for 3 s; it is otherwise a single frame. */
        static uint32_t s_seen_refusals;
        static int64_t  s_warn_until;
        const char     *warn = NULL;
        int64_t nowms = esp_timer_get_time() / 1000;

        if (st.ptt_refusals != s_seen_refusals) {
            s_seen_refusals = st.ptt_refusals;
            s_warn_until    = nowms + 3000;
        }
        if (nowms < s_warn_until)                    warn = "TX REFUSED";
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
            .tx            = st.tx,
            /* Only IDLE counts as "someone else". During our own RELEASING --
             * between sending trx:false and the confirmation arriving -- the
             * radio is still transmitting and the state is not PTT_ON, which
             * briefly and wrongly read as a remote transmission. */
            .tx_remote     = (st.tx && st.ptt_state == PTT_IDLE),
            .link_ok       = (st.link == TCI_LINK_READY ||
                              st.link == TCI_LINK_DEGRADED),
            .slice_locked  = st.slice_locked,
            .tot_remain_ms = st.tot_remain_ms,
            .may_key       = (st.permit == PERMIT_ALL),
            .warn          = warn,
        };
        ui_update(&u);
    }
}

static void net_task(void *arg)
{
    (void)arg;
    const vfo_cfg_t *cfg = net_prov_cfg();
    char ip[32] = { 0 };
    bool started = false;

    for (;;) {
        if (!net_prov_is_connected()) {
            started = false;
        } else if (!started) {
            ESP_LOGI(TAG, "--- M12 WiFi up ---");
            if (net_prov_resolve(ip, sizeof ip) == ESP_OK) {
                ESP_LOGI(TAG, "  AetherSDR %s -> ws://%s:%u",
                         cfg->tci_host, ip, (unsigned)cfg->tci_port);
                ESP_LOGI(TAG, "--- M13 TCI client --- (free internal %u, "
                              "largest DMA %u)",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
                if (tci_client_start(ip, cfg->tci_port) == ESP_OK) {
                    started = true;
                } else {
                    /* Usually means internal RAM was too tight to spawn the
                     * WebSocket task. Retrying is right: memory pressure is
                     * transient, and giving up leaves the knob permanently
                     * deaf with no indication why. */
                    ESP_LOGE(TAG, "  client failed to start; retrying");
                }
            } else {
                ESP_LOGE(TAG, "  cannot resolve %s", cfg->tci_host);
            }
        }

        if (started) {
            tci_status_t st;
            tci_get_status(&st);
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

static void boot_ok_cb(void *arg)
{
    (void)arg;
    net_prov_boot_ok();
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "VFO-Knob | ESP32-S3 rev%d.%d, %d core(s), reset=%d",
             chip.revision / 100, chip.revision % 100, chip.cores,
             (int)esp_reset_reason());

#if CONFIG_VFO_GPIO_SCAN
    bring_up("nvs", net_prov_init);
    bring_up("board", board_init);
    xTaskCreatePinnedToCore(gpio_scan_task, "enctest", 4096, NULL, 5, NULL, 1);
    return;
#else
    bring_up("nvs", net_prov_init);

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

    bring_up("knob", hal_encoder_init);

    if (!safe) {
        bring_up("audio-out", audio_out_init);
        bring_up("mic", audio_in_init);
    }

    /* Declare the boot healthy once we have been up a while. Anything that
     * panics before this leaves the counter raised and edges us toward safe
     * mode on the next attempt. */
    const esp_timer_create_args_t ok = { .callback = boot_ok_cb, .name = "bootok" };
    esp_timer_handle_t okt;
    if (esp_timer_create(&ok, &okt) == ESP_OK)
        esp_timer_start_once(okt, 20 * 1000 * 1000);

    if (!safe) {
        esp_err_t werr = net_prov_wifi_start();
        if (werr != ESP_OK)
            ESP_LOGE(TAG, "wifi     FAILED: %s -- continuing offline",
                     esp_err_to_name(werr));
        xTaskCreatePinnedToCore(net_task, "net_sup", 4096, NULL, 3, NULL, 0);
    }

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
