#include "pt_display.h"
#include "esp_log.h"
#include "bsp/esp32_p4_function_ev_board.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "pt_display";

static lv_display_t *s_disp = NULL;
static lv_obj_t *s_main_scr = NULL;
static lv_obj_t *s_test_scr = NULL;
static void pt_display_return_to_main_locked(void);

static void btn_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_PT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);
}

static bool btn_read_raw(void)
{
    int level = gpio_get_level(CONFIG_PT_BTN_GPIO);
#if CONFIG_PT_BTN_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

esp_err_t pt_display_init(void)
{
    s_disp = bsp_display_start();
    if (!s_disp) {
        ESP_LOGE(TAG, "bsp_display_start returned NULL");
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(CONFIG_PT_DISPLAY_INIT_DELAY_MS));

    // Всички начални LVGL операции под дисплейния lock
    if (bsp_display_lock(portMAX_DELAY)) {
        lv_display_set_default(s_disp);

        // закачаме span групата за printf
        pt_screenprintf_bind();
        s_main_scr = lv_scr_act();

        // начален фон – черен, чистим текста
        lv_obj_t *scr = lv_scr_act();
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_invalidate(scr);
        pt_screenprintf_clear();

        bsp_display_unlock();
    }

    btn_init();

    return ESP_OK;
}

void pt_display_clear(lv_color_t color)
{
    if (!bsp_display_lock(portMAX_DELAY)) {
        return;
    }

    /* If we're currently showing a test screen (e.g., color bars), return to main first */
    if (s_test_scr) {
        pt_display_return_to_main_locked();
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, color, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_invalidate(scr);
    pt_screenprintf_clear();

    bsp_display_unlock();
}

static void show_color_bars(void)
{
    if (!bsp_display_lock(portMAX_DELAY)) {
        return;
    }

    /* Create a dedicated test screen so we don't destroy the printf UI on the main screen */
    if (s_test_scr) {
        lv_obj_del(s_test_scr);
        s_test_scr = NULL;
    }

    s_test_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_test_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_test_scr, LV_OPA_COVER, 0);

    const int w = lv_display_get_horizontal_resolution(s_disp);
    const int h = lv_display_get_vertical_resolution(s_disp);

    const int num = 10;
    int bar_w = w / num;

    lv_color_t cols[] = {
        lv_color_make(255, 0, 0),
        lv_color_make(0, 255, 0),
        lv_color_make(0, 0, 255),
        lv_color_make(255, 255, 0),
        lv_color_make(255, 0, 255),
        lv_color_make(0, 255, 255),
        lv_color_make(255, 128, 0),
        lv_color_make(128, 0, 255),
        lv_color_make(0, 192, 192),
        lv_color_make(255, 255, 255),
    };

    for (int i = 0; i < num; ++i) {
        lv_obj_t *bar = lv_obj_create(s_test_scr);
        lv_obj_remove_style_all(bar);

        int w_i = (i == num - 1) ? (w - (i * bar_w)) : bar_w;
        lv_obj_set_size(bar, w_i, h);
        lv_obj_set_pos(bar, i * bar_w, 0);

        lv_obj_set_style_bg_color(bar, cols[i], 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }

    lv_obj_t *label = lv_label_create(s_test_scr);
    lv_label_set_text(label, "HDMI TEST: Color Bars");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);

    lv_screen_load(s_test_scr);

    bsp_display_unlock();
}
static void pt_display_return_to_main_locked(void)
{
    /* Must be called with bsp_display_lock already held */
    if (s_main_scr) {
        lv_screen_load(s_main_scr);
    }

    if (s_test_scr) {
        lv_obj_del(s_test_scr);
        s_test_scr = NULL;
    }

    /* Ensure printf UI exists on the active (main) screen */
    pt_screenprintf_bind();
    s_main_scr = lv_scr_act();
}

void pt_display_color_bars(void)
{
    show_color_bars();
    vTaskDelay(pdMS_TO_TICKS(5000));

    if (bsp_display_lock(portMAX_DELAY)) {
        pt_display_return_to_main_locked();
        bsp_display_unlock();
    }

    pt_display_clear(lv_color_black());
    vTaskDelay(pdMS_TO_TICKS(200));
}

void pt_display_run_color_test_sequence(void)
{
    lv_color_t seq[] = {
        lv_color_white(),
        lv_color_black(),
        lv_color_hex(0xFF0000), /* red */
        lv_color_hex(0x00FF00), /* green */
        lv_color_hex(0x0000FF)  /* blue */
    };
    const size_t seq_len = sizeof(seq) / sizeof(seq[0]);

    for (size_t i = 0; i < seq_len; ++i) {
        pt_display_clear(seq[i]);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

}

pt_btn_action_t pt_display_wait_button_action(void)
{
    const int long_ms = CONFIG_PT_BTN_LONGPRESS_MS;
    const int poll_ms = 20;

    bool prev = false;
    int pressed_ms = 0;
    bool long_given = false;

    while (1) {
        bool now = btn_read_raw();

        if (now && !prev) {
            pressed_ms = 0;
            long_given = false;
        } else if (now && prev) {
            pressed_ms += poll_ms;
            if (!long_given && pressed_ms >= long_ms) {
                long_given = true;
                return PT_BTN_ACTION_LONG;
            }
        } else if (!now && prev) {
            if (!long_given) {
                return PT_BTN_ACTION_SHORT;
            }
        }

        prev = now;
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
}
