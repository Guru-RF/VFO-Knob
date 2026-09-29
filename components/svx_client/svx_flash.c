/* NVS from a task with its stack in PSRAM. See svx_flash.h. */
#include "svx_flash.h"

#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "svx";

typedef struct {
    void (*fn)(void *);
    void *arg;
    SemaphoreHandle_t done;
} job_t;

static void runner(void *p)
{
    job_t *j = p;
    j->fn(j->arg);
    xSemaphoreGive(j->done);
    vTaskDelete(NULL);
}

void svx_flash_safe(void (*fn)(void *), void *arg)
{
    if (!esp_ptr_external_ram((const void *)esp_cpu_get_sp())) {
        fn(arg);
        return;
    }
    job_t j = { .fn = fn, .arg = arg, .done = xSemaphoreCreateBinary() };
    if (j.done && xTaskCreatePinnedToCore(runner, "svxnvs", 4096, &j,
                                          uxTaskPriorityGet(NULL), NULL,
                                          xPortGetCoreID()) == pdPASS) {
        xSemaphoreTake(j.done, portMAX_DELAY);
    } else {
        ESP_LOGE(TAG, "no internal RAM for an NVS access");
    }
    if (j.done) vSemaphoreDelete(j.done);
}
