/*******************************************************************************
 * Size: 14 px
 * Bpp: 4
 * Opts: --font .cache/upstream/bootstrap-icons.woff -r 0xF914 --size 14 --bpp 4 --no-compress --format lvgl --lv-include lvgl.h -o claude_nocompress_tmp.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef CLAUDE_NOCOMPRESS_TMP
#define CLAUDE_NOCOMPRESS_TMP 1
#endif

#if CLAUDE_NOCOMPRESS_TMP

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F914 "樂" */
    0x0, 0x9, 0xc0, 0x3, 0x70, 0x0, 0x0, 0x0,
    0x9, 0xf3, 0x8, 0xa0, 0x15, 0x0, 0x4, 0x11,
    0xfa, 0x9, 0x80, 0xdf, 0x0, 0xd, 0xe3, 0x7f,
    0x2b, 0x5b, 0xf7, 0x0, 0x1, 0xcf, 0x7e, 0xad,
    0x9f, 0xb0, 0x0, 0x0, 0x7, 0xff, 0xff, 0xfe,
    0x11, 0x44, 0x55, 0x32, 0x4d, 0xff, 0xfe, 0xdf,
    0xd7, 0x68, 0x9a, 0xad, 0xff, 0xfe, 0x84, 0x20,
    0x0, 0x0, 0x8e, 0xff, 0xfa, 0x9d, 0xfc, 0x0,
    0x5e, 0x99, 0xbe, 0xfe, 0x60, 0x11, 0x6, 0xd3,
    0x5c, 0x58, 0xbb, 0xb7, 0x0, 0x0, 0x2, 0xe2,
    0x86, 0x2f, 0x57, 0x40, 0x0, 0xb, 0x40, 0xb5,
    0x6, 0xc0, 0x0, 0x0, 0x0, 0x0, 0xd3, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 224, .box_w = 14, .box_h = 15, .ofs_x = 0, .ofs_y = -1}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 63764, .range_length = 1, .glyph_id_start = 1,
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



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t claude_nocompress_tmp = {
#else
lv_font_t claude_nocompress_tmp = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 15,          /*The maximum line height required by the font*/
    .base_line = 1,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 0,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if CLAUDE_NOCOMPRESS_TMP*/
