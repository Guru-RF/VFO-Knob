/* SVXConnect's talkgroups (components/svx_client/svx/tg/tgmanager.c): a
 * switchable talkgroup is on the dial and heard only while selected, a monitored
 * one is heard and followed (GitHub #21); what a + does, and on which list; and
 * the quiet spell's drop to monitoring, only with something to monitor. */
#include "tiny.h"
#include "tg/tgmanager.h"
#include "common/util.h"
#include "esp_timer.h"

#include <string.h>

/* now_ms() stands still: the quiet spell gets its time from tgm_tick(). */
int64_t esp_timer_get_time(void) { return 1000000; }

static uint32_t s_mon[SVX_MAX_TG * 2];
static size_t   s_n_mon;

static void cb_select(void *u, uint32_t tg, int gate) { (void)u; (void)tg; (void)gate; }
static void cb_monitor(void *u, const uint32_t *ids, size_t n)
{
    (void)u;
    s_n_mon = n;
    memcpy(s_mon, ids, n * sizeof *ids);
}

static bool monitored(uint32_t tg)
{
    for (size_t i = 0; i < s_n_mon; i++)
        if (s_mon[i] == tg) return true;
    return false;
}

static svx_config s_cfg;
static tg_manager s_m;

static void start(const char *sw, const char *mon)
{
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.n_switchable = tglist_parse(sw, s_cfg.switchable, SVX_MAX_TG);
    s_cfg.n_monitored  = tglist_parse(mon, s_cfg.monitored, SVX_MAX_TG);
    s_cfg.linger_seconds = 30;
    s_cfg.idle_seconds   = 60;
    const tgm_callbacks cb = { .select_tg = cb_select, .set_monitor = cb_monitor };
    s_n_mon = 0;
    tgm_init(&s_m, &s_cfg, &cb);
    tgm_after_connect(&s_m);
}

static void test_monitor_set(void)
{
    CASE("the reflector is told the monitored list, never the switchable one");
    start("4, 6, 10, 23, 8, 11", "2000, 91");
    CHECK_EQ(tgm_selected(&s_m), 4);
    CHECK_EQ(s_n_mon, 2);
    CHECK(monitored(2000) && monitored(91));
    CHECK(!monitored(6) && !monitored(8) && !monitored(11));

    CASE("no monitored list: nothing but the talkgroup selected is heard");
    start("4, 6, 10, 23", "");
    CHECK_EQ(s_n_mon, 0);

    CASE("in both lists: monitored, until it is the one selected");
    start("4, 2000", "2000");
    CHECK(monitored(2000));
    tgm_select(&s_m, 2000);
    CHECK_EQ(s_n_mon, 0);
    tgm_select(&s_m, 4);
    CHECK(monitored(2000));

    CASE("the lock still empties it");
    start("4", "2000");
    tgm_set_lock(&s_m, 1);
    CHECK_EQ(s_n_mon, 0);
    tgm_set_lock(&s_m, 0);
    CHECK(monitored(2000));
}

static void test_what_moves(void)
{
    CASE("traffic on a switchable talkgroup moves nothing, should it be reported");
    start("4, 6, 8", "2000");
    tgm_on_talker_start(&s_m, 8, "ON6ABC");
    CHECK_EQ(tgm_selected(&s_m), 4);
    tgm_on_talker_stop(&s_m, 8, "ON6ABC");

    CASE("traffic on a monitored one takes a quiet talkgroup");
    tgm_on_talker_start(&s_m, 2000, "ON6ABC");
    CHECK_EQ(tgm_selected(&s_m), 2000);

    CASE("the dial still reaches every switchable one, and that is heard");
    start("4, 6, 8", "2000");
    tgm_next(&s_m);
    CHECK_EQ(tgm_selected(&s_m), 6);
    tgm_next(&s_m);
    CHECK_EQ(tgm_selected(&s_m), 8);
}

static void test_priority(void)
{
    CASE("a + counts on the monitored list");
    start("4", "2000, 91+");
    tgm_on_talker_start(&s_m, 4, "ON6AAA");       /* busy where it is */
    tgm_on_talker_start(&s_m, 2000, "ON6BBB");
    CHECK_EQ(tgm_selected(&s_m), 4);              /* priority 0 waits */
    tgm_on_talker_start(&s_m, 91, "ON6CCC");
    CHECK_EQ(tgm_selected(&s_m), 91);             /* priority 1 takes it */

    CASE("on the switchable list it means nothing: no shield, no pull");
    start("4+", "91+");
    CHECK_EQ(tgm_priority(&s_m, 4), 0);
    tgm_on_talker_start(&s_m, 4, "ON6AAA");
    tgm_on_talker_start(&s_m, 91, "ON6CCC");
    CHECK_EQ(tgm_selected(&s_m), 91);

    CASE("in both lists the monitored one's counts");
    start("2000", "2000++");
    CHECK_EQ(tgm_priority(&s_m, 2000), 2);
}

static void test_quiet_spell(void)
{
    const uint64_t later = now_ms() + 61000;

    CASE("with a monitored list, a quiet spell drops to monitoring");
    start("4, 6", "2000");
    tgm_tick(&s_m, later);
    CHECK_EQ(tgm_selected(&s_m), 0);
    CHECK(monitored(2000));

    CASE("without one it stays: nothing to monitor, it would be deaf");
    start("4, 6", "");
    tgm_tick(&s_m, later);
    CHECK_EQ(tgm_selected(&s_m), 4);

    CASE("idle 0 never drops");
    start("4, 6", "2000");
    s_cfg.idle_seconds = 0;
    tgm_tick(&s_m, later);
    CHECK_EQ(tgm_selected(&s_m), 4);
}

T_MAIN(
    test_monitor_set();
    test_what_moves();
    test_priority();
    test_quiet_spell();
)
