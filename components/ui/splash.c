/* Boot splash: RF.Guru branding, in the palette of rfguru.app.
 *
 * It runs over the finished main screen rather than instead of it, so the dial
 * is already built and live the instant the splash fades out. It also covers
 * dead time that exists anyway -- the panel is up about 2.3 s into boot and the
 * radio link is not there until 7 s, so a few seconds of branding costs the
 * operator nothing.
 */
#include "splash.h"
#include "ui.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

/* Sampled from the logo and from rfguru.app. */
#define RFG_GOLD   lv_color_hex(RFG_GOLD_HEX)
#define RFG_GOLD_D lv_color_hex(0xD59417)
#define RFG_INK    lv_color_hex(0x0C0E13)
#define RFG_TEXT   lv_color_hex(0xECC34A)

/* The backdrop from rfguru.app: a near-black base, a gold glow falling away
 * from the top, and a 48 px grid of 1 px white lines at a few percent.
 *
 * The site's glow is two radial gradients. LVGL has those only with
 * LV_USE_DRAW_SW_COMPLEX_GRADIENTS, which is not compiled in here and is not
 * worth the draw cost for a backdrop, so it is a vertical gradient instead --
 * at this diameter, and at that opacity, the difference is not visible. The
 * top colour is rgba(229,168,35,0.12) composited onto the base by hand.
 *
 * The grid is lifted from the site's 2.5% to 4%: a 1.8" panel at arm's length
 * is not a desktop monitor, and at 2.5% it simply disappeared. */
#define RFG_GLOW   lv_color_hex(0x262015)
#define RFG_GRID   48
#define RFG_GRID_OPA 10

#define SPLASH_MS 2900


extern const lv_image_dsc_t rfguru_logo;

static lv_obj_t *s_splash;
static lv_obj_t *s_main;

static void a_opa(void *o, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
}

static void a_zoom(void *o, int32_t v)
{
    lv_image_set_scale((lv_obj_t *)o, v);
}

static void a_arc(void *o, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)o, v);
}

static void anim_to(void *target, lv_anim_exec_xcb_t cb, int32_t from,
                    int32_t to, uint32_t delay, uint32_t time, lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_duration(&a, time);
    if (path) lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

static void done_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    /* Hand over with a fade. The main screen is already built and updating
     * behind this, so there is nothing to wait for. */
    /* auto_del: the splash is never shown again, and it is not cheap to keep
     * -- a decoded 120x120 logo, sixteen grid rectangles, an arc and three
     * labels, all pinned in RAM for the life of the device. Leaving it
     * allocated cost enough internal heap that RSA-3072 signature
     * verification on the next OTA could not allocate its working memory and
     * rejected a perfectly good image. */
    /* Only if the splash is still what is on screen, though. auto_del deletes
     * whichever screen is active when the fade starts, and if the update
     * screen took over during the splash -- an accepted update installs
     * straight after a quick WiFi boot -- that would be the update screen,
     * freed under splash.c's feet. Then the splash just deletes itself. */
    if (lv_screen_active() == s_splash)
        lv_screen_load_anim(s_main, LV_SCR_LOAD_ANIM_FADE_ON, 320, 0, true);
    else
        lv_obj_delete(s_splash);
    s_splash = NULL;
}

void ui_splash_start(void)
{
    s_main   = lv_screen_active();
    s_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_splash, RFG_GLOW, 0);
    lv_obj_set_style_bg_grad_color(s_splash, RFG_INK, 0);
    lv_obj_set_style_bg_grad_dir(s_splash, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_splash, 0, 0);
    lv_obj_remove_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);

    /* The site's grid, drawn rather than tiled -- sixteen thin rectangles is
     * less machinery than an image, and they are created once. */
    for (int g = RFG_GRID; g < 360; g += RFG_GRID) {
        for (int axis = 0; axis < 2; axis++) {
            lv_obj_t *ln = lv_obj_create(s_splash);
            lv_obj_set_size(ln, axis ? 1 : 360, axis ? 360 : 1);
            lv_obj_set_pos(ln, axis ? g : 0, axis ? 0 : g);
            lv_obj_set_style_radius(ln, 0, 0);
            lv_obj_set_style_border_width(ln, 0, 0);
            lv_obj_set_style_bg_color(ln, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(ln, RFG_GRID_OPA, 0);
            lv_obj_remove_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    /* Sweep: a gold arc that draws itself once around, the same band the
     * S-meter uses so the splash and the dial feel like one instrument. */
    lv_obj_t *sweep = lv_arc_create(s_splash);
    lv_obj_set_size(sweep, 332, 332);
    lv_obj_center(sweep);
    lv_arc_set_rotation(sweep, 270);
    lv_arc_set_bg_angles(sweep, 0, 360);
    lv_arc_set_range(sweep, 0, 1000);
    lv_arc_set_value(sweep, 0);
    lv_obj_remove_style(sweep, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(sweep, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(sweep, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sweep, lv_color_hex(0x1A1D25), LV_PART_MAIN);
    lv_obj_set_style_arc_width(sweep, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sweep, RFG_GOLD_D, LV_PART_INDICATOR);

    lv_obj_t *logo = lv_image_create(s_splash);
    lv_image_set_src(logo, &rfguru_logo);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -44);
    lv_image_set_scale(logo, 200);
    lv_obj_set_style_opa(logo, LV_OPA_TRANSP, 0);

    lv_obj_t *brand = lv_label_create(s_splash);
    lv_label_set_text(brand, "RF.Guru");
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(brand, RFG_TEXT, 0);
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, 46);
    lv_obj_set_style_opa(brand, LV_OPA_TRANSP, 0);

    lv_obj_t *prod = lv_label_create(s_splash);
    lv_label_set_text(prod, "VFO Knob");
    lv_obj_set_style_text_font(prod, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(prod, lv_color_hex(0xC8D8E8), 0);
    lv_obj_align(prod, LV_ALIGN_CENTER, 0, 82);
    lv_obj_set_style_opa(prod, LV_OPA_TRANSP, 0);

    /* Nothing may be drawn over the mark or the names. */
    lv_obj_move_foreground(logo);
    lv_obj_move_foreground(brand);
    lv_obj_move_foreground(prod);

    lv_screen_load(s_splash);

    /* Logo settles, then the names, while the sweep completes around them. */
    anim_to(logo,  a_zoom, 200, 256,   60, 520, lv_anim_path_overshoot);
    anim_to(logo,  a_opa,    0, 255,   60, 420, NULL);
    anim_to(sweep, a_arc,    0, 1000, 180, 1500, lv_anim_path_ease_in_out);
    anim_to(brand, a_opa,    0, 255,  620, 420, NULL);
    anim_to(prod,  a_opa,    0, 255,  900, 420, NULL);

    lv_timer_create(done_cb, SPLASH_MS, NULL);
}


/* ---------------------------------------------------------------- update */

/* A firmware upload owns the device.
 *
 * RX audio is a continuous ~96 kB/s inbound stream sharing the socket and the
 * USB pipe with the image, and with both running the upload broke midway --
 * thousands of dropped audio frames and a truncated transfer. Quiescing is
 * therefore not just presentation: it is what makes the upload survive.
 *
 * Presentation matters too, though. A knob that looks perfectly normal while
 * its flash is being rewritten invites exactly the one thing that must not
 * happen, so this says so, in the branding, with a bar that is visibly moving.
 * Being a separate screen, it also puts PTT out of reach for the duration. */
static lv_obj_t *s_upd;
static lv_obj_t *s_upd_arc;
static lv_obj_t *s_upd_title;
static lv_obj_t *s_upd_pct;
static lv_obj_t *s_upd_msg;

void ui_updating_show(void)
{
    if (!lvgl_port_lock(200)) return;
    if (!s_upd) {
        s_upd = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_upd, RFG_GLOW, 0);
        lv_obj_set_style_bg_grad_color(s_upd, RFG_INK, 0);
        lv_obj_set_style_bg_grad_dir(s_upd, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(s_upd, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_upd, 0, 0);
        lv_obj_remove_flag(s_upd, LV_OBJ_FLAG_SCROLLABLE);

        s_upd_arc = lv_arc_create(s_upd);
        lv_obj_set_size(s_upd_arc, 300, 300);
        lv_obj_center(s_upd_arc);
        lv_arc_set_rotation(s_upd_arc, 270);
        lv_arc_set_bg_angles(s_upd_arc, 0, 360);
        lv_arc_set_range(s_upd_arc, 0, 100);
        lv_arc_set_value(s_upd_arc, 0);
        lv_obj_remove_style(s_upd_arc, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(s_upd_arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(s_upd_arc, 10, LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_upd_arc, lv_color_hex(0x1A1D25), LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_upd_arc, 10, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_upd_arc, RFG_GOLD, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(s_upd_arc, false, LV_PART_INDICATOR);

        s_upd_title = lv_label_create(s_upd);
        lv_obj_set_style_text_font(s_upd_title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_upd_title, RFG_TEXT, 0);

        s_upd_pct = lv_label_create(s_upd);
        lv_label_set_text(s_upd_pct, "0%");
        lv_obj_set_style_text_font(s_upd_pct, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(s_upd_pct, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(s_upd_pct, LV_ALIGN_CENTER, 0, -6);

        s_upd_msg = lv_label_create(s_upd);
        lv_label_set_text(s_upd_msg, "Do not unplug");
        lv_obj_set_style_text_font(s_upd_msg, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_upd_msg, lv_color_hex(0xFF9A3C), 0);
        lv_obj_align(s_upd_msg, LV_ALIGN_CENTER, 0, 58);
    }
    lv_label_set_text(s_upd_title, "UPDATING");
    lv_obj_align(s_upd_title, LV_ALIGN_CENTER, 0, -62);
    lv_arc_set_value(s_upd_arc, 0);
    lv_label_set_text(s_upd_pct, "0%");
    lv_obj_align(s_upd_pct, LV_ALIGN_CENTER, 0, -6);
    lv_label_set_text(s_upd_msg, "Do not unplug");
    lv_obj_set_style_text_color(s_upd_msg, lv_color_hex(0xFF9A3C), 0);
    lv_obj_align(s_upd_msg, LV_ALIGN_CENTER, 0, 58);
    lv_screen_load(s_upd);
    lvgl_port_unlock();
}

void ui_updating_reboot(void)
{
    ui_updating_show();
    if (!lvgl_port_lock(200)) return;
    lv_label_set_text(s_upd_title, "REBOOTING");
    lv_obj_align(s_upd_title, LV_ALIGN_CENTER, 0, -62);
    lv_label_set_text(s_upd_pct, LV_SYMBOL_REFRESH);
    lv_obj_align(s_upd_pct, LV_ALIGN_CENTER, 0, -6);
    lv_label_set_text(s_upd_msg, "into update mode");
    lv_obj_align(s_upd_msg, LV_ALIGN_CENTER, 0, 58);
    lvgl_port_unlock();
}

void ui_updating_progress(int percent)
{
    if (!s_upd) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* Never block the transfer for the sake of a redraw. */
    if (!lvgl_port_lock(5)) return;
    if (lv_arc_get_value(s_upd_arc) != percent) {
        lv_arc_set_value(s_upd_arc, percent);
        lv_label_set_text_fmt(s_upd_pct, "%d%%", percent);
        lv_obj_align(s_upd_pct, LV_ALIGN_CENTER, 0, -6);
    }
    lvgl_port_unlock();
}

void ui_updating_result(bool ok, const char *message)
{
    if (!s_upd) return;
    if (!lvgl_port_lock(200)) return;
    lv_label_set_text(s_upd_msg, message ? message : (ok ? "Restarting" : "Failed"));
    lv_obj_set_style_text_color(s_upd_msg,
        ok ? lv_color_hex(0x4DD87A) : lv_color_hex(0xFF4D4D), 0);
    lv_obj_align(s_upd_msg, LV_ALIGN_CENTER, 0, 58);
    lvgl_port_unlock();
}

void ui_updating_hide(void)
{
    if (!s_upd || !s_main) return;
    if (!lvgl_port_lock(200)) return;
    lv_screen_load(s_main);
    lvgl_port_unlock();
}
