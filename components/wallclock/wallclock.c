#include "wallclock.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"

static const char *TAG = "clock";

static atomic_bool s_started;
/* Seconds since boot at the last answer, plus one: 0 is never. Written by
 * lwIP's task (the sync callback), read by any. */
static atomic_uint s_synced_at;

static uint32_t uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

/* lwIP's task: no log, nothing slow. */
static void on_sync(struct timeval *tv)
{
    (void)tv;
    atomic_store(&s_synced_at, uptime_s() + 1u);
}

void wallclock_start(void)
{
    if (atomic_exchange(&s_started, true)) return;
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    /* Nobody waits for the first answer: a face asks wallclock_now(). */
    sc.wait_for_sync = false;
    sc.sync_cb       = on_sync;
    const esp_err_t e = esp_netif_sntp_init(&sc);
    if (e != ESP_OK) ESP_LOGE(TAG, "SNTP not started: %s", esp_err_to_name(e));
}

void wallclock_set_zone(const char *rule)
{
    setenv("TZ", rule && rule[0] ? rule : "UTC0", 1);
    tzset();
}

uint32_t wallclock_age_s(void)
{
    const uint32_t at = atomic_load(&s_synced_at);
    return at ? uptime_s() - (at - 1u) : UINT32_MAX;
}

bool wallclock_now(struct tm *local, struct tm *utc, uint16_t *ms)
{
    if (!atomic_load(&s_synced_at)) return false;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const time_t t = tv.tv_sec;
    if (local) localtime_r(&t, local);
    if (utc)   gmtime_r(&t, utc);
    if (ms)    *ms = (uint16_t)(tv.tv_usec / 1000);
    return true;
}
