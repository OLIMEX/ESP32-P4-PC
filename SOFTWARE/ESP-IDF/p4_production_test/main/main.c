#include "pt_display.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "sd_card_test.h"
#include "usb_flash_test.h"
#include "audio_loopback_test.h"
#include "lan_test.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG = "app_main";

/* ANSI colors */
#define ANSI_RESET   "\033[0m"
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_CYAN    "\033[96m"

/* Canonical LVGL/BSP-safe printf */
#define PT_LCD_PRINTF(fmt, ...)                                 \
    do {                                                        \
        if (bsp_display_lock(50)) {                              \
            pt_screen_printf((fmt), ##__VA_ARGS__);              \
            bsp_display_unlock();                                \
        }                                                       \
    } while (0)

/* Small helpers for aligned status output */
static void lcd_print_status_line(const char *label, const char *status_colored)
{
    /* label left padded to 32 chars, then status */
    PT_LCD_PRINTF("%-32s%s\n", label, status_colored);
}

static void lcd_print_status_inline(const char *label, const char *status_colored)
{
    /* same as above but no newline at end (caller can add more) */
    PT_LCD_PRINTF("%-32s%s", label, status_colored);
}


        static void lcd_print_reason(const char *prefix, const char *reason)
        {
            if (!reason || (reason[0] == 0)) return;
            PT_LCD_PRINTF("%s%s%s %s", ANSI_RED, prefix ? prefix : "", ANSI_RESET, reason);
        }

static pt_btn_action_t wait_for_short_or_long(void)
{
    while (true) {
        pt_btn_action_t a = pt_display_wait_button_action();
        if (a == PT_BTN_ACTION_SHORT || a == PT_BTN_ACTION_LONG) {
            return a;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static int usbj_nonblocking_vprintf(const char *fmt, va_list ap)
{
    // Малък stack buffer; ако имаш много дълги логове, ще се режат (по-добре от блокиране).
    char buf[256];

    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (len <= 0) return len;

    // Ако е отрязано, пращаме каквото имаме.
    size_t to_write = (len < (int)sizeof(buf)) ? (size_t)len : (sizeof(buf) - 1);

    // Ако драйверът не е инсталиран – не правим нищо (важното: да не блокира).
    if (!usb_serial_jtag_is_driver_installed()) {
        return (int)to_write;
    }

    // Ключовото: timeout = 0 ticks => non-blocking.
    // Ако няма кой да чете/буферът е пълен – връща 0 и ние дропваме.
    int written = usb_serial_jtag_write_bytes(buf, to_write, 0);
    (void)written;

    return (int)to_write; // Връщаме "успех" за да не се опитва лог системата да прави глупости.
}

static void setup_usb_serial_jtag_nonblocking_logs(void)
{
    // По-голям TX буфер = по-малко дроп, когато monitor е пуснат.
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 2048;
    cfg.rx_buffer_size = 256;

    // Ако вече е инсталиран някъде (BSP), това ще върне грешка.
    // Не е фатално – важното е да НЕ блокираш.
    (void)usb_serial_jtag_driver_install(&cfg);

    // Пренасочваме ESP_LOG към нашия non-blocking vprintf.
    esp_log_set_vprintf(usbj_nonblocking_vprintf);
}

/* 1) HDMI test loop */
static void run_hdmi_test_loop(void)
{
    while (true) {
        /* Show bars for 5s then return to main UI (pt_display component handles screens) */
        pt_display_color_bars();  // shows bars 5s, returns to main, clears to black (per your component)

        PT_LCD_PRINTF("Long press  BOOT - retry\n");
        PT_LCD_PRINTF("Short press BOOT - continue\n");

        pt_btn_action_t act = wait_for_short_or_long();
        if (act == PT_BTN_ACTION_LONG) {
            continue;  // repeat HDMI test
        }
        /* short -> continue to next test */
        break;
    }
}

static void boot_gpio_init_for_button(void)
{
    // BOOT е active-low обикновено
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_35,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

/* 2) SD card test loop */
static void run_sd_test_loop(void)
{
    char buf[128] = {0};

    while (true) {
        pt_display_clear(lv_color_black());
        vTaskDelay(pdMS_TO_TICKS(100));
        /* 2.1 */
        lcd_print_status_line("SD Card test........", ANSI_GREEN "[STARTED]" ANSI_RESET);

        /* 2.2 + 2.3 init */
        lcd_print_status_inline("SD Card initialization.........", "");
        
        esp_err_t err = sd_test_init();

        if (err != ESP_OK) {
            /* init failed */
            PT_LCD_PRINTF("%s[FAILED]%s\n", ANSI_RED, ANSI_RESET);
            //sd_test_deinit();

            PT_LCD_PRINTF("Long press BOOT - skip\n");
            PT_LCD_PRINTF("Short press BOOT - retry\n");

            pt_btn_action_t act = wait_for_short_or_long();
            if (act == PT_BTN_ACTION_SHORT) {
                continue;  // retry init
            } else {
                break;     // skip SD test, go next section
            }
        }

        /* init OK */
        PT_LCD_PRINTF("%s[DONE]%s\n", ANSI_GREEN, ANSI_RESET);

        /* 2.5 run test */
        lcd_print_status_inline("SD card TEST........", "");

        buf[0] = '\0';
        err = sd_test_run(buf, sizeof(buf));

        if (err == ESP_OK) {
            PT_LCD_PRINTF("%s[PASSED]%s\n", ANSI_GREEN, ANSI_RESET);
            sd_test_deinit();
            break;  // go next section
        }

        /* test failed */
        PT_LCD_PRINTF("%s[FAILED]%s\n", ANSI_RED, ANSI_RESET);

        /* reason on new line */
        if (buf[0] == '\0') {
            PT_LCD_PRINTF("SD test failed (no details)\n");
        } else {
            PT_LCD_PRINTF("%s\n", buf);
        }

        sd_test_deinit();

        /* Spec didn’t request retry/skip after test failure, only to print reason.
           We proceed to next section. If you want retry here too, tell me. */
        break;
    }
}


/* 2) USB FLASH test loop */
static void run_usb_test_loop(void)
{
    char reason[128] = {0};

    while (true) {
        pt_display_clear(lv_color_black());
        vTaskDelay(pdMS_TO_TICKS(100));

        lcd_print_status_line("USB Host init.......", ANSI_GREEN "[STARTED]" ANSI_RESET);

        lcd_print_status_inline("USB Host init.......", "");
        esp_err_t err = usb_flash_test_init();
        if (err != ESP_OK) {
            PT_LCD_PRINTF("%s[FAILED]%s\n", ANSI_RED, ANSI_RESET);
            snprintf(reason, sizeof(reason), "usb_flash_test_init failed: %s", esp_err_to_name(err));
            lcd_print_reason("", reason);

            PT_LCD_PRINTF("Long press BOOT - continue\n");
            PT_LCD_PRINTF("Short press BOOT - retry\n");
            pt_btn_action_t act = wait_for_short_or_long();
            if (act == PT_BTN_ACTION_SHORT) {
                continue;
            } else {
                break;
            }
        } else {
            PT_LCD_PRINTF("%s[DONE]%s\n", ANSI_GREEN, ANSI_RESET);
        }

        lcd_print_status_inline("USB FLASH TEST............", "");
        memset(reason, 0, sizeof(reason));
        err = usb_flash_test_run(30000, reason, sizeof(reason));
        if (err != ESP_OK) {
            PT_LCD_PRINTF("%s[FAILED]%s\n", ANSI_RED, ANSI_RESET);
            lcd_print_reason("", reason);

            PT_LCD_PRINTF("Long press BOOT - continue\n");
            PT_LCD_PRINTF("Short press BOOT - retry\n");
            pt_btn_action_t act = wait_for_short_or_long();
            if (act == PT_BTN_ACTION_SHORT) {
                // retry: deinit to ensure clean re-enumeration
                usb_flash_test_deinit();
                continue;
            } else {
                usb_flash_test_deinit();
                break;
            }
        } else {
            PT_LCD_PRINTF("%s[PASSED]%s\n", ANSI_GREEN, ANSI_RESET);
            usb_flash_test_deinit();
            break;
        }
    }
}

/* 3) AUDIO loopback test loop */
static void run_audio_test_loop(void)
{
    audio_loopback_test_cfg_t cfg = {
        .sample_rate = 48000,
        .channel = 2,
        .bits_per_sample = 16,
        .tone_hz = 1000,        // 1kHz по-чуваем от 10k
        .volume = 60,
        .duration_ms = 0,
        .continuous = true,
        .use_microphone = true, // MIC->HP loopback
    };

    while (true) {
        pt_display_clear(lv_color_black());
        vTaskDelay(pdMS_TO_TICKS(100));

        lcd_print_status_line("Audio test........", ANSI_GREEN "[STARTED]" ANSI_RESET);

        lcd_print_status_inline("Audio init....................", "");
        esp_err_t err = audio_loopback_test_init(&cfg);
        if (err != ESP_OK) {
            PT_LCD_PRINTF("%s[FAILED]%s\n", ANSI_RED, ANSI_RESET);
            PT_LCD_PRINTF("%sAudio init failed: %s%s\n", ANSI_RED, esp_err_to_name(err), ANSI_RESET);

            PT_LCD_PRINTF("Long press BOOT - skip\n");
            PT_LCD_PRINTF("Short press BOOT - retry\n");
            pt_btn_action_t act = wait_for_short_or_long();
            if (act == PT_BTN_ACTION_SHORT) {
                audio_loopback_test_deinit();
                continue;
            } else {
                audio_loopback_test_deinit();
                break;
            }
        } else {
            PT_LCD_PRINTF("%s[DONE]%s\n", ANSI_GREEN, ANSI_RESET);
        }

        lcd_print_status_inline("Audio loopback...............", "");
        err = audio_loopback_test_start();
        if (err != ESP_OK) {
            PT_LCD_PRINTF("%s[FAILED]%s\n", ANSI_RED, ANSI_RESET);
            PT_LCD_PRINTF("%sAudio start failed: %s%s\n", ANSI_RED, esp_err_to_name(err), ANSI_RESET);

            PT_LCD_PRINTF("Long press BOOT - skip\n");
            PT_LCD_PRINTF("Short press BOOT - retry\n");
            pt_btn_action_t act = wait_for_short_or_long();
            if (act == PT_BTN_ACTION_SHORT) {
                audio_loopback_test_stop();
                audio_loopback_test_deinit();
                continue;
            } else {
                audio_loopback_test_stop();
                audio_loopback_test_deinit();
                break;
            }
        } else {
            PT_LCD_PRINTF("%s[DONE]%s\n", ANSI_GREEN, ANSI_RESET);
        }

        PT_LCD_PRINTF("\n%s=== Speak to the microphone  ===%s\n", ANSI_CYAN, ANSI_RESET);
        PT_LCD_PRINTF("Short-press BOOT - stop test\n");
        PT_LCD_PRINTF("Long-press  BOOT - restart test\n");

        while (true) {
            pt_btn_action_t a = pt_display_wait_button_action();
            if (a == PT_BTN_ACTION_SHORT) {
                audio_loopback_test_stop();
                audio_loopback_test_deinit();
                return; // continue to END
            } else if (a == PT_BTN_ACTION_LONG) {
                audio_loopback_test_stop();
                audio_loopback_test_deinit();
                break; // restart audio test loop
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

static void run_lan_test_loop(void)
{
    char reason[128] = {0};
    esp_netif_t *netif = NULL;
    esp_netif_ip_info_t ip = {0};

    while (true) {
        pt_display_clear(lv_color_black());
        vTaskDelay(pdMS_TO_TICKS(100));

        /* ---------------- Header ---------------- */
        lcd_print_status_line(
            "LAN TEST...................",
            ANSI_GREEN "[STARTED]" ANSI_RESET
        );

        /* BOOT (GPIO35) is shared with RMII_TXD1 */
        gpio_reset_pin(GPIO_NUM_35);

        /* ---------------- LAN INIT ---------------- */
        lcd_print_status_inline("LAN INIT...................", "");
        esp_err_t err = lan_test_eth_start(&netif);
        if (err != ESP_OK) {
            PT_LCD_PRINTF("%s[FAILED]%s\n", ANSI_RED, ANSI_RESET);
            snprintf(reason, sizeof(reason), "LAN init failed: %s", esp_err_to_name(err));
            lcd_print_reason("", reason);
            goto fail_retry;
        }
        PT_LCD_PRINTF("%s[DONE]%s\n", ANSI_GREEN, ANSI_RESET);

        /* ---------------- DHCP IP ---------------- */
        lcd_print_status_inline("DHCP IP....................", "");
        err = lan_test_wait_dhcp(netif, 8000, &ip);
        if (err != ESP_OK) {
            PT_LCD_PRINTF("%s[FAILED]%s\n", ANSI_RED, ANSI_RESET);
            snprintf(reason, sizeof(reason), "DHCP failed: %s", esp_err_to_name(err));
            lcd_print_reason("", reason);
            goto fail_retry;
        }

        PT_LCD_PRINTF("%s[%d.%d.%d.%d]%s\n",
                      ANSI_CYAN,
                      IP2STR(&ip.ip),
                      ANSI_RESET);

        /* ---------------- PING ---------------- */
        lcd_print_status_inline("PING 8.8.8.8 ..............", "");

        lan_ping_stats_t st;
        memset(&st, 0, sizeof(st));
        memset(reason, 0, sizeof(reason));

        err = lan_test_run_stats(15000, 10, &st, reason, sizeof(reason));
        if (err != ESP_OK || st.dropped > 0) {
            /* ---- FAILED ---- */
            PT_LCD_PRINTF("%s[FAILED]%s\n", ANSI_RED, ANSI_RESET);

            PT_LCD_PRINTF(
                "%s[tx=%u rx=%u drop=%u avg=%ums]%s\n",
                ANSI_RED,
                (unsigned)st.transmitted,
                (unsigned)st.received,
                (unsigned)st.dropped,
                (unsigned)st.avg_time_ms,
                ANSI_RESET
            );

            if (err != ESP_OK) {
                lcd_print_reason("", reason);
            } else {
                lcd_print_reason("", "Packet loss detected");
            }

            goto fail_retry;
        }

        /* ---- SUCCESS ---- */
        PT_LCD_PRINTF("%s[SUCCESS]%s\n", ANSI_GREEN, ANSI_RESET);
        PT_LCD_PRINTF(
            "%s[tx=%u rx=%u drop=%u avg=%ums]%s\n",
            ANSI_GREEN,
            (unsigned)st.transmitted,
            (unsigned)st.received,
            (unsigned)st.dropped,
            (unsigned)st.avg_time_ms,
            ANSI_RESET
        );

        /* ---------------- End OK ---------------- */
        lan_test_deinit();
        boot_gpio_init_for_button();
        break;

    fail_retry:
        lan_test_deinit();
        boot_gpio_init_for_button();

        PT_LCD_PRINTF("Short press BOOT - retry\n");
        PT_LCD_PRINTF("Long  press BOOT - skip test\n");

        pt_btn_action_t act = wait_for_short_or_long();
        if (act == PT_BTN_ACTION_SHORT) {
            continue;
        } else {
            break;
        }
    }
}




/* 3) END section loop */
static void run_end_loop(void)
{
    PT_LCD_PRINTF("%s===========  END ===========%s\n", ANSI_CYAN, ANSI_RESET);

    /* 1. Configure GPIO2 as output */
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << GPIO_NUM_2,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    int level = 0;

    /* 2. Toggle forever */
    while (true) {
        gpio_set_level(GPIO_NUM_2, level);
        level = !level;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    setup_usb_serial_jtag_nonblocking_logs();
    
    ESP_LOGI(TAG, "Init HDMI/LVGL display...");
    if (pt_display_init() != ESP_OK) 
    {
        ESP_LOGE(TAG, "pt_display_init failed");
        return;
    }

    /* Outer forever loop for full test suite */
    while (true) {
        pt_display_clear(lv_color_black());

        run_hdmi_test_loop();
        run_sd_test_loop();
        run_usb_test_loop();
        run_audio_test_loop();
        run_lan_test_loop();
        run_end_loop();
        /* then repeats */
    }
}