/* The audio's way into the ring, block by block. See websdr_link.h.
 *
 * kiwi_sess.c's flow(), as it has ridden out TerraBooster's stalls and a
 * phone hotspot's bunching for hours (2026-10-03..06), with the SND frame
 * made a block of the caller's: its number, its length, how long it lasts.
 * The Kiwi's session keeps its own copy until it moves onto this link. */
#include "websdr_link.h"

#include <string.h>

#include "esp_log.h"

#define JUMP_PCT   80                   /* a backlog that would fill the ring past this: left out */
#define AVG_K      0.05f                /* the ring's level, smoothed block by block */
#define TRIM_GAIN  0.002f               /* the trim, per the level's distance from its target */
#define LIVE_CREEP 1000.0               /* the live point's reference, crept on 1 us a ms */
#define REFILL_US  (1000000LL)          /* after a stall's jump, the ring's refill: no other in it */
#define MORE_MS    50                   /* a block near the live point: the next, looked for this long */
#define USUAL_EBB  16.0                 /* the usual lateness ebbs a 16th of a block a block */
#define AGAIN_US   (120 * 1000000LL)    /* a break this soon after the last grows the target */
#define CALM_US    (180 * 1000000LL)    /* ...this long without one, it eases back */
#define EASE_US    (30 * 1000000LL)     /* ...a step at a time this far apart */

static unsigned long ms_of(size_t n) { return (unsigned long)((uint64_t)n * 1000 / WL_OUT_HZ); }

static const char *tag_of(const wl_flow_cfg_t *l) { return l->tag ? l->tag : "wl"; }

void wl_flow_init(wl_flow_t *w, const wl_flow_cfg_t *l)
{
    memset(w, 0, sizeof *w);
    w->tgt = w->roof = l->target;
    w->ur = l->underruns ? l->underruns(l->ctx) : 0;
    if (l->preroll) l->preroll(l->ctx, l->target);
}

/* Where the ring is kept now: the target, or the level an eased one's ring
 * is still coming down from. */
static size_t flow_top(const wl_flow_t *w) { return w->roof > w->tgt ? w->roof : w->tgt; }

/* A break -- the ring run dry, or a stall's backlog about to be cut short
 * with sound still in the ring: counted; one within AGAIN_US of the last
 * grows the target to hold a stall as late as this block came, a quarter
 * block to spare, half a block more at least. True when it grew. */
static bool flow_break(wl_flow_t *w, const wl_flow_cfg_t *l, double late_n, size_t len, int64_t now)
{
    const bool again = w->t_brk && now - w->t_brk < AGAIN_US;
    w->t_brk = now;
    w->rep.breaks++;
    if (!again || w->tgt >= l->target_max) return false;
    size_t want = late_n > 0 ? (size_t)late_n + len * 3 / 4 : 0;
    if (want < w->tgt + len / 2) want = w->tgt + len / 2;
    if (want > l->target_max) want = l->target_max;
    w->tgt = want;
    if (l->preroll) l->preroll(l->ctx, want);
    ESP_LOGW(tag_of(l), "the stream breaks up again and again: the ring kept at %lu ms from now", ms_of(want));
    return true;
}

/* Calm a while: the target back down half a block every EASE_US. */
static void flow_ease(wl_flow_t *w, const wl_flow_cfg_t *l, size_t len, int64_t now)
{
    if (w->tgt <= l->target || now - w->t_brk < CALM_US || now - w->t_ease < EASE_US) return;
    w->tgt = w->tgt > l->target + len / 2 ? w->tgt - len / 2 : l->target;
    w->t_ease = now;
    if (l->preroll) l->preroll(l->ctx, w->tgt);
    ESP_LOGI(tag_of(l), "calm a while: the ring kept at %lu ms from now", ms_of(w->tgt));
}

static void jump_end(wl_flow_t *w, const wl_flow_cfg_t *l, int64_t now)
{
    const unsigned long ms = ms_of(w->left);
    if (w->jump == WL_JUMP_STALE)
        ESP_LOGW(tag_of(l), "a stall's backlog: %lu ms left out in one jump%s, on from the live point", ms,
                 w->silent ? ", in its silence" : "");
    else
        ESP_LOGW(tag_of(l), "the ring full: %lu ms left out in one jump, back to its target", ms);
    w->refill = w->jump == WL_JUMP_STALE;
    w->jump = WL_JUMP_NONE;
    w->t_jump = now;
    w->rep.jumps++;
    w->rep.left_ms += (uint32_t)ms;
    w->fed = 0;
}

bool wl_flow(wl_flow_t *w, const wl_flow_cfg_t *l, kiwi_dsp_t *d, uint32_t seq, size_t len, double period_us,
             int64_t now, bool play, bool (*more)(void *mctx), void *mctx)
{
    /* How it came: the longest wait for a block, and its number. */
    const int64_t p = (int64_t)period_us;
    int32_t ahead = 0;
    if (w->any) {
        const int64_t gap = now - w->t_last;
        if (gap / 1000 > w->rep.gap_ms) w->rep.gap_ms = (uint32_t)(gap / 1000);
        ahead = (int32_t)(seq - w->seq);
        if (ahead > 1)
            w->rep.lost += (uint32_t)(ahead - 1);
        else if (ahead == 1 && p > 0 && gap > 2 * p)
            w->rep.held += (uint32_t)((gap + p / 2) / p - 1);
    }
    /* Its lateness against where it stands in the stream. */
    double e = 0, late_us = 0;
    if (p > 0) {
        if (ahead < 1 || period_us != w->period_us) {
            w->pos_us = 0;
            w->ref_us = (double)now;
            w->t_ref = now;
            w->usual_us = 0;
        } else {
            w->pos_us += ahead * period_us;
        }
        w->period_us = period_us;
        e = (double)now - w->pos_us;
        double ref = w->ref_us + (double)(now - w->t_ref) / LIVE_CREEP;
        if (e < ref) {
            w->ref_us = ref = e;
            w->t_ref = now;
        }
        late_us = e - ref;
    }
    w->any = true;
    w->seq = seq;
    w->t_last = now;
    w->rep.blocks++;
    if (!play || !l->queued || !len || !l->room) return play;

    const size_t q = l->queued(l->ctx);
    const bool dry = q < len / 8;
    if (dry && w->roof > w->tgt) w->roof = w->tgt;
    const double over_us = late_us - w->usual_us;
    const double late_n = late_us * WL_OUT_HZ / 1e6;
    bool stale = over_us > period_us / 2 && (double)q + late_n > (double)(flow_top(w) + len / 4);
    bool backlog = false;
    if (w->refill && (q >= w->tgt || now - w->t_jump > REFILL_US)) w->refill = false;
    const uint32_t ur = l->underruns ? l->underruns(l->ctx) : w->ur;
    const bool ran_dry = ur != w->ur;
    w->ur = ur;
    const bool broke = !w->jump && (ran_dry || (stale && !dry && !w->refill));
    if (broke && flow_break(w, l, late_n, len, now))
        stale = over_us > period_us / 2 && (double)q + late_n > (double)(flow_top(w) + len / 4);
    flow_ease(w, l, len, now);
    if (!w->jump && stale && !w->refill) {
        w->jump = WL_JUMP_STALE;
        w->left = w->jump_n = 0;
        w->t_jump = now;
        w->silent = dry;
        w->first_us = late_us;
    }
    if (w->jump == WL_JUMP_STALE) {
        const bool fast = !w->jump_n || (double)w->jump_n * period_us >= 2.0 * (double)(now - w->t_jump);
        const bool aging = !stale && over_us > period_us * 3 / 4 && q + len < w->tgt &&
                           !(more && more(mctx));
        if ((stale || aging) && fast) {
            w->left += (uint32_t)len;
            w->jump_n++;
            return false;
        }
        jump_end(w, l, now);
        backlog = true;
        const double taught = w->first_us < 2.5 * period_us ? w->first_us : 2.5 * period_us;
        if (taught > w->usual_us) w->usual_us = taught;
        if (stale) {
            w->ref_us = e;
            w->t_ref = now;
        }
    }
    const size_t lvl = w->fed >= 2 ? (size_t)w->avg : w->tgt;
    if (w->roof > lvl) w->roof = lvl;
    if (w->roof < w->tgt) w->roof = w->tgt;
    const size_t grown = w->roof - l->target;
    size_t hi = l->room * JUMP_PCT / 100 + grown;
    if (hi < w->tgt + len) hi = w->tgt + len;
    if (hi > l->room + grown) hi = l->room + grown;
    if (!w->jump && q + len > hi) {
        w->jump = WL_JUMP_FULL;
        w->left = 0;
    }
    if (w->jump == WL_JUMP_FULL) {
        if (q + len / 2 > w->tgt) {
            w->left += (uint32_t)len;
            return false;
        }
        jump_end(w, l, now);
    }
    if (!backlog) {
        w->usual_us -= period_us / USUAL_EBB;
        if (!stale && late_us > w->usual_us) w->usual_us = late_us;
        if (w->usual_us < 0) w->usual_us = 0;
    }
    if (dry) w->fed = 0;
    const float level = (float)(q + len / 2), target = (float)w->tgt;
    if (w->fed < 2) {
        w->avg = target;
    } else {
        w->avg += AVG_K * (level - w->avg);
        if (l->trim && d) {
            float t = TRIM_GAIN * (w->avg - target) / (float)l->target;
            if (w->roof > w->tgt && w->avg > target + (float)(len / 4)) t = 0.002f;
            if (t > 0.002f) t = 0.002f;
            if (t < -0.002f) t = -0.002f;
            kiwi_dsp_trim(d, 1.0f + t);
        }
    }
    w->fed++;
    return true;
}

void wl_flow_report(wl_flow_t *w, wl_report_t *out, float trim, int64_t now)
{
    w->rep.level_ms = (uint32_t)(w->avg * 1000 / WL_OUT_HZ);
    w->rep.target_ms = (uint32_t)ms_of(w->tgt);
    w->rep.trim = trim;
    if (out) *out = w->rep;
    memset(&w->rep, 0, sizeof w->rep);
    w->t_rep = now;
}
