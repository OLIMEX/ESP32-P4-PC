#include "pt_display.h"
#include "esp_log.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "pt_screenprintf";

static lv_obj_t *s_spangroup = NULL;
static lv_color_t s_cur_color;

static lv_color_t ansi_to_color(int code, bool bright)
{
    switch (code) {
        case 30: return bright ? lv_color_make(128,128,128) : lv_color_make(0,0,0);
        case 31: return bright ? lv_color_make(255,64,64)   : lv_color_make(255,0,0);
        case 32: return bright ? lv_color_make(64,255,64)   : lv_color_make(0,255,0);
        case 33: return bright ? lv_color_make(255,255,64)  : lv_color_make(255,255,0);
        case 34: return bright ? lv_color_make(64,64,255)   : lv_color_make(0,0,255);
        case 35: return bright ? lv_color_make(255,64,255)  : lv_color_make(255,0,255);
        case 36: return bright ? lv_color_make(64,255,255)  : lv_color_make(0,255,255);
        case 37: return bright ? lv_color_make(255,255,255) : lv_color_make(220,220,220);
        default: return lv_color_make(255,255,255);
    }
}

void pt_screenprintf_bind(void)
{
    lv_obj_t *scr = lv_scr_act();

    if (s_spangroup) {
        lv_obj_del(s_spangroup);
        s_spangroup = NULL;
    }

    s_spangroup = lv_spangroup_create(scr);
    lv_obj_set_size(s_spangroup, lv_pct(100), lv_pct(100));
    lv_obj_align(s_spangroup, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_spangroup_set_align(s_spangroup, LV_TEXT_ALIGN_LEFT);
    lv_spangroup_set_mode(s_spangroup, LV_SPAN_MODE_BREAK);

    s_cur_color = lv_color_make(255,255,255);
}

void pt_screenprintf_clear(void)
{
    if (!s_spangroup) return;
    lv_obj_clean(s_spangroup);
    s_cur_color = lv_color_make(255,255,255);
}

static void append_text(const char *txt, lv_color_t color)
{
    if (!s_spangroup || !txt || !txt[0]) return;

    lv_span_t *span = lv_spangroup_new_span(s_spangroup);
    lv_span_set_text(span, txt);
    lv_style_t *st = lv_span_get_style(span);
    lv_style_init(st);
    lv_style_set_text_color(st, color);
    lv_spangroup_refresh(s_spangroup);
}

static void parse_and_append(const char *str)
{
    const char *p = str;
    char buf[256];
    size_t bi = 0;

    while (*p && bi < sizeof(buf) - 1) {
        if (*p == '\x1b' && *(p+1) == '[') {
            buf[bi] = '\0';
            append_text(buf, s_cur_color);
            bi = 0;

            p += 2;
            int num = 0;
            int nums[3];
            int nnums = 0;
            bool bright = false;

            while (*p && *p != 'm' && nnums < 3) {
                if (*p >= '0' && *p <= '9') {
                    num = num*10 + (*p - '0');
                } else if (*p == ';') {
                    nums[nnums++] = num;
                    num = 0;
                } else {
                    break;
                }
                p++;
            }
            if (*p == 'm') {
                nums[nnums++] = num;
                for (int i = 0; i < nnums; ++i) {
                    int code = nums[i];
                    if (code == 0) {
                        s_cur_color = lv_color_make(255,255,255);
                    } else if (code == 1) {
                        bright = true;
                    } else if (code >= 30 && code <= 37) {
                        s_cur_color = ansi_to_color(code, bright);
                    }
                }
                p++;
            }
        } else {
            buf[bi++] = *p++;
        }
    }
    buf[bi] = '\0';
    append_text(buf, s_cur_color);
}

int pt_screen_printf(const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (len > 0) {
        parse_and_append(tmp);
    }
    return len;
}
