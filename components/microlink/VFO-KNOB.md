# MicroLink in the VFO Knob

MicroLink 2.3.0 (https://github.com/CamM2325/microlink, tag v2.3.0, commit
b454b16, MIT; its WireGuard is wireguard-lwip, BSD-3-Clause), an unofficial
Tailscale-compatible client, for the tailchaser firmware. Copied from
`components/microlink` of that tag, with these changes:

- **Keys:** `src/microlink.c` keeps no keys in NVS. The machine, WireGuard and
  DISCO private keys come from `microlink_keys_load()` and go to
  `microlink_keys_save()` (weak; the firmware keeps them in its settings store
  on the SD card), and the public keys are derived from them.
  `microlink_factory_reset()` calls `microlink_keys_erase()` and opens no NVS
  namespace.
- **No peer cache:** `src/ml_peer_nvs.c` is replaced by `src/ml_peer_nvs_none.c`,
  which keeps nothing. The knob's NVS is small and shared.
- **Task stacks in PSRAM:** the four tasks are created with
  `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` and leave with
  `vTaskDeleteWithCaps()`. None of them touches flash.
- `idf_component.yml` left out (ESP-IDF 5.5 has cJSON as `json`).
- `sdkconfig.tailchaser` turns `CONFIG_ML_OTA_ROLLBACK` off: `ml_ota.c` reads
  the OTA state from flash, which a task with its stack in PSRAM may not do
  (it asserted in `microlink_start` on the first knob, 2026-11-20). Anything
  new MicroLink does with flash (NVS, partitions, OTA) needs the same care:
  `xtensa-esp32s3-elf-nm -u` on every object under the build's
  `esp-idf/microlink` (wireguard_lwip's included) lists what they call.
- `components/wireguard_lwip/src/wireguard-platform-esp32.c`: the handshake
  timestamp floor goes through the weak hooks `microlink_ts_floor_load` /
  `microlink_ts_floor_save` (the application's settings store) in place of
  NVS namespace `ml_wg`, which the wg_mgr task wrote from its PSRAM stack:
  the second assert on the first knob, at its first WireGuard handshake.
- `CMakeLists.txt`: `ml_peer_nvs_none.c` in place of `ml_peer_nvs.c`, and
  stringop/format truncation warnings not errors (the project's `-Werror`).

To update: copy the new tag's `components/microlink` over this one and make
the same changes again.
