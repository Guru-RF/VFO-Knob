# SVXConnect on the knob

A feasibility study, not a plan of record. Could
[SVXConnect-CLI](https://github.com/Guru-RF/SVXConnect-CLI) — the pure-C client
for SvxLink v3 reflectors: mutual TLS with X.509 client certificates, AES-128-GCM
encrypted Opus over UDP, talkgroups — run on this knob instead of the AetherSDR
firmware?

Looked at on 2026-09-28, against SVXConnect-CLI `d8f4158` (0.1.4) and this repo
at v1.4.1. Parked until there is time for it. Figures marked *measured* come
from the knob or a published benchmark; *estimated* ones have not been checked
on this board.

## Verdict

**Feasible, with caveats, and pure C throughout** — ESP-IDF, mbedTLS, a
fixed-point libopus and LVGL are all C, and SVXConnect's only non-C file is a
macOS helper that is dropped anyway.

- It is a **separate, WiFi-only build** of this repo, the same way `TODO.md`
  plans the Icom variant. It does not fit beside the TCI client: internal RAM
  is the binding constraint.
- The **USB-C link cannot reach a reflector**. It has no gateway by design
  (`components/usb_net/usb_net.c`), and routing through the host would need NAT
  on the computer — against the "nothing to install" promise.

## Shape

- `sdkconfig.svx`, an overlay like `sdkconfig.usbnet`: USB networking off, TCI
  not linked. First-run setup through the SoftAP portal from `TODO.md`, or a
  minimal NCM fallback (1+1 NTB buffers, ~6.4 kB) for setup only.
- `components/svx`: the ported C11 core behind a thin `svx_port.h` shim —
  PSRAM allocation, sleeps, fixed paths, eventfd.
- `components/opus`: xiph libopus, **fixed-point**, vendored. Not Espressif's
  `esp_audio_codec`, which is a closed library restricted to Espressif products.
- Tasks, all on core 0, all with PSRAM stacks and static internal TCBs, never
  deleted (the pattern in `components/ota/ota.c` — deleting a PSRAM-stack task
  makes IDF allocate a helper in internal RAM, and abort if it cannot):
  - `svx_svc` — poll loop, TLS, UDP, GCM, jitter buffer, decode (~16 kB stack)
  - `svx_conn` — connect / handshake worker
  - `svx_enc` — runs only while keyed, paced by the PDM DMA
  - `svx_store` — an **internal**-stack task (~3 kB) doing every NVS write from
    a queue: a task whose stack is in PSRAM must not write flash
    (`esp_system/.../cache_utils.c` asserts it)
- Config, key and certificates in NVS (~5 kB of PEM). SNTP on WiFi.
- Before any release: its own update channel (`firmware/svx/`) and a
  project-name check on uploads and OTA — today any correctly signed image
  installs, so an SVX knob would take the AetherSDR release. Same signing key
  and `partitions.csv`, so either firmware swaps for the other through the
  configuration page's file upload.

## What carries over

- **Unchanged** (19 files, 1,523 lines): `common/proto`, `common/ring`,
  `audio/playout`, `audio/jitter`, `audio/watchdog`, `audio/dev.h`,
  `tg/tgmanager`, `reflector/cert`, `reflector/nodeinfo`, `reflector/frameio`.
- **Small edits** (20 files, 3,701 lines): `handshake`/`client` (replace
  `pipe()` with `esp_vfs_eventfd`, set thread stacks via `esp_pthread_cfg_t`),
  `crypto` (only the ~50-line GCM core, to `mbedtls_gcm`; `RAND_bytes` →
  `esp_fill_random`), `net` (drop the libresolv SRV lookup — it already falls
  back to host:port), `util`/`log`/`config` (no `getenv`, `nanosleep`,
  `fchmod`), `codec` (lower complexity, one encoder and one decoder), `app`,
  `enroll`.
- **From the knob**: `ptt_fsm`, with a transmit time-out added back for this
  build (see 6); the PTT slab and red TX face, the warning panel, the update
  screens, and the turn-to-choose / tap-to-accept editor for talkgroups. The
  knob turning through talkgroups is the natural control.
- **Audio stays at 24 kHz**: `opus_decoder_create(24000, 2)` writes exactly
  the play ring's int16 stereo format; `opus_encoder_create(24000, 1)` with
  `OPUS_SET_MAX_BANDWIDTH(WIDEBAND)` takes the PDM mic's 480-sample frames.
- **mbedTLS config needs no change**: TLS 1.2 client, GCM, PEM/PK write, CSR
  write and prime generation are compiled in; PSRAM allocation is already on;
  the 16 kB input record buffer must stay.
- **Flash**: fixed-point libopus is ~280 kB against ~1.3 MB free in the 3 MB
  slot.

## What gets rewritten

- `common/tls.c` (39 OpenSSL calls) → `mbedtls_ssl_*`: TLS 1.2 pinned, no
  server verification (as today), own certificate, `set_bio` on the lwIP fd,
  non-blocking handshake in 100 ms poll slices. The buffering design stays.
- `common/pki.c` (54 OpenSSL calls) → `mbedtls_rsa_gen_key`,
  `mbedtls_x509write_csr`, `mbedtls_x509_crt_parse`, plus a small `timegm`.
  mbedTLS writes keyUsage non-critical and has no setters for
  basicConstraints / extendedKeyUsage in a CSR (hand-built DER) — harmless, the
  reflector replaces the CSR's extensions.
- `audio/dev_miniaudio.c` (+ the 95,864-line `miniaudio.h`) → a `dev_i2s.c`
  of ~150–300 lines over `audio_out` / `audio_in`.
- `ui/ui.c` (ncurses) and `main.c` → an LVGL SVX face and `app_main` wiring:
  the talker's callsign on the big row, the S-meter arc as speaker level, the
  TX arc as mic peak; `encoder_task`'s `tci_tune_by()` becomes talkgroup
  selection.
- New: SNTP (the firmware has none; certificate expiry and renewal need wall
  time).
- Dropped (13 files, 689 lines): `ctl/ctlfifo`, `common/lock`,
  `common/status` (1 Hz flash writes), `conftemplate`, `audiotest`, signal
  handling, `mic_tcc.m`.

## The hard parts, in order

1. **Internal RAM.** *Measured* on WiFi: ~10 kB free with TCI running; 15 kB
   (6.6 kB largest block) with TinyUSB not started. An SVX-only WiFi build is
   *estimated* at ~34 kB free before the client starts — the 15 kB plus the
   19.2 kB of static NCM buffers it would not link. Traps:
   - every `malloc` of 16 kB or less goes to internal RAM first
     (`SPIRAM_MALLOC_ALWAYSINTERNAL=16384`), and SVXConnect's TLS out-queue
     and playback ring are exactly 16,384 B — use
     `heap_caps_malloc(MALLOC_CAP_SPIRAM)` everywhere;
   - lwIP/WiFi buffers stay internal by design; each queued UDP datagram pins
     a ~1.6 kB WiFi RX buffer, up to ~10 kB if the service task falls behind.
   Headroom if needed: WiFi static RX buffers 10 → 4 (+9.6 kB); the LVGL pool
   to PSRAM (+64 kB, which would help the TCI build too).
2. **WiFi only, in a metal case.** The USB build exists because the machined
   case makes 2.4 GHz unreliable. SVX needs 50 packets/s each way with no DTX;
   the receiver conceals at most 8 lost frames and does not decode in-band FEC
   (`codec.c`, `decode_fec=0`). No firmware change fixes the RF.
3. **Opus encode CPU.** SVXConnect encodes at complexity 10. A published
   fixed-point benchmark on an ESP32-S3, 16 kHz mono, *measured* per 20 ms
   frame: 13.6 ms at complexity 10 (and 8), 9.6 ms at 5, 6.8 ms at 2, 5.2 ms
   at 0; floating point misses real time even at 0. That ran with 64 kB D- /
   32 kB I-cache; the knob has 32 / 16 kB, so expect slower — unmeasured.
   Complexity must drop to ~2–5 (encoder-only, the wire format is unchanged).
   The codec wants ~23–28 kB of stack, in PSRAM. Decode is ~2%.
4. **TLS porting traps.**
   - `tls_flush()` retries a write with a different length after
     `WANT_WRITE` (OpenSSL's `ACCEPT_MOVING_WRITE_BUFFER`). `mbedtls_ssl_write`
     requires the same arguments on retry; ported naively it silently drops
     bytes. Remember the in-flight chunk length.
   - Certificate-rejection detection uses OpenSSL reason codes. mbedTLS only
     returns `MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE`; the alert number is in a
     private field.
   - Per connect ~0.3–1 s of CPU (RSA-2048 client signature ~118 ms, ECDHE
     60–130 ms); a session takes 30–45 kB of PSRAM.
5. **Enrolment on the device.** RSA-2048 key generation takes seconds to tens
   of seconds (IDF's own test allows 60 s) — run it at low priority, clear of
   the 5 s task watchdog. Entropy is truly random only with WiFi up. The key
   sits in **plaintext flash**, readable with `esptool read_flash`: this board
   deliberately has no Secure Boot or flash encryption, and NVS encryption or
   the DS peripheral would each burn an eFuse — against the "nothing
   irreversible" rule.
6. **PTT safety turns around.** With TCI a *dead* knob can leave the radio
   keyed; with SVX a *hung but alive* knob keeps sending 50 frames/s and keys
   a whole reflector network. Every frame sent checks that `ptt_fsm` is ON,
   and a transmit time-out is the primary guard — which this build has to
   bring back: the AetherSDR firmware dropped its own in favour of the
   radio's, and there is no radio behind a reflector. SVXConnect's 120 s is
   the model. The 6 s pong-stale abort cannot
   be copied — the reflector only heartbeats after ~10 s idle and SVXConnect's
   receive timeout is 30 s — so ~15 s or more. Confirmation becomes the
   reflector's talker-start naming us.

## Budgets (estimated unless marked)

| | |
|---|---|
| Internal RAM, SVX's own | ~6–8 kB steady, ~20 kB worst case; stay above 16 kB free with a 6 kB block |
| PSRAM | ~0.3–0.6 MB: Opus 29 kB encoder + 18 kB decoder, 30–120 kB codec scratch, 30–45 kB TLS, 48 kB rings. SVXConnect's desktop buffers (256 kB frames, 1 MiB TLS queue cap, 125 kB log ring) cut 8–16× |
| CPU | TX encode 5.2 / 6.8 / 9.6 / 13.6 ms per 20 ms at complexity 0 / 2 / 5 / 10 (*measured*, bigger caches); decode ~2%; AES-GCM <0.5%; handshake 0.3–1 s per connect; key generation once |
| Network | ~37 kbit/s each way while talking |
| Flash | ~280 kB for libopus, ~1.3 MB free |

## First experiment: `svx-probe`

About 1–2 days. It measures the two numbers that decide the port and that
nobody has measured on this board.

A throwaway build: WiFi only (no TinyUSB), TCI client not started, fixed-point
libopus as a component. One static-TCB task with a 32 kB PSRAM stack on core 0:

1. After WiFi and LVGL are up, log internal free memory, the largest block and
   `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`.
2. With the dial animating, encode 30 s of canned 16 kHz speech from flash at
   complexity 0, 2, 5 and 10 (20 kbit/s, VOICE, FEC on); log µs per frame,
   average and maximum.
3. Open a mbedTLS TLS 1.2 connection with a client certificate made on a PC,
   to `openssl s_server -tls1_2 -Verify 1` on the LAN (or a real reflector),
   and exchange ~60-byte UDP datagrams at 50/s each way for 60 s; log the
   minimum internal free memory and largest block throughout.

**Pass**: at complexity ≥ 2, encode averages ≤ 8 ms and never exceeds 15 ms per
20 ms frame, and internal RAM never drops below 16 kB free with a 6 kB block. If
it fails, move the LVGL pool to PSRAM and cut WiFi RX buffers to 4 before
anything else.
