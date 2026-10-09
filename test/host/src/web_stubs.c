/* What webcfg.c asks of the rest of the knob, for owrx_web_host: the parts
 * the configuration and radio pages only read or pass on -- OTA, the
 * Bluetooth chip, the face, the board, WiFi, the knob's own settings -- as
 * a new knob has them, doing nothing. The receivers' list and the receiver
 * itself are the real ones (owrx_host.c, components/owrx_client). The page's
 * login is the shipped admin/admin. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_in.h"
#include "audio_out.h"
#include "board.h"
#include "bt_level.h"
#include "bt_link.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "net_prov.h"
#include "ota.h"
#include "shim.h"
#include "ui.h"

/* ------------------------------------------------- the knob's settings */

static vfo_cfg_t s_cfg = { .radio_port = 8073 };
static uint8_t   s_vol = 60;

const vfo_cfg_t *net_prov_cfg(void) { return &s_cfg; }
esp_err_t net_prov_save_cfg(const vfo_cfg_t *cfg) { s_cfg = *cfg; return ESP_OK; }
bool net_prov_ap_active(void) { return false; }
uint16_t net_prov_blank_min(void) { return 0; }
uint16_t net_prov_dim_min(void) { return 5; }
uint8_t net_prov_boot_count(void) { return 0; }
uint8_t net_prov_volume(void) { return s_vol; }
uint8_t net_prov_mic_gain(void) { return 100; }
uint8_t net_prov_mic_gain_headset(void) { return 100; }
uint16_t net_prov_ota_hours(void) { return 0; }
void net_prov_save_audio(uint8_t volume, uint8_t mic, uint8_t mic_hs) { s_vol = volume; (void)mic; (void)mic_hs; }
void net_prov_save_dim(uint16_t dim, uint16_t blank) { (void)dim; (void)blank; }
void net_prov_save_ota_hours(uint16_t hours) { (void)hours; }
void net_prov_save_web(const char *user, const char *pass) { (void)user; (void)pass; }
bool net_prov_web_is_default(void) { return true; }
const char *net_prov_web_user(void) { return "admin"; }
const char *net_prov_web_pass(void) { return "admin"; }
int net_prov_scan(net_prov_net_t *out, int max) { (void)out; (void)max; return 0; }
esp_err_t net_prov_join(const char *ssid, const char *pass) { (void)ssid; (void)pass; return ESP_FAIL; }
net_join_t net_prov_join_state(char *ssid, size_t sn, char *why, size_t wn)
{
    if (sn) ssid[0] = 0;
    if (wn) why[0] = 0;
    return NET_JOIN_IDLE;
}
int net_prov_wifi_count(void) { return 0; }
bool net_prov_wifi_get(int i, net_wifi_t *out) { (void)i; (void)out; return false; }
const char *net_prov_wifi_now(void) { return ""; }
esp_err_t net_prov_wifis_save(const net_wifi_t *list, int n) { (void)list; (void)n; return ESP_OK; }
bool net_prov_peek(const char *key) { (void)key; return false; }
bool net_prov_take_once(const char *key) { (void)key; return false; }

/* ------------------------------------------------- OTA: none here */

#if VFO_RADIO_WEBSDR
const char *ota_radio(void) { return "websdr"; }
#else
const char *ota_radio(void) { return "owrx"; }
#endif
const char *ota_root_url(void) { return ""; }
const char *ota_base_url(void) { return ""; }
bool ota_busy(void) { return false; }
void ota_get_status(ota_status_t *out) { memset(out, 0, sizeof *out); }
bool ota_is_newer(const char *a, const char *b) { (void)a; (void)b; return false; }
void ota_mark_valid(void) {}
esp_err_t ota_set_interval(uint32_t hours) { (void)hours; return ESP_OK; }
esp_err_t ota_start_check(bool install) { (void)install; return ESP_ERR_INVALID_STATE; }
esp_err_t ota_upload_begin(const char *radio, size_t size) { (void)radio; (void)size; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ota_upload_write(const void *d, size_t n) { (void)d; (void)n; return ESP_FAIL; }
esp_err_t ota_upload_end(void) { return ESP_FAIL; }
void ota_upload_abort(void) {}
void ota_watchdog_relax(bool relaxed) { (void)relaxed; }
bool ota_companion_image_ok(const uint8_t *img, size_t n, char *ver, size_t cap, uint8_t sha[8])
{
    (void)img; (void)n; (void)ver; (void)cap; (void)sha;
    return false;
}
void ota_companion_offer(ota_comp_offer_t *out) { memset(out, 0, sizeof *out); }

/* ------------------------------------------------- no second chip */

int bt_level_default(uint8_t kind) { (void)kind; return 0; }
bool bt_level_ok(long db) { (void)db; return false; }
int bt_link_battery(void) { return -1; }
bool bt_link_boom_ptt(void) { return false; }
void bt_link_connect(const uint8_t bda[6]) { (void)bda; }
void bt_link_disconnect(void) {}
void bt_link_forget(const uint8_t bda[6]) { (void)bda; }
int bt_link_found(btl_found_t *out, int max) { (void)out; (void)max; return 0; }
bool bt_link_headset_connected(void) { return false; }
int bt_link_level(const uint8_t bda[6], uint8_t kind, bool *own) { (void)bda; (void)kind; if (own) *own = false; return 0; }
void bt_link_scan(uint8_t seconds) { (void)seconds; }
void bt_link_set_boom_ptt(bool on) { (void)on; }
bool bt_link_set_kind(const uint8_t bda[6], uint8_t kind) { (void)bda; (void)kind; return false; }
bool bt_link_set_level(const uint8_t bda[6], int db) { (void)bda; (void)db; return false; }
uint32_t bt_link_speaker_delay_ms(void) { return 0; }
uint8_t bt_link_speaker_volume(void) { return 0; }
void bt_link_status(bt_link_status_t *out) { memset(out, 0, sizeof *out); }
bool bt_link_update_blocked(const uint8_t sha[8], char *why, size_t cap) { (void)sha; if (cap) why[0] = 0; return false; }
bool bt_link_update_holding(void) { return false; }
esp_err_t bt_link_update_start(uint8_t *img, size_t len, const uint8_t sha256[32], bool forced)
{
    (void)img; (void)len; (void)sha256; (void)forced;
    return ESP_ERR_INVALID_STATE;
}
void bt_link_update_status(bt_link_upd_t *out) { memset(out, 0, sizeof *out); }

/* ------------------------------------------------- the face, the board */

void ui_dim_set_minutes(uint16_t dim, uint16_t blank) { (void)dim; (void)blank; }
void ui_set_levels(uint8_t volume, uint8_t mic) { (void)volume; (void)mic; }
void ui_updating_show(void) {}
void ui_updating_hide(void) {}
void ui_updating_progress(int pct) { (void)pct; }
void ui_updating_result(bool ok, const char *msg) { (void)ok; (void)msg; }
void ui_switching(const char *name) { (void)name; }
void board_power_get(board_power_t *out) { memset(out, 0, sizeof *out); }
void audio_in_set_gain(uint8_t pct) { (void)pct; }
void audio_out_set_volume(uint8_t vol) { (void)vol; }

/* ------------------------------------------------- the chip */

const esp_app_desc_t *esp_app_get_description(void)
{
#if VFO_RADIO_WEBSDR
    static const esp_app_desc_t d = { .version = "1.20.0-host", .project_name = "vfo-knob-websdr",
#else
    static const esp_app_desc_t d = { .version = "1.20.0-host", .project_name = "vfo-knob-owrx",
#endif
                                      .time = "00:00:00", .date = "Oct  8 2026", .idf_ver = "host" };
    return &d;
}
esp_err_t esp_core_dump_image_get(size_t *addr, size_t *size) { (void)addr; (void)size; return ESP_ERR_NOT_FOUND; }
esp_err_t esp_flash_read(esp_flash_t *chip, void *buf, uint32_t addr, uint32_t len)
{
    (void)chip; (void)buf; (void)addr; (void)len;
    return ESP_FAIL;
}
void esp_restart(void)
{
    shim_say("@RESTART");
    exit(0);
}
uint32_t ulTaskGetIdleRunTimeCounterForCore(int core) { (void)core; return 0; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t) { (void)t; return 4096; }
