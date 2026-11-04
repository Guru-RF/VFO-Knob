/* The clock faces (components/ui/clockface.c) on the PC: each face at a few
 * moments, as PNGs, and the promise the face makes -- a tick that sends only
 * the hands' box leaves the picture exactly as drawing it whole would.
 *   make clockfaces-check OUT=dir */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clockface.h"
#include "lvgl.h"
#include "wallclock.h"

#define S 360
static int n_run, n_fail;
static const char *where = "";
#define CHECK(c) do { n_run++; if (!(c)) { n_fail++; printf("FAIL [%s] %s:%d: %s\n", where, __FILE__, __LINE__, #c); } } while (0)

/* The time the faces read: seconds since the epoch and the ms into it. */
static time_t   t_now = 0;
static uint16_t t_ms;
static bool     t_have = true, t_fresh = true;
bool wallclock_now(struct tm *local, struct tm *utc, uint16_t *ms)
{
    if (!t_have) return false;
    const time_t l = t_now + 3600;          /* Brussels in winter */
    if (local) gmtime_r(&l, local);
    if (utc) gmtime_r(&t_now, utc);
    if (ms) *ms = t_ms;
    return true;
}
uint32_t wallclock_age_s(void) { return t_fresh ? 60 : 3 * 86400; }

static uint32_t tick_ms;
static uint32_t tick_cb(void) { return tick_ms; }
static uint8_t  dbuf[S * 12 * 2];
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) { (void)a; (void)px; lv_display_flush_ready(d); }

static uint16_t pic[S * S], whole[S * S];

static void save(const char *dir, const char *name, const uint16_t *px)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", S, S);
    for (int i = 0; i < S * S; i++) {
        const int x = i % S - S / 2, y = i / S - S / 2;
        const uint16_t c = x * x + y * y <= 180 * 180 ? px[i] : 0;
        const uint8_t o[3] = { (uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3) };
        fwrite(o, 1, 3, f);
    }
    fclose(f);
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "command -v magick >/dev/null && magick %s/%s.ppm %s/%s.png && rm %s/%s.ppm",
             dir, name, dir, name, dir, name);
    if (system(cmd)) {}
}

static const char *const NAME[CF_N] = { "shack", "diver", "classic", "digital" };

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *d = lv_display_create(S, S);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, dbuf, NULL, sizeof dbuf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);

    /* LVGL's pool before any face: what the drawing takes above it is
     * checked at the end. */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    const size_t used0 = mon.total_size - mon.free_size;

    /* 2026-11-04 09:08:37 UTC, 10:08 in Brussels: the hands apart. */
    const time_t base = 1793783317;
    for (int f = 0; f < CF_N; f++) {
        const cf_opts_t o = { .face = (uint8_t)f, .seconds = true, .h24 = true };
        lv_area_t a;
        char name[64];

        where = NAME[f];
        t_now = base; t_ms = 0; t_have = t_fresh = true;
        clockface_reset();
        CHECK(clockface_draw(pic, &o, &a));
        CHECK(a.x1 == 0 && a.y1 == 0 && a.x2 == S - 1 && a.y2 == S - 1);
        snprintf(name, sizeof name, "clock-%s", NAME[f]);
        save(dir, name, pic);

        /* An hour of ticks -- each second, and on the Diver each sixth --
         * drawn box by box, against the face drawn whole at the end. */
        where = "a tick sends its box, and the picture is the whole one";
        int boxes = 0;
        long px_sent = 0;
        const int steps = f == CF_DIVER ? 6 * 90 : 3700;
        for (int i = 1; i <= steps; i++) {
            if (f == CF_DIVER) { t_now = base + i / 6; t_ms = (uint16_t)(i % 6 * 167); }
            else               { t_now = base + i;     t_ms = 0; }
            if (clockface_draw(pic, &o, &a)) {
                boxes++;
                px_sent += (long)(a.x2 - a.x1 + 1) * (a.y2 - a.y1 + 1);
                CHECK(a.x1 >= 0 && a.y1 >= 0 && a.x2 < S && a.y2 < S);
            }
        }
        clockface_reset();
        CHECK(clockface_draw(whole, &o, &a));
        int diff = 0;
        for (int i = 0; i < S * S; i++) diff += pic[i] != whole[i];
        CHECK(diff == 0);
        if (diff) printf("  %s: %d pixels differ\n", NAME[f], diff);
        printf("%s: %d ticks drawn, %ld px a tick on average\n", NAME[f], boxes, boxes ? px_sent / boxes : 0);

        where = "stale: a day without an answer, the hands grey";
        t_fresh = false;
        clockface_reset();
        CHECK(clockface_draw(pic, &o, &a));
        snprintf(name, sizeof name, "clock-%s-stale", NAME[f]);
        save(dir, name, pic);
        where = "no time yet: the dial alone";
        t_have = false;
        CHECK(clockface_draw(pic, &o, &a));
        snprintf(name, sizeof name, "clock-%s-notime", NAME[f]);
        save(dir, name, pic);
    }
    /* The drawing's draws wait in LVGL's pool until their layer is finished:
     * the shack clock's dial, some 150 at once, ran the knob's 64 kB out
     * (2026-11-04). In batches (CF_BATCH) the dial's peak stays small. */
    where = "LVGL's pool: the faces' peak above what was in use before";
    lv_mem_monitor(&mon);
    printf("LVGL pool: %zu bytes at most above the %zu in use before\n", mon.max_used - used0, used0);
    CHECK(mon.max_used - used0 < 16 * 1024);
    printf("%s: %d checks, %d failed\n", argv[0], n_run, n_fail);
    return n_fail ? 1 : 0;
}
