/*******************************************************************************
 * Size: 14 px
 * Bpp: 4
 * Opts: --font .cache/upstream/bootstrap-icons.woff -r 0xF912 --size 14 --bpp 4 --no-compress --format lvgl --lv-include lvgl.h -o anthropic_nocompress_tmp.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef ANTHROPIC_NOCOMPRESS_TMP
#define ANTHROPIC_NOCOMPRESS_TMP 1
#endif

#if ANTHROPIC_NOCOMPRESS_TMP

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F912 "裸" */
    0x0, 0x0, 0x44, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x7, 0xff, 0x40, 0xbf, 0x60, 0x0, 0x0, 0xd,
    0xff, 0xb0, 0x4f, 0xc0, 0x0, 0x0, 0x3f, 0xde,
    0xf1, 0xe, 0xf2, 0x0, 0x0, 0xaf, 0x79, 0xf7,
    0x8, 0xf9, 0x0, 0x0, 0xff, 0x13, 0xfd, 0x2,
    0xfe, 0x0, 0x6, 0xfd, 0x55, 0xef, 0x30, 0xbf,
    0x50, 0xc, 0xff, 0xff, 0xff, 0xa0, 0x5f, 0xc0,
    0x3f, 0xe8, 0x88, 0x8f, 0xf0, 0xe, 0xf2, 0x9f,
    0x80, 0x0, 0xb, 0xf6, 0x8, 0xf8, 0x89, 0x20,
    0x0, 0x3, 0x97, 0x2, 0x98
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 224, .box_w = 14, .box_h = 11, .ofs_x = 0, .ofs_y = 2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 63762, .range_length = 1, .glyph_id_start = 1,
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
const lv_font_t anthropic_nocompress_tmp = {
#else
lv_font_t anthropic_nocompress_tmp = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 11,          /*The maximum line height required by the font*/
    .base_line = -2,             /*Baseline measured from the bottom of the line*/
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



#endif /*#if ANTHROPIC_NOCOMPRESS_TMP*/
