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
#include <stdlib.h>

#include "board.h"
#include "board_pins.h"
#include "drv2605.h"
#include "gpio_scan.h"
#include "hal_encoder.h"
#include "net_prov.h"
#include "tci_client.h"
#include "panel.h"
#include "vfo_tune.h"

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
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

/* Strongest first. Confirmed distinguishable by hand at M5. */
static const uint8_t LADDER[6] = { 24, 25, 26, 61, 62, 63 };
#define EFF_ROLLOVER 27

/* Velocity bands, detents/s. ERM values: the motor's 20-40 ms rise time is
 * what sets these, so they move with the actuator, not with taste. */
#define V_MODERATE 13.0f
#define V_BRISK    20.0f
#define V_FAST     33.0f
#define V_SPIN     70.0f

/* Hard floor between plays. Below this an ERM has not finished the previous
 * pulse and the two merge into mush. */
#define GOV_FLOOR_US   55000
/* A click this far behind its detent has lost causal binding; playing it makes
 * the knob feel MUSHIER than silence would. Drop, never delay. */
#define GOV_STALE_US   12000

/* One contact pulse per detent per direction -- this is a switch knob, not a
 * quadrature encoder, so there is no x4 multiplier to divide out. */
static int s_counts_per_detent = ENC_COUNTS_PER_DETENT_DEFAULT;

/* Character varies by decade, not just amplitude: the operator learns
 * "heavy = moving fast" in ten minutes and stops looking at the screen. */
static int base_rung(int32_t step_hz)
{
    if (step_hz >= 1000000) return -1;          /* special-cased to EFF_ROLLOVER */
    if (step_hz >= 10000)   return 0;           /* 24, full strength */
    if (step_hz >= 100)     return 2;           /* 26 */
    return 4;                                   /* 62, a whisper */
}

static void encoder_task(void *arg)
{
    (void)arg;
    accel_t accel; accel_init(&accel);
    tune_t  tune;  tune_init(&tune, 14074000, 100);

    TickType_t next       = xTaskGetTickCount();
    int32_t  residue      = 0;
    int64_t  last_play_us = 0;
    int64_t  idle_since   = esp_timer_get_time();
    int64_t  run_counts   = 0;
    bool     moving       = false;
    uint32_t drops        = 0;
    float    v_peak       = 0.0f;

    ESP_LOGI(TAG, "--- M6/M8 ready ---");
    ESP_LOGI(TAG, "  turn RIGHT one full revolution, pause 2 s;");
    ESP_LOGI(TAG, "  then LEFT one full revolution, pause 2 s;");
    ESP_LOGI(TAG, "  then spin hard. You should feel a click per detent.");

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
                              "f=%lld Hz | hap drops=%u",
                         (long long)run_counts, (long)hal_encoder_count(),
                         (double)v_peak, (unsigned)st.raw_a, (unsigned)st.raw_b,
                         (unsigned)st.accepted_a, (unsigned)st.accepted_b,
                         (unsigned)st.rejected,
                         (long long)tune.f_display, (unsigned)drops);
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

        uint8_t mult = accel_update(&accel, detents, now_ms);
        if (accel.v_detents > v_peak) v_peak = accel.v_detents;

        int64_t before = tune.f_display;
        tune_apply(&tune, detents, mult, 5, 1000LL, 75000000LL);
        /* The local model exists only to drive the haptics with zero network
         * latency. The client keeps its own optimistic copy, because that one
         * has to survive reconciliation against the radio. */
        tci_tune_by(detents, mult, tune.step_hz);

        /* --- velocity-aware haptic scheduling ------------------------------
         * Above ~33 det/s an ERM physically cannot render one click per detent
         * (pulses fuse below ~30 ms apart), so instead of buzzing we tick on
         * round-number crossings. The tick then means "you just passed
         * 14.075.00" rather than "four clicks happened": more information per
         * event, not less, which is why it fires one rung STRONGER. */
        float v = accel.v_detents;
        int rung = base_rung(tune.step_hz);
        bool play = true;

        if (rung < 0) {
            /* 1 MHz step: every detent is a rollover. */
            rung = 0;
        } else if (v > V_SPIN) {
            play = false;                       /* silence beats mush */
        } else if (v > V_FAST) {
            int32_t decade = tune.step_hz * 50;
            play = (before / decade) != (tune.f_display / decade);
            if (rung > 0) rung--;               /* stronger: it carries more */
        } else if (v > V_BRISK) {
            rung += 2;
        } else if (v > V_MODERATE) {
            rung += 1;
        }
        if (rung > 5) rung = 5;

        /* A 1 MHz boundary crossing replaces that detent's tick; it never
         * stacks on top of it, because two ERM events inside 30 ms merge. */
        uint8_t effect = LADDER[rung];
        if ((before / 1000000) != (tune.f_display / 1000000)) {
            effect = EFF_ROLLOVER;
            play = true;
        }

        if (play) {
            if (now_us - last_play_us < GOV_FLOOR_US) {
                drops++;                        /* drop, never delay */
            } else if (esp_timer_get_time() - now_us > GOV_STALE_US) {
                drops++;                        /* already too late to bind */
            } else {
                drv2605_fire(&s_drv, effect);
                last_play_us = now_us;
            }
        }
    }
}

/* The TCI client declares this weak so it stays free of a haptic dependency.
 * PTT is the one place haptics are load-bearing: with no error frame and no
 * button, the motor is the only channel that can tell the operator the radio
 * said no. */
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
    ESP_LOGI(TAG, "         p=abort:pong-stale  d=abort:link-down  o=TOT 10s");

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
                ESP_LOGI(TAG, "--- M13 TCI client ---");
                tci_client_start(ip, cfg->tci_port);
                started = true;
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
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

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

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "VFO-Knob bring-up | ESP32-S3 rev%d.%d, %d core(s)",
             chip.revision / 100, chip.revision % 100, chip.cores);

#if !CONFIG_VFO_GPIO_SCAN
    ESP_ERROR_CHECK(board_init());
    report_memory();
    probe_i2c();
    haptic_bringup();
#endif
#if CONFIG_VFO_GPIO_SCAN
    /* Diagnostic build: do NOT claim GPIO 7/8 for PCNT, so the analyser can
     * observe them like any other pin. */
    xTaskCreatePinnedToCore(gpio_scan_task, "enctest", 4096, NULL, 5, NULL, 1);
#else
    ESP_ERROR_CHECK(panel_init());
    ESP_ERROR_CHECK(hal_encoder_init());

    ESP_ERROR_CHECK(net_prov_init());
    ESP_ERROR_CHECK(net_prov_wifi_start());
    xTaskCreatePinnedToCore(net_task, "net_sup", 4096, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 2, NULL, 0);

    /* Core 1 is the "feel" core: encoder, haptics, touch and LVGL. Core 0 is
     * reserved for WiFi and lwIP, whose burst timing we cannot control. */
    xTaskCreatePinnedToCore(encoder_task, "enc_input", 4096, NULL, 15, NULL, 1);
#endif
}
