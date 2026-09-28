/*******************************************************************************
 * Size: 14 px
 * Bpp: 4
 * Opts: --no-compress --no-prefilter --bpp 4 --size 14 --font managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff -r 0xF130 --format lvgl --force-fast-kern-format --lv-include lvgl.h --lv-fallback lv_font_montserrat_14 -o components/ui/font_mic_14.c
 ******************************************************************************/

/* The microphone (U+F130) from the Font Awesome 5 Free copy LVGL bundles for
 * its own symbols (SIL OFL 1.1). LVGL's built-in symbol set has no microphone,
 * and the mic-gain readout had been borrowing the music note instead.
 *
 * Generated with the options above, then line_height/base_line set to
 * Montserrat 14's 16/3 so this label measures exactly like the volume label
 * beside it. The digits come from the fallback. Regenerate rather than edit. */

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef FONT_MIC_14
#define FONT_MIC_14 1
#endif

#if FONT_MIC_14

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F130 "" */
    0x0, 0x0, 0x10, 0x0, 0x0, 0x0, 0x1c, 0xff,
    0x50, 0x0, 0x0, 0x9f, 0xff, 0xf1, 0x0, 0x0,
    0xdf, 0xff, 0xf3, 0x0, 0x0, 0xdf, 0xff, 0xf4,
    0x0, 0x0, 0xdf, 0xff, 0xf4, 0x0, 0xd3, 0xdf,
    0xff, 0xf4, 0xc4, 0xf5, 0xdf, 0xff, 0xf4, 0xe6,
    0xe7, 0xbf, 0xff, 0xf2, 0xf5, 0xac, 0x4f, 0xff,
    0xa5, 0xf2, 0x3f, 0x92, 0x64, 0x3e, 0xa0, 0x5,
    0xfe, 0xac, 0xfc, 0x0, 0x0, 0x28, 0xfc, 0x50,
    0x0, 0x0, 0x45, 0xf9, 0x50, 0x0, 0x0, 0xbf,
    0xff, 0xf3, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 154, .box_w = 10, .box_h = 15, .ofs_x = 0, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 61744, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};

extern const lv_font_t lv_font_montserrat_14;


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t font_mic_14 = {
#else
lv_font_t font_mic_14 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = 3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -5,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = &lv_font_montserrat_14,
#endif
    .user_data = NULL,
};



#endif /*#if FONT_MIC_14*/

