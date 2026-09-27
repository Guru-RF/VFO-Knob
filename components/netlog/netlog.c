#include "netlog.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

/* 16 kB holds the whole boot log with room to spare, and it lives in PSRAM so
 * it costs nothing that the draw buffers or WiFi descriptors want. */
#define RING_SZ 16384

static char           *s_ring;
static size_t          s_total;      /* bytes ever written; never wraps in practice */
static portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t  s_prev;
static TaskHandle_t    s_task;
static void          (*s_pre_reboot)(void);

/* The critical sections here are a bounded memcpy of at most 256 bytes, so a
 * spinlock is cheaper than a mutex and -- unlike a mutex -- cannot deadlock
 * against a log call from inside lwIP while we are sending. */
static void ring_put(const char *p, size_t n)
{
    if (n > RING_SZ) { p += n - RING_SZ; n = RING_SZ; }
    portENTER_CRITICAL(&s_mux);
    size_t off   = s_total % RING_SZ;
    size_t first = RING_SZ - off;
    if (first > n) first = n;
    memcpy(s_ring + off, p, first);
    if (n > first) memcpy(s_ring, p + first, n - first);
    s_total += n;
    portEXIT_CRITICAL(&s_mux);
}

/* Copies out whatever the reader has not seen, advancing *rd. A reader that
 * falls more than a ring behind is fast-forwarded rather than served garbage. */
static size_t ring_read(size_t *rd, char *out, size_t cap)
{
    portENTER_CRITICAL(&s_mux);
    if (s_total - *rd > RING_SZ) *rd = s_total - RING_SZ;
    size_t n = s_total - *rd;
    if (n > cap) n = cap;
    size_t off   = *rd % RING_SZ;
    size_t first = RING_SZ - off;
    if (first > n) first = n;
    memcpy(out, s_ring + off, first);
    if (n > first) memcpy(out + first, s_ring, n - first);
    *rd += n;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

static int log_hook(const char *fmt, va_list ap)
{
    /* va_list is consumed by the first use, so the console copy needs its own. */
    va_list ap2;
    va_copy(ap2, ap);

    char buf[256];
    int  n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) ring_put(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);

    int r = s_prev ? s_prev(fmt, ap2) : n;
    va_end(ap2);
    return r;
}

static void serve(int cs)
{
    /* Start at the oldest byte still retained, so a client that connects after
     * boot still gets the boot log. */
    size_t rd;
    portENTER_CRITICAL(&s_mux);
    rd = s_total > RING_SZ ? s_total - RING_SZ : 0;
    portEXIT_CRITICAL(&s_mux);

    char   out[512];
    char   cmd[16];
    size_t cmdlen = 0;

    for (;;) {
        /* Read first, and on every pass: this both detects a client that has
         * gone away (nothing else would, when there is no log traffic for
         * minutes) and picks up commands even while the log is streaming. */
        char in[32];
        int  r = recv(cs, in, sizeof in, MSG_DONTWAIT);
        if (r == 0) return;
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return;
        for (int i = 0; i < r; i++) {
            if (in[i] == '\n' || in[i] == '\r') {
                cmd[cmdlen] = 0;
                cmdlen = 0;
                /* "reboot" exists because a USB-networking build can paint
                 * itself into a corner: once TinyUSB owns the pads there is no
                 * serial port, and the flash window only opens after a reset
                 * -- which without this needs someone to pull the cable. It is
                 * also the safe direction to fail: rebooting drops the TCI
                 * socket, and AetherSDR unkeys a client that disconnects. */
                if (strcmp(cmd, "reboot") == 0) {
                    const char *m = "rebooting\n";
                    send(cs, m, strlen(m), 0);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    if (s_pre_reboot) s_pre_reboot();
                    esp_restart();
                }
            } else if (cmdlen < sizeof cmd - 1) {
                cmd[cmdlen++] = in[i];
            }
        }

        size_t n = ring_read(&rd, out, sizeof out);
        if (n) {
            if (send(cs, out, n, 0) < 0) return;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void netlog_task(void *arg)
{
    (void)arg;
    for (;;) {
        int ls = socket(AF_INET, SOCK_STREAM, 0);
        if (ls < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        int yes = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

        struct sockaddr_in a = {
            .sin_family      = AF_INET,
            .sin_port        = htons(NETLOG_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 1) != 0) {
            close(ls);
            vTaskDelay(pdMS_TO_TICKS(1000));   /* network not up yet; retry */
            continue;
        }
        for (;;) {
            int cs = accept(ls, NULL, NULL);
            if (cs < 0) break;
            serve(cs);
            close(cs);
        }
        close(ls);
    }
}

esp_err_t netlog_init(void)
{
    if (s_ring) return ESP_OK;

    s_ring = heap_caps_malloc(RING_SZ, MALLOC_CAP_SPIRAM);
    if (!s_ring) return ESP_ERR_NO_MEM;

    s_prev = esp_log_set_vprintf(log_hook);
    return ESP_OK;
}

void netlog_on_reboot(void (*fn)(void))
{
    s_pre_reboot = fn;
}

esp_err_t netlog_start(void)
{
    if (!s_ring) return ESP_ERR_INVALID_STATE;
    if (s_task)  return ESP_OK;

    return xTaskCreatePinnedToCore(netlog_task, "netlog", 4096, NULL, 3,
                                   &s_task, 0) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}
