#pragma once

#include "lvgl.h"
#include "esp_err.h"

typedef enum {
    PT_BTN_ACTION_SHORT = 0,
    PT_BTN_ACTION_LONG,
    PT_BTN_ACTION_REPEAT,
    PT_BTN_ACTION_EXIT
} pt_btn_action_t;

esp_err_t pt_display_init(void);

void pt_display_clear(lv_color_t color);

void pt_display_run_color_test_sequence(void);

pt_btn_action_t pt_display_wait_button_action(void);

void pt_screenprintf_bind(void);
void pt_screenprintf_clear(void);
int  pt_screen_printf(const char *fmt, ...);
void pt_display_color_bars();