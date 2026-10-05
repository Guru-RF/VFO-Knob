/* This chip's own firmware, from the knob. See upd.h.
 *
 * The transfer. BEGIN comes on link_rx, which only looks and holds: a
 * bootloader that can go back, not on trial, no headset or speaker --
 * hfp_try_hold(), which also stops calling it meanwhile. The flash work is
 * the `upd` task's, one for each transfer: link_rx hands it DATA and END
 * through a queue and never waits on the flash itself, as the device's audio
 * passes through it. The task writes the slot not running, erasing each 4 kB
 * sector as the data reaches it (esp_ota_begin(SEQUENTIAL)) and hashing as it
 * goes. At END esp_ota_end() checks the image and its RSA signature against
 * the firmware running -- the S3's key, the one trust anchor without secure
 * boot; then otadata switches to it, and the chip restarts into it.
 *
 * The trial. The bootloader starts a new firmware PENDING_VERIFY and, at the
 * next reset of any kind, marks it ABORTED and goes back to the one before.
 * So the new one keeps itself only once the round trip is proven: the knob's
 * KEEP -- sent when it has read this firmware's INFO saying TRIAL -- and 20 s
 * up, or a headset or a speaker come. Two minutes without KEEP and it
 * restarts itself, to go back. A hang goes back too: the bootloader's RTC
 * watchdog stays on into the app (BOOTLOADER_WDT_DISABLE_IN_USER_CODE), and
 * only a trial's main loop feeds it.
 *
 * What went back, and why. The new firmware writes NVS upd/trial as its trial
 * begins. The firmware before, at its first start after, finds the record --
 * or, gone back without one, the slot otadata gave up on -- and names the
 * cause from esp_reset_reason(): a fixed RTC register, so it crosses images,
 * which RTC_NOINIT variables do not. A real failure (a crash or a hang, on
 * trial or before it began, a firmware the bootloader would not load, the
 * crash guard) is refused here for good; a harmless one (the power, no knob)
 * may come again.
 *
 * The crash guard. For the first 20 starts after a firmware is kept, three
 * crashes in a row send it back as well. */
#include "upd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt_link_proto.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_bootloader_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/wdt_hal.h"
#include "hfp.h"
#include "link.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "upd";

#define NVS_NS          "upd"
#define KEEP_AFTER_US   20000000LL      /* up this long, and the knob said KEEP: kept */
#define TRIAL_US        120000000LL     /* no KEEP in this long: back */
#define QUIET_MS        15000           /* a transfer with nothing coming in this long: stopped */
#define PROBATION       20              /* the starts after keeping that the crash guard watches */
#define GUARD_CRASHES   3
#define GUARD_CLEAR_US  600000000LL     /* up 10 minutes: the crashes before no longer count */
#define QUEUE_LEN       (BTL_UPD_WINDOW + 2)    /* the window's DATA, the END, a wake */

/* NVS upd/. Written by one firmware and read by the next, so their layout
 * is as frozen as the protocol's. */
typedef struct __attribute__((packed)) {
    uint8_t sha8[8];                    /* the firmware on trial */
    char    ver[16];
} trial_rec_t;
typedef struct __attribute__((packed)) {
    uint8_t cls;                        /* BTL_BACK_* */
    uint8_t sha8[8];                    /* the firmware that went back */
    char    ver[16];
} back_rec_t;
typedef struct __attribute__((packed)) {
    uint8_t sha8[8];                    /* the firmware kept */
    uint8_t starts;                     /* its starts since, up to PROBATION */
} prob_rec_t;

/* A DATA or an END for the task -- or, empty, a wake. */
typedef struct {
    uint8_t  type;
    uint16_t n;                         /* DATA: bytes in d */
    uint32_t off;                       /* DATA: where they go; END: the size */
    uint8_t  d[BTL_UPD_CHUNK];
} item_t;

/* As upd_boot() found them; read-only after it -- but for the record of what
 * went back, which a transfer's task forgets as it starts to write the slot
 * (one task at a time, and the only one to read it after upd_boot). */
static uint8_t    s_own[8];             /* this firmware's identity: its app_elf_sha256 */
static bool       s_can;                /* it takes updates */
static back_rec_t s_back;               /* NVS upd/back */
static bool       s_have_back;
static char       s_story[151];         /* the boot story */
static bool       s_told;               /* ...gone to the knob (link_rx) */

/* Shared by link_rx, the task and the main loop, under s_lk. */
static portMUX_TYPE    s_lk = portMUX_INITIALIZER_UNLOCKED;
static btl_upd_info_t  s_info;
static btl_upd_begin_t s_b;             /* the transfer's BEGIN */
static volatile bool   s_active;        /* a transfer is open */
static bool            s_going;         /* ...and its READY went out */
static struct {                         /* how the last one ended, for an END said again */
    bool             set;
    uint32_t         size;
    btl_upd_status_t st;
} s_latch;

static volatile uint32_t s_next;        /* bytes written: the task's */
static volatile uint8_t  s_stop;        /* BTL_UPD_WHY_*: link_rx says stop */
static volatile uint8_t  s_stop_knob;   /* ...the knob's own why, for the log */
/* Made at the first BEGIN and kept to the restart, so that neither is ever
 * freed under link_rx. */
static QueueHandle_t     s_q;
static item_t           *s_work;        /* the task's */
static item_t            s_in;          /* link_rx's */
static int64_t           s_no_session_us;

/* The trial. */
static volatile bool s_trial;
static volatile bool s_keep_asked, s_headset_came;
static int64_t       s_keep_retry_us;

/* The crash guard's count: this image's own, in RTC memory, which a crash's
 * restart leaves alone. A power cut leaves noise: the magic, and the image's
 * identity, tell. */
#define RG_MAGIC 0x47445055u
RTC_NOINIT_ATTR static struct {
    uint32_t magic;
    uint8_t  sha8[8];
    uint8_t  crashes, tried;
} s_rg;
static bool s_rg_armed;

/* ---- words ------------------------------------------------------------- */

static const char *hex8(char out[17], const uint8_t *b)
{
    for (int i = 0; i < 8; i++) sprintf(out + 2 * i, "%02x", b[i]);
    return out;
}

/* A version into the records' 16 bytes, as the frames have it: NUL-padded,
 * cut at 16 -- longer versions than that are development builds. */
static void ver16(char out[16], const char *v)
{
    const size_t n = strnlen(v, 16);
    memset(out, 0, 16);
    memcpy(out, v, n);
}

static const char *why_str(uint8_t w)
{
    switch (w) {
    case BTL_UPD_WHY_HEADSET:    return "a headset or a speaker";
    case BTL_UPD_WHY_TRIAL:      return "the firmware running is on trial";
    case BTL_UPD_WHY_BUSY:       return "another image is coming in";
    case BTL_UPD_WHY_SIZE:       return "its size";
    case BTL_UPD_WHY_BEFORE:     return "it went back here before";
    case BTL_UPD_WHY_BOOTLOADER: return "this chip's bootloader cannot go back -- the bench, once";
    case BTL_UPD_WHY_FLASH:      return "the flash";
    case BTL_UPD_WHY_SHA:        return "its SHA-256 is not the one announced";
    case BTL_UPD_WHY_IMAGE:      return "the image did not verify";
    case BTL_UPD_WHY_PROJECT:    return "not a second-chip firmware";
    case BTL_UPD_WHY_QUIET:      return "nothing came for 15 s";
    case BTL_UPD_WHY_KNOB:       return "the knob called it off";
    case BTL_UPD_WHY_MEMORY:     return "no memory for it";
    case BTL_UPD_WHY_NO_SESSION: return "no update going";
    case BTL_UPD_WHY_RESTARTED:  return "the knob restarted";
    default:                     return "?";
    }
}

/* The knob's own why, in its ABORT. */
static const char *knob_why_str(uint8_t w)
{
    switch (w) {
    case BTL_UPD_WHY_BUSY:    return "an over or a call";
    case BTL_UPD_WHY_HEADSET: return "a headset or a speaker";
    case BTL_UPD_WHY_KNOB:    return "its own update";
    default:                  return why_str(w);
    }
}

static const char *reset_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";          /* the EN pin's too, on the ESP32 */
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default:                return "other";
    }
}

static const char *back_str(uint8_t cls)
{
    switch (cls) {
    case BTL_BACK_POWER:   return "the power went before it was kept -- it may come again";
    case BTL_BACK_QUIET:   return "no knob kept it within 2 minutes -- it may come again";
    case BTL_BACK_CRASHED: return "it crashed on trial";
    case BTL_BACK_HUNG:    return "it hung on trial";
    case BTL_BACK_EARLY:   return "it stopped before its trial could begin";
    case BTL_BACK_GUARD:   return "it crashed 3 times in a row after it was kept";
    default:               return "?";
    }
}

/* A firmware that went back for one of these is never taken again. */
static bool real(uint8_t cls)
{
    return cls == BTL_BACK_CRASHED || cls == BTL_BACK_HUNG || cls == BTL_BACK_EARLY || cls == BTL_BACK_GUARD;
}

/* Why the firmware on trial went back, from the reset that ended it. */
static uint8_t back_class(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:                   /* the EN pin counts as power-on on the ESP32 */
    case ESP_RST_BROWNOUT: return BTL_BACK_POWER;
    case ESP_RST_SW:       return BTL_BACK_QUIET;      /* the only restart a trial makes is its 2-minute one */
    case ESP_RST_WDT:      return BTL_BACK_HUNG;       /* the RTC watchdog only a trial feeds */
    default:               return BTL_BACK_CRASHED;    /* a panic, the interrupt or task watchdog, any other */
    }
}

static bool crash_reset(esp_reset_reason_t r)
{
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

/* The firmware in `part`, not the one running, never started: otadata still
 * names it as the one to start, new or on trial, and yet this one runs. The
 * bootloader could not load it -- a firmware the check here took and the
 * bootloader, frozen at the bench, cannot -- and started this one instead on
 * the same boot, without a word in otadata: only the next restart marks it
 * ABORTED. It went back as surely as one that crashed. A plain read of
 * otadata, as esp_ota_get_state_partition()'s. */
static bool unloadable(const esp_partition_t *part, esp_ota_img_states_t st)
{
    return part && (st == ESP_OTA_IMG_NEW || st == ESP_OTA_IMG_PENDING_VERIFY) &&
           esp_ota_get_boot_partition() == part;
}

/* ---- small things -------------------------------------------------------- */

/* The RTC watchdog the bootloader left running: fed on trial, else off --
 * as IDF itself would have had it, off, before app_main. */
static void rwdt(bool feed)
{
    wdt_hal_context_t w = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&w);
    if (feed) wdt_hal_feed(&w);
    else      wdt_hal_disable(&w);
    wdt_hal_write_protect_enable(&w);
}

static bool rec_get(nvs_handle_t h, const char *key, void *rec, size_t size)
{
    size_t n = size;
    return nvs_get_blob(h, key, rec, &n) == ESP_OK && n == size;
}

static void rec_set(nvs_handle_t h, const char *key, const void *rec, size_t size)
{
    const esp_err_t e = nvs_set_blob(h, key, rec, size);
    if (e != ESP_OK) ESP_LOGW(TAG, "NVS %s/%s not written: %s", NVS_NS, key, esp_err_to_name(e));
}

static void status(uint8_t state, uint8_t why, uint32_t next, int32_t err)
{
    const btl_upd_status_t s = { .state = state, .why = why, .next = next, .err = err };
    link_send(BTL_UPD_STATUS, &s, sizeof s);
}

static void send_info(void)
{
    btl_upd_info_t i;
    portENTER_CRITICAL(&s_lk);
    i = s_info;
    portEXIT_CRITICAL(&s_lk);
    link_send(BTL_UPD_INFO, &i, sizeof i);
}

/* ---- the boot story -------------------------------------------------------- */

static void add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void add(const char *fmt, ...)
{
    const size_t n = strlen(s_story);
    if (n + 1 >= sizeof s_story) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_story + n, sizeof s_story - n, fmt, ap);
    va_end(ap);
}

/* At most 150 characters: on the console now, and to the knob at its first
 * HELLO and each HELLO asking -- a knob restarted has lost the lines before. */
static void story(const esp_app_desc_t *me, const esp_partition_t *run, uint32_t bver, esp_reset_reason_t why,
                  bool back_here, bool found, bool stale, uint8_t probation, esp_err_t guard_err)
{
    s_story[0] = 0;
    add("boot: %s in %s", me->version, run->label);
    if (s_trial) add(", on trial");
    if (bver) add("; bootloader %lu", (unsigned long)bver);
    else      add("; a bootloader of no version");
    if (bver < 2)    add(" -- it cannot go back, so it takes no updates until the bench flashes it once");
    else if (!s_can) add(" -- not in an update slot, so it takes no updates");
    if (probation) {
        add("; crash guard: start %u of %d", probation, PROBATION);
        if (s_rg.crashes) add(", %u crash%s in a row", s_rg.crashes, s_rg.crashes == 1 ? "" : "es");
    }
    if (back_here) {
        add("; %.16s went back: %s", s_back.ver, back_str(s_back.cls));
        if (found && stale)
            add(" (it would not load)");
        else if (found && (s_back.cls == BTL_BACK_CRASHED || s_back.cls == BTL_BACK_EARLY))
            add(" (%s)", reset_str(why));
        if (real(s_back.cls)) add(" -- not taken again");
    }
    if (guard_err != ESP_OK) add("; could not go back: %s", esp_err_to_name(guard_err));
    if (!back_here) add("; reset: %s", reset_str(why));
}

/* ---- at the start ----------------------------------------------------------- */

/* Kept, then crashed GUARD_CRASHES times in a row: back to the firmware
 * before. Its own frame, as the call checks that firmware's image (the RSA
 * check, deep), on the main task's 8 kB. Returns only if it could not. */
static __attribute__((noinline)) esp_err_t guard(nvs_handle_t h, const esp_app_desc_t *me)
{
    s_rg.tried = 1;                     /* once a run of crashes: no going round */
    const back_rec_t was  = s_back;
    const bool       had  = s_have_back;
    s_back.cls = BTL_BACK_GUARD;
    memcpy(s_back.sha8, s_own, 8);
    ver16(s_back.ver, me->version);
    rec_set(h, "back", &s_back, sizeof s_back);
    nvs_erase_key(h, "prob");
    nvs_commit(h);
    ESP_LOGW(TAG, "%s crashed %u times in a row since it was kept: going back", me->version, s_rg.crashes);
    const esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Still here: there was nothing to go back to. */
    s_back      = was;
    s_have_back = had;
    if (had) rec_set(h, "back", &s_back, sizeof s_back);
    else     nvs_erase_key(h, "back");
    return e;
}

void upd_boot(void)
{
    const esp_app_desc_t    *me  = esp_app_get_description();
    const esp_reset_reason_t why = esp_reset_reason();
    memcpy(s_own, me->app_elf_sha256, 8);

    /* Where this chip stands, from plain reads only: otadata, the images'
     * descriptions, the bootloader's. (esp_ota_get_last_invalid_partition()
     * and esp_ota_check_rollback_is_possible() would run the RSA check.) */
    const esp_partition_t *run   = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (other == run) other = NULL;
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED, ost = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);  /* not found: left UNDEFINED */
    if (other) esp_ota_get_state_partition(other, &ost);
    esp_app_desc_t od;
    const bool od_ok      = other && esp_ota_get_partition_description(other, &od) == ESP_OK;
    const bool stale      = unloadable(other, ost);     /* it went back on this very boot */
    const bool other_back = ost == ESP_OTA_IMG_ABORTED || ost == ESP_OTA_IMG_INVALID || stale;
    esp_bootloader_desc_t bd;
    const uint32_t bver = esp_ota_get_bootloader_description(NULL, &bd) == ESP_OK ? bd.version : 0;
    const uint8_t  slot = run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? 0
                        : run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 ? 1 : 0xFF;
    s_can   = bver >= 2 && slot != 0xFF;
    s_trial = st == ESP_OTA_IMG_PENDING_VERIFY;   /* only a rollback bootloader makes it so */

    nvs_handle_t h  = 0;
    const bool   nv = nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK;
    trial_rec_t  tr;
    prob_rec_t   pr;
    const bool have_tr = nv && rec_get(h, "trial", &tr, sizeof tr);
    bool       have_pr = nv && rec_get(h, "prob", &pr, sizeof pr);
    s_have_back        = nv && rec_get(h, "back", &s_back, sizeof s_back);
    bool found = false;                 /* what went back, found at this start */

    if (s_trial) {
        /* Where the firmware before looks, if this one goes back. */
        memcpy(tr.sha8, s_own, 8);
        ver16(tr.ver, me->version);
        if (nv) rec_set(h, "trial", &tr, sizeof tr);
    } else {
        if (have_tr && memcmp(tr.sha8, s_own, 8)) {
            /* A new firmware was on trial, and this one runs: it went back,
             * and the reset that ended it says why. */
            s_back.cls = back_class(why);
            memcpy(s_back.sha8, tr.sha8, 8);
            memcpy(s_back.ver, tr.ver, sizeof s_back.ver);
            s_have_back = found = true;
            rec_set(h, "back", &s_back, sizeof s_back);
            nvs_erase_key(h, "trial");
        } else if (have_tr && st == ESP_OTA_IMG_VALID) {
            /* This one was kept, and the power went before its trial record
             * did: the record goes now, and its probation starts. Only as
             * otadata has it kept: one started with nothing else to start,
             * its trial over unkept, has no probation to begin. */
            nvs_erase_key(h, "trial");
            if (!have_pr || memcmp(pr.sha8, s_own, 8)) {
                memcpy(pr.sha8, s_own, 8);
                pr.starts = 0;
                have_pr   = true;
                rec_set(h, "prob", &pr, sizeof pr);
            }
        }
        if (other_back && od_ok && (!s_have_back || memcmp(s_back.sha8, od.app_elf_sha256, 8))) {
            /* Gone back with no record of its trial: it stopped before it
             * could write one, and the reset that stopped it says why, as
             * for a trial -- the power, or the EN pin (the console's port
             * opened), may come again; a crash or a hang never. One the
             * bootloader could not load at all is as real. */
            s_back.cls = !stale && (why == ESP_RST_POWERON || why == ESP_RST_BROWNOUT) ? BTL_BACK_POWER
                                                                                      : BTL_BACK_EARLY;
            memcpy(s_back.sha8, od.app_elf_sha256, 8);
            ver16(s_back.ver, od.version);
            s_have_back = found = true;
            if (nv) rec_set(h, "back", &s_back, sizeof s_back);
        }
    }

    /* The crash guard, while this firmware is on probation: one NVS write a
     * start, 20 at most. */
    uint8_t   probation = 0;
    esp_err_t guard_err = ESP_OK;
    if (!s_trial && have_pr && !memcmp(pr.sha8, s_own, 8) && pr.starts < PROBATION) {
        if (s_rg.magic != RG_MAGIC || memcmp(s_rg.sha8, s_own, 8)) {
            memset(&s_rg, 0, sizeof s_rg);
            s_rg.magic = RG_MAGIC;
            memcpy(s_rg.sha8, s_own, 8);
        }
        if (crash_reset(why)) {
            s_rg.crashes++;
        } else {
            s_rg.crashes = 0;
            s_rg.tried   = 0;
        }
        probation = ++pr.starts;
        if (pr.starts >= PROBATION) nvs_erase_key(h, "prob");
        else                        rec_set(h, "prob", &pr, sizeof pr);
        s_rg_armed = true;
        if (s_rg.crashes >= GUARD_CRASHES && !s_rg.tried) guard_err = guard(h, me);
    }

    rwdt(s_trial);

    s_info.boot_ver = bver > 255 ? 255 : (uint8_t)bver;
    s_info.slot     = slot;
    s_info.state    = s_trial ? BTL_RUN_TRIAL : st == ESP_OTA_IMG_VALID ? BTL_RUN_VALID : BTL_RUN_OTHER;
#ifdef CONFIG_VFO_COMPANION_RELEASE
    s_info.flags = BTL_INFO_RELEASE;
#endif
    memcpy(s_info.app_sha, s_own, 8);
    /* What went back, while the slot not running still holds it, gone back. */
    const bool back_here = s_have_back && other_back && od_ok && !memcmp(od.app_elf_sha256, s_back.sha8, 8);
    if (back_here) {
        s_info.back = s_back.cls;
        memcpy(s_info.back_sha, s_back.sha8, 8);
        memcpy(s_info.back_ver, s_back.ver, sizeof s_info.back_ver);
    }
    if (nv) {
        nvs_commit(h);
        nvs_close(h);
    }
    story(me, run, bver, why, back_here, found, stale, probation, guard_err);
    ESP_LOGI(TAG, "%s", s_story);
}

bool upd_can_take(void) { return s_can; }
bool upd_active(void) { return s_active; }
void upd_headset_came(void) { s_headset_came = true; }

/* ---- the transfer ------------------------------------------------------------ */

/* This very image went back here before, for a real failure: the slot not
 * running still holds it, otadata says it went back -- or still names it,
 * which the bootloader could not load (unloadable()) -- and NVS says why:
 * the same test as INFO's back (upd_boot), so the two always agree. Before
 * esp_ota_begin(), which erases that otadata entry. */
static bool went_back_for_real(const esp_partition_t *part, const uint8_t *app_sha)
{
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_app_desc_t       d;
    return esp_ota_get_state_partition(part, &st) == ESP_OK &&
           (st == ESP_OTA_IMG_ABORTED || st == ESP_OTA_IMG_INVALID || unloadable(part, st)) &&
           esp_ota_get_partition_description(part, &d) == ESP_OK && !memcmp(d.app_elf_sha256, app_sha, 8) &&
           s_have_back && !memcmp(s_back.sha8, app_sha, 8) && real(s_back.cls);
}

/* The transfer's end, said: its STATUS, and a line for the log. */
static btl_upd_status_t ended(const char *ver, uint8_t state, uint8_t why, int32_t err)
{
    const btl_upd_status_t s = { .state = state, .why = why, .next = s_next, .err = err };
    link_send(BTL_UPD_STATUS, &s, sizeof s);
    const char *what = state == BTL_UPD_REFUSED ? "refused" : state == BTL_UPD_FAILED ? "failed" : "stopped";
    if (why == BTL_UPD_WHY_KNOB && s_stop_knob)
        link_log("update: %s %s: %s (%s)", ver, what, why_str(why), knob_why_str(s_stop_knob));
    else if (err)
        link_log("update: %s %s: %s (%s)", ver, what, why_str(why), esp_err_to_name(err));
    else
        link_log("update: %s %s: %s", ver, what, why_str(why));
    return s;
}

static void upd_task(void *arg)
{
    (void)arg;
    btl_upd_begin_t b;
    portENTER_CRITICAL(&s_lk);
    b = s_b;
    portEXIT_CRITICAL(&s_lk);
    item_t *const          it = s_work;
    const esp_app_desc_t  *me = esp_app_get_description();
    char ver[17];
    memcpy(ver, b.version, 16);
    ver[16] = 0;
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    esp_ota_handle_t h   = 0;
    btl_upd_status_t end = { 0 };
    esp_err_t        e;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || !b.size || b.size > part->size || b.size % 4096) {
        end = ended(ver, BTL_UPD_REFUSED, BTL_UPD_WHY_SIZE, 0);
        goto out;
    }
    if (went_back_for_real(part, b.app_sha)) {
        end = ended(ver, BTL_UPD_REFUSED, BTL_UPD_WHY_BEFORE, 0);
        goto out;
    }
    /* otadata still has the slot not running as the one to start: the last
     * firmware written there would not load, and this one started instead
     * (unloadable()). esp_ota_set_boot_partition() writes the entry opposite
     * the active one -- now this firmware's own -- so another taken now
     * would leave nothing to go back to: its failed trial would start it
     * again. Not now, which the knob takes as such: the chip's next restart
     * marks that entry ABORTED, and the two agree again. */
    if (esp_ota_get_boot_partition() != esp_ota_get_running_partition()) {
        status(BTL_UPD_REFUSED, BTL_UPD_WHY_TRIAL, 0, 0);
        end = (btl_upd_status_t){ .state = BTL_UPD_REFUSED, .why = BTL_UPD_WHY_TRIAL };
        link_log("update: %s not now: otadata still names %s, which would not load -- after a restart", ver,
                 part->label);
        goto out;
    }
    /* Called off before it began (the knob's ABORT, or its restart): not
     * even otadata touched. */
    if (s_stop) {
        end = ended(ver, BTL_UPD_STOPPED, s_stop, 0);
        goto out;
    }
    /* Erases nothing of the slot up front: each esp_ota_write() erases the
     * sector it enters, so each wait on the flash is one sector's. */
    e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) {
        h   = 0;
        end = e == ESP_ERR_OTA_ROLLBACK_INVALID_STATE ? ended(ver, BTL_UPD_REFUSED, BTL_UPD_WHY_TRIAL, 0)
                                                      : ended(ver, BTL_UPD_FAILED, BTL_UPD_WHY_FLASH, e);
        goto out;
    }
    /* The slot not running is being written: what went back from it is gone
     * -- its record too, so that the next firmware there, the same image
     * again or not, is judged afresh by how it goes. (Left, a harmless
     * record would hide a real failure of the same image the next time.) */
    portENTER_CRITICAL(&s_lk);
    s_info.back = BTL_BACK_NONE;
    memset(s_info.back_sha, 0, sizeof s_info.back_sha);
    memset(s_info.back_ver, 0, sizeof s_info.back_ver);
    s_going = true;
    portEXIT_CRITICAL(&s_lk);
    if (s_have_back) {
        nvs_handle_t nh;
        if (nvs_open(NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
            nvs_erase_key(nh, "back");
            nvs_commit(nh);
            nvs_close(nh);
        }
        s_have_back = false;
    }
    mbedtls_sha256_starts(&c, 0);
    status(BTL_UPD_READY, BTL_UPD_WHY_NONE, 0, 0);
    link_log("update: %s, %lu bytes, into %s", ver, (unsigned long)b.size, part->label);

    const int64_t t0    = esp_timer_get_time();
    int64_t       t_ack = 0;
    int           quarter = 1;
    for (;;) {
        if (xQueueReceive(s_q, it, pdMS_TO_TICKS(QUIET_MS)) != pdTRUE) {
            end = ended(ver, BTL_UPD_STOPPED, BTL_UPD_WHY_QUIET, 0);
            break;
        }
        const uint8_t stop = s_stop;
        if (stop) {
            end = ended(ver, BTL_UPD_STOPPED, stop, 0);
            break;
        }
        if (it->type == BTL_UPD_DATA) {
            const uint32_t nx = s_next;
            if (it->off != nx) {
                /* A frame before it was lost on the wire: where to go on from. */
                const int64_t now = esp_timer_get_time();
                if (now - t_ack >= 50000) {
                    t_ack = now;
                    status(BTL_UPD_ACK, BTL_UPD_WHY_NONE, nx, 0);
                }
                continue;
            }
            if (nx + it->n > b.size) {
                end = ended(ver, BTL_UPD_FAILED, BTL_UPD_WHY_SIZE, 0);
                break;
            }
            /* A headset's or a speaker's link starting, a scan, audio: it goes first. */
            if (!hfp_idle()) {
                end = ended(ver, BTL_UPD_STOPPED, BTL_UPD_WHY_HEADSET, 0);
                break;
            }
            e = esp_ota_write(h, it->d, it->n);
            if (e != ESP_OK) {
                end = ended(ver, BTL_UPD_FAILED, e == ESP_ERR_OTA_VALIDATE_FAILED ? BTL_UPD_WHY_IMAGE : BTL_UPD_WHY_FLASH, e);
                break;
            }
            mbedtls_sha256_update(&c, it->d, it->n);
            s_next = nx + it->n;
            status(BTL_UPD_ACK, BTL_UPD_WHY_NONE, s_next, 0);
            for (; quarter < 4 && (uint64_t)s_next * 4 >= (uint64_t)b.size * (uint64_t)quarter; quarter++)
                link_log("update: %d%% in %.1f s", quarter * 25, (double)(esp_timer_get_time() - t0) / 1e6);
            continue;
        }
        if (it->type != BTL_UPD_END) continue;         /* a wake: s_stop was looked at */

        if (s_next != b.size || it->off != b.size) {
            end = ended(ver, BTL_UPD_FAILED, BTL_UPD_WHY_SIZE, 0);
            break;
        }
        uint8_t sha[32];
        mbedtls_sha256_finish(&c, sha);
        if (memcmp(sha, b.sha256, sizeof sha)) {
            end = ended(ver, BTL_UPD_FAILED, BTL_UPD_WHY_SHA, 0);
            break;
        }
        /* The image whole: its form, its own hash, the chip and revision it
         * is for, and its signature against the firmware running. */
        e = esp_ota_end(h);
        h = 0;                          /* freed, whatever it said */
        if (e != ESP_OK) {
            end = ended(ver, BTL_UPD_FAILED, BTL_UPD_WHY_IMAGE, e);
            break;
        }
        esp_app_desc_t d;
        if (esp_ota_get_partition_description(part, &d) != ESP_OK ||
            strncmp(d.project_name, me->project_name, sizeof d.project_name)) {
            end = ended(ver, BTL_UPD_FAILED, BTL_UPD_WHY_PROJECT, 0);
            break;
        }
        /* The last moment at which nothing has changed yet: the knob's word
         * -- an over or a call begun while the image was checked, a second
         * or two -- and a headset or a speaker, both looked at again. */
        const uint8_t late = s_stop;
        if (late || !hfp_idle()) {
            end = ended(ver, BTL_UPD_STOPPED, late ? late : BTL_UPD_WHY_HEADSET, 0);
            break;
        }
        e = esp_ota_set_boot_partition(part);   /* checks it again; writes otadata's next entry */
        if (e != ESP_OK) {
            end = ended(ver, BTL_UPD_FAILED, BTL_UPD_WHY_FLASH, e);
            break;
        }
        /* DONE, latched first: an END said again while this one restarts
         * gets it again. */
        end = (btl_upd_status_t){ .state = BTL_UPD_DONE, .why = BTL_UPD_WHY_NONE, .next = b.size, .err = 0 };
        portENTER_CRITICAL(&s_lk);
        s_latch.set  = true;
        s_latch.size = b.size;
        s_latch.st   = end;
        portEXIT_CRITICAL(&s_lk);
        link_send(BTL_UPD_STATUS, &end, sizeof end);
        char a[17], z[17];
        link_log("update: taken by %s [%s]: %.32s [%s] checked; restarting; stack %u bytes never used",
                 me->version, hex8(a, s_own), d.version, hex8(z, d.app_elf_sha256),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        link_flush();
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
out:
    if (h) esp_ota_abort(h);
    mbedtls_sha256_free(&c);
    hfp_hold(false);
    portENTER_CRITICAL(&s_lk);
    s_latch.set  = true;
    s_latch.size = b.size;
    s_latch.st   = end;
    s_going      = false;
    s_active     = false;
    portEXIT_CRITICAL(&s_lk);
    vTaskDelete(NULL);
}

/* ---- the knob's frames, on link_rx ------------------------------------------- */

static void refuse(uint8_t why, const btl_upd_begin_t *b)
{
    status(BTL_UPD_REFUSED, why, 0, 0);
    link_log("update: %.16s refused: %s", b->version, why_str(why));
}

/* DATA, END or ABORT with no transfer open: said, at most twice a second. */
static void no_session(void)
{
    const int64_t now = esp_timer_get_time();
    if (now - s_no_session_us < 500000) return;
    s_no_session_us = now;
    status(BTL_UPD_STOPPED, BTL_UPD_WHY_NO_SESSION, 0, 0);
}

/* The task's attention: a DATA, an END -- or, empty, a wake for s_stop. A
 * full queue drops it; the knob sends it again. */
static void to_task(uint8_t type, uint32_t off, const uint8_t *d, uint16_t n)
{
    if (!s_q) return;
    s_in.type = type;
    s_in.off  = off;
    s_in.n    = n;
    if (n) memcpy(s_in.d, d, n);
    xQueueSend(s_q, &s_in, 0);
}

static void begin(const uint8_t *p, uint16_t n)
{
    if (n < sizeof(btl_upd_begin_t)) return;
    btl_upd_begin_t b;
    memcpy(&b, p, sizeof b);
    portENTER_CRITICAL(&s_lk);
    const bool active = s_active;
    const bool same   = active && !memcmp(s_b.sha256, b.sha256, sizeof b.sha256);
    const bool going  = s_going;
    portEXIT_CRITICAL(&s_lk);
    if (active) {
        /* The knob lost our answer -- or sends another image. */
        if (!same) {
            refuse(BTL_UPD_WHY_BUSY, &b);
        } else if (going) {
            const uint32_t nx = s_next;
            status(nx ? BTL_UPD_ACK : BTL_UPD_READY, BTL_UPD_WHY_NONE, nx, 0);
        }
        return;                         /* still opening: its READY or REFUSED follows */
    }
    if (!s_can) {
        refuse(BTL_UPD_WHY_BOOTLOADER, &b);
        return;
    }
    if (s_trial) {
        refuse(BTL_UPD_WHY_TRIAL, &b);
        return;
    }
    if (!hfp_try_hold()) {
        refuse(BTL_UPD_WHY_HEADSET, &b);
        return;
    }
    if (!s_q) s_q = xQueueCreate(QUEUE_LEN, sizeof(item_t));
    if (!s_work) s_work = malloc(sizeof *s_work);
    /* What a transfer before left in the queue: none of it is this one's.
     * Here, before the task is: an ABORT that comes before the task has run
     * leaves its wake in the queue from now on. (The task before took
     * nothing more from it once it said it was done.) */
    if (s_q) xQueueReset(s_q);
    s_next      = 0;
    s_stop      = 0;
    s_stop_knob = 0;
    portENTER_CRITICAL(&s_lk);
    s_b         = b;
    s_going     = false;
    s_latch.set = false;
    s_active    = true;
    portEXIT_CRITICAL(&s_lk);
    /* 8 kB: the RSA check at END is the deepest point (the main task's
     * reason too). Below the link and the audio's pump, on their core. */
    if (!s_q || !s_work || xTaskCreatePinnedToCore(upd_task, "upd", 8192, NULL, 5, NULL, 1) != pdPASS) {
        portENTER_CRITICAL(&s_lk);
        s_active = false;
        portEXIT_CRITICAL(&s_lk);
        hfp_hold(false);
        refuse(BTL_UPD_WHY_MEMORY, &b);
    }
}

void upd_on_frame(uint8_t type, const uint8_t *p, uint16_t n)
{
    switch (type) {
    case BTL_UPD_BEGIN:
        begin(p, n);
        break;
    case BTL_UPD_DATA: {
        if (n <= 4 || n > 4 + BTL_UPD_CHUNK) break;
        uint32_t off;
        memcpy(&off, p, 4);
        if (s_active) to_task(BTL_UPD_DATA, off, p + 4, (uint16_t)(n - 4));
        else          no_session();
        break;
    }
    case BTL_UPD_END: {
        if (n < 4) break;
        uint32_t size;
        memcpy(&size, p, 4);
        portENTER_CRITICAL(&s_lk);
        const bool             again  = s_latch.set && s_latch.size == size;
        const btl_upd_status_t st     = s_latch.st;
        const bool             active = s_active;
        portEXIT_CRITICAL(&s_lk);
        if (again)       link_send(BTL_UPD_STATUS, &st, sizeof st);  /* the answer we gave, lost */
        else if (active) to_task(BTL_UPD_END, size, NULL, 0);
        else             no_session();
        break;
    }
    case BTL_UPD_ABORT:
        if (!s_active) {
            no_session();
            break;
        }
        s_stop_knob = n ? p[0] : 0;
        s_stop      = BTL_UPD_WHY_KNOB;
        to_task(BTL_UPD_ABORT, 0, NULL, 0);
        break;
    case BTL_UPD_KEEP:
        if (s_trial) s_keep_asked = true;
        send_info();
        break;
    case BTL_UPD_ASK:
        send_info();
        break;
    default:
        break;
    }
}

void upd_knob_hello(bool ask)
{
    /* Only over the link: the console had it at the start. */
    if (!s_told || ask) {
        s_told = true;
        link_send(BTL_EVT_LOG, s_story, (uint16_t)strlen(s_story));
    }
    /* Restarted mid-transfer, the knob has no transfer any more. */
    if (ask && s_active) {
        s_stop = BTL_UPD_WHY_RESTARTED;
        to_task(BTL_UPD_ABORT, 0, NULL, 0);
    }
    send_info();
}

/* ---- the trial, from the main loop --------------------------------------------- */

static void keep(int64_t now)
{
    const esp_app_desc_t *me = esp_app_get_description();
    /* Rewrites its otadata entry: no image check. */
    const esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    if (e != ESP_OK) {
        s_keep_retry_us = now + 5000000;
        link_log("keeping %s failed: %s -- again in 5 s", me->version, esp_err_to_name(e));
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        prob_rec_t pr = { .starts = 0 };
        memcpy(pr.sha8, s_own, 8);
        /* Probation first, then the trial record goes: a power cut between
         * the two leaves both, which the next start reads as kept and its
         * probation going on (upd_boot). The other way round it would leave
         * neither, and no crash guard. */
        rec_set(h, "prob", &pr, sizeof pr);
        nvs_erase_key(h, "trial");
        nvs_commit(h);
        nvs_close(h);
    }
    rwdt(false);
    s_trial = false;
    portENTER_CRITICAL(&s_lk);
    s_info.state = BTL_RUN_VALID;
    portEXIT_CRITICAL(&s_lk);
    send_info();
    link_log("kept %s, %lu s after its start; main task stack %u bytes never used", me->version,
             (unsigned long)(now / 1000000), (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

void upd_tick(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_rg_armed && now >= GUARD_CLEAR_US) {
        s_rg_armed   = false;
        s_rg.crashes = 0;               /* a crash from here on starts a run of its own */
    }
    if (!s_trial) return;
    rwdt(true);
    if (s_keep_asked && (now >= KEEP_AFTER_US || s_headset_came)) {
        if (now >= s_keep_retry_us) keep(now);
    } else if (now >= TRIAL_US && !s_keep_asked && !hfp_audio_open()) {
        /* A knob with no sender, or a link this firmware broke. The 2
         * minutes wait while a device's audio is open: a late KEEP never
         * drops a call. */
        link_log("not kept: no knob said so in 2 minutes -- going back");
        link_flush();
        esp_restart();
    }
}
