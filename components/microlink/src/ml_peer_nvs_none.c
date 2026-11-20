/* VFO-Knob: no peer cache. MicroLink's ml_peer_nvs.c keeps the peers in an NVS
 * blob (up to ~6 kB) so DISCO can start before the control plane answers; the
 * knob's NVS is a small shared partition that holds the WiFi and Bluetooth
 * only. Without the cache the peers come from the first MapResponse, a second
 * or two after each start. */
#include "microlink_internal.h"

esp_err_t ml_peer_nvs_init(void) { return ESP_OK; }
void ml_peer_nvs_deinit(void) {}
esp_err_t ml_peer_nvs_save(const ml_peer_t *peer) { (void)peer; return ESP_OK; }
int ml_peer_nvs_load_all(ml_peer_t *peers, int max_peers) { (void)peers; (void)max_peers; return 0; }
esp_err_t ml_peer_nvs_clear(void) { return ESP_OK; }
esp_err_t ml_peer_nvs_remove(const uint8_t *public_key) { (void)public_key; return ESP_OK; }
void ml_peer_nvs_batch_begin(void) {}
void ml_peer_nvs_batch_end(void) {}
