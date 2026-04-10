#include "ui_fonts.h"

#include <stdbool.h>

#include "generated/departure_mono_11.h"
#include "generated/departure_mono_22.h"
#include "generated/departure_mono_44.h"
#include "generated/noto_sans_cjk_11.h"
#include "generated/noto_sans_cjk_22.h"

static lv_font_t s_text_11;
static lv_font_t s_text_22;
static bool s_initialized;

static void ui_fonts_init_once(void)
{
    if (s_initialized) {
        return;
    }

    s_text_11 = departure_mono_11;
    s_text_11.fallback = &noto_sans_cjk_11;

    s_text_22 = departure_mono_22;
    s_text_22.fallback = &noto_sans_cjk_22;

    s_initialized = true;
}

const lv_font_t *ui_font_text_11(void)
{
    ui_fonts_init_once();
    return &s_text_11;
}

const lv_font_t *ui_font_text_22(void)
{
    ui_fonts_init_once();
    return &s_text_22;
}

const lv_font_t *ui_font_display_44(void) { return &departure_mono_44; }
