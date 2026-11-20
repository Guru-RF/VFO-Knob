/* The knob on a tailnet, through MicroLink. See tc_ts.h. */
#include "tc_ts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "kvstore.h"
#include "microlink.h"

static const char *TAG = "tailscale";

#define NS      "tchaser"
#define KEYS    "tskeys"        /* machine, WireGuard and DISCO private keys, 96 bytes */
#define FLOOR   "tsfloor"       /* WireGuard's handshake-timestamp floor, seconds */

static microlink_t *s_ml;
static volatile bool s_failed;

/* -------------------------------------------- MicroLink's keys (hooks) */

bool microlink_keys_load(uint8_t machine[32], uint8_t wg[32], uint8_t disco[32])
{
    uint8_t k[96];
    size_t n = sizeof k;
    kv_handle_t h;
    if (kv_open(NS, &h) != ESP_OK) return false;
    const bool ok = kv_get_blob(h, KEYS, k, &n) == ESP_OK && n == sizeof k;
    kv_close(h);
    if (ok) {
        memcpy(machine, k, 32);
        memcpy(wg, k + 32, 32);
        memcpy(disco, k + 64, 32);
    }
    memset(k, 0, sizeof k);
    return ok;
}

void microlink_keys_save(const uint8_t machine[32], const uint8_t wg[32], const uint8_t disco[32])
{
    uint8_t k[96];
    memcpy(k, machine, 32);
    memcpy(k + 32, wg, 32);
    memcpy(k + 64, disco, 32);
    kv_handle_t h;
    if (kv_open(NS, &h) == ESP_OK) {
        if (kv_set_blob(h, KEYS, k, sizeof k) == ESP_OK) kv_commit(h);
        kv_close(h);
    }
    memset(k, 0, sizeof k);
}

void microlink_keys_erase(void)
{
    kv_handle_t h;
    if (kv_open(NS, &h) == ESP_OK) {
        kv_erase_key(h, KEYS);
        kv_commit(h);
        kv_close(h);
    }
}

/* WireGuard's timestamp floor: a peer drops a handshake whose timestamp is
 * not above the last it took from the knob, and the knob has no clock that
 * survives a restart. Called from MicroLink's tasks (stacks in PSRAM): the
 * settings store writes from tasks of its own, so no flash is touched here.
 * Saved means on the card and read back: a timeout counts as not saved, and
 * WireGuard tries again at its next handshake. */
bool microlink_ts_floor_load(uint64_t *v)
{
    kv_handle_t h;
    if (kv_open(NS, &h) != ESP_OK) return false;
    const bool ok = kv_get_u64(h, FLOOR, v) == ESP_OK;
    kv_close(h);
    return ok;
}

esp_err_t microlink_ts_floor_save(uint64_t v)
{
    kv_handle_t h;
    esp_err_t e = kv_open(NS, &h);
    if (e != ESP_OK) return e;
    e = kv_set_u64(h, FLOOR, v);
    if (e == ESP_OK) e = kv_commit_wait(h, 3000);
    kv_close(h);
    return e;
}

/* ------------------------------------------------------------------ */

void tc_ts_start(const char *auth_key, const char *device_name)
{
    if (s_ml || s_failed) return;
    microlink_config_t cfg = {
        .auth_key    = (auth_key && auth_key[0]) ? auth_key : NULL,
        .device_name = device_name,
    };
    s_ml = microlink_init(&cfg);
    if (!s_ml) {
        ESP_LOGE(TAG, "could not start (no memory?)");
        s_failed = true;
        return;
    }
    if (microlink_start(s_ml) != ESP_OK) {
        ESP_LOGE(TAG, "could not start its tasks");
        microlink_destroy(s_ml);
        s_ml = NULL;
        s_failed = true;
        return;
    }
    ESP_LOGI(TAG, "starting as %s, %s", device_name, cfg.auth_key ? "with an auth key" : "logging in by browser");
}

tc_ts_state_t tc_ts_state(void)
{
    if (s_failed) return TC_TS_FAILED;
    if (!s_ml) return TC_TS_OFF;
    switch (microlink_get_state(s_ml)) {
    case ML_STATE_CONNECTED:   return TC_TS_UP;
    case ML_STATE_NEEDS_LOGIN: return TC_TS_LOGIN;
    case ML_STATE_AUTH_FAILED: return TC_TS_REFUSED;
    default:                   return TC_TS_STARTING;
    }
}

const char *tc_ts_state_word(tc_ts_state_t s)
{
    switch (s) {
    case TC_TS_OFF:      return "off";
    case TC_TS_STARTING: return "connecting";
    case TC_TS_LOGIN:    return "waiting for a login";
    case TC_TS_UP:       return "connected";
    case TC_TS_REFUSED:  return "refused";
    case TC_TS_FAILED:   return "failed to start";
    }
    return "?";
}

bool tc_ts_login_url(char *out, size_t cap)
{
    if (cap) out[0] = 0;
    return s_ml && microlink_get_state(s_ml) == ML_STATE_NEEDS_LOGIN &&
           microlink_get_auth_url(s_ml, out, cap) == ESP_OK;
}

uint32_t tc_ts_ip(void)     { return s_ml ? microlink_get_vpn_ip(s_ml) : 0; }
int      tc_ts_peers(void)  { return s_ml ? microlink_get_peer_count(s_ml) : 0; }

uint32_t tc_ts_resolve(const char *host)
{
    if (!s_ml || microlink_get_state(s_ml) != ML_STATE_CONNECTED) return 0;
    return microlink_resolve(s_ml, host);
}

void tc_ts_forget(void) { microlink_keys_erase(); }

struct tc_ts_conn {
    microlink_tcp_socket_t *s;
};

tc_ts_conn_t *tc_ts_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms)
{
    if (!s_ml || microlink_get_state(s_ml) != ML_STATE_CONNECTED) return NULL;
    microlink_tcp_socket_t *s = microlink_tcp_connect(s_ml, ip, port, timeout_ms);
    if (!s) return NULL;
    tc_ts_conn_t *c = calloc(1, sizeof *c);
    if (!c) {
        microlink_tcp_close(s);
        return NULL;
    }
    c->s = s;
    return c;
}

int tc_ts_send(tc_ts_conn_t *c, const void *p, size_t n)
{
    return c && microlink_tcp_send(c->s, p, n) == ESP_OK ? (int)n : -1;
}

int tc_ts_path(uint32_t ip)
{
    if (!s_ml || microlink_get_state(s_ml) != ML_STATE_CONNECTED) return 0;
    const int n = microlink_get_peer_count(s_ml);
    for (int i = 0; i < n; i++) {
        microlink_peer_info_t p;
        if (microlink_get_peer_info(s_ml, i, &p) == ESP_OK && p.vpn_ip == ip) return p.direct_path ? 1 : 2;
    }
    return 0;
}

int tc_ts_recv(tc_ts_conn_t *c, void *p, size_t n, uint32_t timeout_ms)
{
    return c ? microlink_tcp_recv(c->s, p, n, timeout_ms) : -1;
}

void tc_ts_close(tc_ts_conn_t *c)
{
    if (!c) return;
    microlink_tcp_close(c->s);
    free(c);
}
