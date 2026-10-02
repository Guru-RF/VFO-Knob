#include "drv2605.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "drv2605";

/* Every transaction is bounded. The default (-1) would let a sick chip hold
 * the shared bus indefinitely, and the touch controller is on it. */
#define XFER_TIMEOUT_MS 20
#define ERR_STREAK_FAULT 3

#define REG_STATUS        0x00
#define REG_MODE          0x01
#define REG_RTPIN         0x02
#define REG_LIBRARY       0x03
#define REG_WAVESEQ1      0x04
#define REG_WAVESEQ2      0x05
#define REG_GO            0x0C
#define REG_RATED_VOLTAGE 0x16
#define REG_OD_CLAMP      0x17
#define REG_A_CAL_COMP    0x18
#define REG_A_CAL_BEMF    0x19
#define REG_FEEDBACK_CTRL 0x1A
#define REG_CTRL1         0x1B
#define REG_CTRL2         0x1C
#define REG_CTRL3         0x1D
#define REG_CTRL4         0x1E

#define MODE_INTTRIG      0x00
#define MODE_RTP          0x05
#define MODE_AUTOCAL      0x07
#define MODE_STANDBY      0x40
#define MODE_DEV_RESET    0x80

#define STATUS_DIAG_RESULT (1u << 3)

/* A 3 V coin ERM driven from 3.3 V. These are starting points for
 * auto-calibration, which then measures the actuator's real back-EMF. */
#define ERM_RATED_VOLTAGE 0x3F
#define ERM_OD_CLAMP      0x89
#define LRA_RATED_VOLTAGE 0x3E
#define LRA_OD_CLAMP      0x8C

static esp_err_t wr(drv2605_t *d, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    esp_err_t err = i2c_master_transmit(d->dev, buf, 2, XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        if (++d->err_streak >= ERR_STREAK_FAULT && !d->faulted) {
            d->faulted = true;
            ESP_LOGE(TAG, "latched fault after %d consecutive I2C errors; "
                          "haptics disabled so the touch path stays clean",
                     d->err_streak);
        }
    } else {
        d->err_streak = 0;
    }
    return err;
}

static esp_err_t rd(drv2605_t *d, uint8_t reg, uint8_t *val)
{
    esp_err_t err = i2c_master_transmit_receive(d->dev, &reg, 1, val, 1,
                                                XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        if (++d->err_streak >= ERR_STREAK_FAULT) d->faulted = true;
    } else {
        d->err_streak = 0;
    }
    return err;
}

bool drv2605_ok(const drv2605_t *d) { return d && !d->faulted; }

esp_err_t drv2605_read_status(drv2605_t *d, uint8_t *status)
{
    return rd(d, REG_STATUS, status);
}

esp_err_t drv2605_init(drv2605_t *d, i2c_master_bus_handle_t bus,
                       uint8_t addr, drv_actuator_t actuator)
{
    if (!d) return ESP_ERR_INVALID_ARG;
    d->actuator    = actuator;
    d->library     = DRV_LIB_EMPTY;
    d->last_effect = 0xFF;
    d->faulted     = false;
    d->err_streak  = 0;
    d->rated       = actuator == DRV_ACTUATOR_ERM ? ERM_RATED_VOLTAGE : LRA_RATED_VOLTAGE;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &d->dev),
                        TAG, "add device");

    ESP_RETURN_ON_ERROR(wr(d, REG_MODE, MODE_DEV_RESET), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(wr(d, REG_MODE, MODE_INTTRIG), TAG, "wake");   /* clears STANDBY */
    ESP_RETURN_ON_ERROR(wr(d, REG_RTPIN, 0x00),        TAG, "rtp0");

    if (actuator == DRV_ACTUATOR_ERM) {
        ESP_RETURN_ON_ERROR(wr(d, REG_RATED_VOLTAGE, ERM_RATED_VOLTAGE), TAG, "rv");
        ESP_RETURN_ON_ERROR(wr(d, REG_OD_CLAMP,      ERM_OD_CLAMP),      TAG, "od");
        /* N_ERM_LRA=0, FB_BRAKE_FACTOR=2 (4x), LOOP_GAIN=2 (high) */
        ESP_RETURN_ON_ERROR(wr(d, REG_FEEDBACK_CTRL, 0x28), TAG, "fb");
        /* Closed loop: leave ERM_OPEN_LOOP clear. Braking is what makes a
         * click a click, and open loop gives up most of it. */
        ESP_RETURN_ON_ERROR(wr(d, REG_CTRL3, 0x80), TAG, "c3");  /* NG_THRESH=2 */
    } else {
        ESP_RETURN_ON_ERROR(wr(d, REG_RATED_VOLTAGE, LRA_RATED_VOLTAGE), TAG, "rv");
        ESP_RETURN_ON_ERROR(wr(d, REG_OD_CLAMP,      LRA_OD_CLAMP),      TAG, "od");
        ESP_RETURN_ON_ERROR(wr(d, REG_FEEDBACK_CTRL, 0xA8), TAG, "fb");  /* N_ERM_LRA=1 */
        ESP_RETURN_ON_ERROR(wr(d, REG_CTRL3, 0x81), TAG, "c3");  /* LRA_OPEN_LOOP=0 */
    }

    /* BIDIR_INPUT=1 (bidirectional), BRAKE_STABILIZER=1, SAMPLE_TIME=3,
     * BLANKING=1, IDISS=1. Library B requires bidirectional closed loop. */
    ESP_RETURN_ON_ERROR(wr(d, REG_CTRL2, 0xF5), TAG, "c2");

    uint8_t st = 0;
    ESP_RETURN_ON_ERROR(rd(d, REG_STATUS, &st), TAG, "status");
    ESP_LOGI(TAG, "init ok, actuator=%s, STATUS=0x%02X",
             actuator == DRV_ACTUATOR_ERM ? "ERM" : "LRA", st);
    return ESP_OK;
}

esp_err_t drv2605_autocal(drv2605_t *d, drv_calibration_t *cal)
{
    if (!d || !cal) return ESP_ERR_INVALID_ARG;
    cal->valid = false;

    ESP_RETURN_ON_ERROR(wr(d, REG_MODE, MODE_AUTOCAL), TAG, "mode");
    /* ZC_DET_TIME=0, AUTO_CAL_TIME=3 (longest, most reliable). Never touch
     * OTP_PROGRAM -- burning the one-time fuses is irreversible and we cache
     * the result in NVS instead. */
    ESP_RETURN_ON_ERROR(wr(d, REG_CTRL4, 0x30), TAG, "c4");
    ESP_RETURN_ON_ERROR(wr(d, REG_GO, 0x01),    TAG, "go");

    uint8_t go = 1;
    for (int i = 0; i < 100 && go; i++) {       /* auto-cal takes ~1.2 s worst case */
        vTaskDelay(pdMS_TO_TICKS(20));
        if (rd(d, REG_GO, &go) != ESP_OK) return ESP_FAIL;
        go &= 1;
    }
    if (go) {
        ESP_LOGE(TAG, "auto-cal did not complete");
        return ESP_ERR_TIMEOUT;
    }

    uint8_t st = 0;
    ESP_RETURN_ON_ERROR(rd(d, REG_STATUS, &st), TAG, "status");
    ESP_RETURN_ON_ERROR(rd(d, REG_A_CAL_COMP,    &cal->a_cal_comp),    TAG, "comp");
    ESP_RETURN_ON_ERROR(rd(d, REG_A_CAL_BEMF,    &cal->a_cal_bemf),    TAG, "bemf");
    ESP_RETURN_ON_ERROR(rd(d, REG_FEEDBACK_CTRL, &cal->feedback_ctrl), TAG, "fb");
    ESP_RETURN_ON_ERROR(rd(d, REG_RATED_VOLTAGE, &cal->rated_voltage), TAG, "rv");
    ESP_RETURN_ON_ERROR(rd(d, REG_OD_CLAMP,      &cal->od_clamp),      TAG, "od");

    ESP_RETURN_ON_ERROR(wr(d, REG_MODE, MODE_INTTRIG), TAG, "mode back");

    /* SLOS854D Table 4: DIAG_RESULT 0 = pass. */
    if (st & STATUS_DIAG_RESULT) {
        ESP_LOGW(TAG, "auto-cal FAILED (STATUS=0x%02X) -- usually the wrong "
                      "actuator type, an open circuit, or a stalled motor", st);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cal->valid = true;
    ESP_LOGI(TAG, "auto-cal PASS: comp=0x%02X bemf=0x%02X fb=0x%02X "
                  "(bemf_gain=%u)",
             cal->a_cal_comp, cal->a_cal_bemf, cal->feedback_ctrl,
             (unsigned)(cal->feedback_ctrl & 0x03));
    return ESP_OK;
}

esp_err_t drv2605_apply_cal(drv2605_t *d, const drv_calibration_t *cal)
{
    if (!d || !cal || !cal->valid) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(wr(d, REG_RATED_VOLTAGE, cal->rated_voltage), TAG, "rv");
    ESP_RETURN_ON_ERROR(wr(d, REG_OD_CLAMP,      cal->od_clamp),      TAG, "od");
    ESP_RETURN_ON_ERROR(wr(d, REG_A_CAL_COMP,    cal->a_cal_comp),    TAG, "comp");
    ESP_RETURN_ON_ERROR(wr(d, REG_A_CAL_BEMF,    cal->a_cal_bemf),    TAG, "bemf");
    ESP_RETURN_ON_ERROR(wr(d, REG_FEEDBACK_CTRL, cal->feedback_ctrl), TAG, "fb");
    return ESP_OK;
}

esp_err_t drv2605_set_library(drv2605_t *d, drv_library_t lib)
{
    ESP_RETURN_ON_ERROR(wr(d, REG_LIBRARY, (uint8_t)lib), TAG, "lib");
    d->library = lib;
    return ESP_OK;
}

esp_err_t drv2605_fire(drv2605_t *d, uint8_t effect)
{
    if (!d || d->faulted) return ESP_ERR_INVALID_STATE;

    /* During a steady spin the effect is usually unchanged, so skipping the
     * waveform write halves the bus time per click (73 us instead of 146). */
    if (effect != d->last_effect) {
        ESP_RETURN_ON_ERROR(wr(d, REG_WAVESEQ1, effect), TAG, "seq1");
        ESP_RETURN_ON_ERROR(wr(d, REG_WAVESEQ2, 0),      TAG, "seq2");
        d->last_effect = effect;
    }
    return wr(d, REG_GO, 0x01);
}

esp_err_t drv2605_rtp_begin(drv2605_t *d)
{
    ESP_RETURN_ON_ERROR(wr(d, REG_RTPIN, 0), TAG, "rtp0");
    return wr(d, REG_MODE, MODE_RTP);
}

esp_err_t drv2605_rtp_write(drv2605_t *d, uint8_t amplitude)
{
    return wr(d, REG_RTPIN, amplitude);
}

esp_err_t drv2605_rtp_begin_at(drv2605_t *d, uint8_t rated)
{
    if (!d || d->faulted) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(wr(d, REG_RATED_VOLTAGE, rated), TAG, "rv");
    return drv2605_rtp_begin(d);
}

esp_err_t drv2605_rtp_end(drv2605_t *d)
{
    ESP_RETURN_ON_ERROR(wr(d, REG_RTPIN, 0), TAG, "rtp0");
    d->last_effect = 0xFF;          /* mode change invalidates the cache */
    ESP_RETURN_ON_ERROR(wr(d, REG_RATED_VOLTAGE, d->rated), TAG, "rv");
    return wr(d, REG_MODE, MODE_INTTRIG);
}
