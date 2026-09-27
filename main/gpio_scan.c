/* Focused encoder A/B analyser.
 *
 * A=8 / B=7 is confirmed by three independent sources (the EmbeddedWizard BSP,
 * Waveshare's own 04_Encoder_Test, and the community hardware reference), yet
 * the broad scan saw GPIO8 toggling while GPIO7 sat at 99-100% high under both
 * pull-up AND pull-down bias. Under a pull-down that means something external
 * is actively holding it up.
 *
 * This build initialises NOTHING else -- no I2C, no haptics, no PCNT -- so
 * nothing can be blamed on interference. It samples both lines fast and
 * histograms the four quadrature states. A working encoder visits all four;
 * a stuck B line visits only two.
 */
#include "gpio_scan.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "enctest";

#define PIN_A 8
#define PIN_B 7

void gpio_scan_task(void *arg)
{
    (void)arg;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_A) | (1ULL << PIN_B),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    ESP_LOGI(TAG, "A=gpio%d B=gpio%d, pull-ups on. TURN THE KNOB.", PIN_A, PIN_B);
    ESP_LOGI(TAG, "a working quadrature encoder visits all four states;");
    ESP_LOGI(TAG, "a stuck B line visits only two.");

    uint32_t state_hits[4] = { 0 };
    uint32_t edges_a = 0, edges_b = 0, samples = 0;
    int la = gpio_get_level(PIN_A), lb = gpio_get_level(PIN_B);
    int64_t t_report = esp_timer_get_time();

    for (;;) {
        for (int rep = 0; rep < 40; rep++) {
            int a = gpio_get_level(PIN_A);
            int b = gpio_get_level(PIN_B);
            if (a != la) { la = a; edges_a++; }
            if (b != lb) { lb = b; edges_b++; }
            state_hits[(a << 1) | b]++;
            samples++;
            esp_rom_delay_us(50);
        }
        vTaskDelay(1);   /* feed the idle task or the WDT panics */

        int64_t now = esp_timer_get_time();
        if (now - t_report >= 2000000) {
            t_report = now;
            int visited = 0;
            for (int i = 0; i < 4; i++) if (state_hits[i] > 0) visited++;
            ESP_LOGI(TAG,
                "A edges=%-5u  B edges=%-5u | AB 00=%u%% 01=%u%% 10=%u%% 11=%u%% "
                "| states visited=%d",
                (unsigned)edges_a, (unsigned)edges_b,
                (unsigned)(state_hits[0] * 100 / samples),
                (unsigned)(state_hits[1] * 100 / samples),
                (unsigned)(state_hits[2] * 100 / samples),
                (unsigned)(state_hits[3] * 100 / samples),
                visited);
            edges_a = edges_b = samples = 0;
            for (int i = 0; i < 4; i++) state_hits[i] = 0;
        }
    }
}
