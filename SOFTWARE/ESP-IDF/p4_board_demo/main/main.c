#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"

#include "pt_display.h"
#include "sd_card_test.h"
#include "usb_flash_test.h"
#include "audio_loopback_test.h"
#include "lan_test.h"

static const char *TAG = "p4_board_demo";

typedef enum {
    DEMO_NOT_RUN = 0,
    DEMO_PASS,
    DEMO_WARN,
    DEMO_FAIL,
    DEMO_SKIP,
} demo_result_t;

typedef enum {
    TEST_HDMI = 0,
    TEST_SD,
    TEST_USB,
    TEST_AUDIO,
    TEST_ETHERNET,
    TEST_COUNT,
} demo_test_t;

typedef struct {
    const char *name;
    demo_result_t result;
    char detail[96];
} demo_test_result_t;

static demo_test_result_t s_results[TEST_COUNT] = {
    [TEST_HDMI] = { .name = "HDMI" },
    [TEST_SD] = { .name = "SDMMC" },
    [TEST_USB] = { .name = "USB HOST" },
    [TEST_AUDIO] = { .name = "AUDIO" },
    [TEST_ETHERNET] = { .name = "ETHERNET" },
};

static const char *result_name(demo_result_t result)
{
    switch (result) {
    case DEMO_PASS: return "PASS";
    case DEMO_WARN: return "WARN";
    case DEMO_FAIL: return "FAIL";
    case DEMO_SKIP: return "SKIP";
    default: return "NOT RUN";
    }
}

static void set_result(demo_test_t test, demo_result_t result, const char *detail)
{
    s_results[test].result = result;
    snprintf(s_results[test].detail, sizeof(s_results[test].detail), "%s",
             detail ? detail : "");
    ESP_LOGI(TAG, "%s: %s%s%s", s_results[test].name, result_name(result),
             detail && detail[0] ? " - " : "", detail ? detail : "");
}

static bool button_is_pressed(void)
{
    int level = gpio_get_level(CONFIG_PT_BTN_GPIO);
#if CONFIG_PT_BTN_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

static void wait_for_button_release(void)
{
    while (button_is_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(80));
}

static pt_btn_action_t wait_for_action(void)
{
    pt_btn_action_t action = pt_display_wait_button_action();
    wait_for_button_release();
    return action;
}

static void restore_demo_button(void)
{
    gpio_reset_pin(CONFIG_PT_BTN_GPIO);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_PT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

static void clear_text_screen(void)
{
    pt_display_clear(lv_color_black());
}

static void print_board_information(void)
{
    esp_chip_info_t chip = {0};
    uint32_t flash_size = 0;
    esp_chip_info(&chip);
    esp_err_t flash_err = esp_flash_get_size(esp_flash_default_chip, &flash_size);
    size_t psram_size = esp_psram_get_size();

    clear_text_screen();
    pt_screen_printf("ESP32-P4-PC BOARD DEMO\n\n");
    pt_screen_printf("ESP-IDF: %s\n", esp_get_idf_version());
    pt_screen_printf("Chip revision: %u, CPU cores: %u\n",
                     (unsigned)chip.revision, (unsigned)chip.cores);
    if (flash_err == ESP_OK) {
        pt_screen_printf("Flash: %lu MB\n", (unsigned long)(flash_size / (1024 * 1024)));
    } else {
        pt_screen_printf("Flash size: unavailable\n");
    }
    pt_screen_printf("PSRAM: %lu MB\n", (unsigned long)(psram_size / (1024 * 1024)));
    pt_screen_printf("HDMI: 1280x720 @ 60 Hz, RGB888\n");
    pt_screen_printf("SDMMC: 4-bit @ 20 MHz\n\n");
    pt_screen_printf("Short press BOOT to begin.\n");
    pt_screen_printf("Long press BOOT to remain on this screen.\n");

    if (wait_for_action() == PT_BTN_ACTION_LONG) {
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

static void run_hdmi_demo(void)
{
    clear_text_screen();
    pt_screen_printf("1/5 HDMI VISUAL TEST\n\n");
    pt_screen_printf("The color bars will be displayed for five seconds.\n");
    pt_screen_printf("Check for correct colors, stable sync, and no flicker.\n");
    vTaskDelay(pdMS_TO_TICKS(1500));
    pt_display_color_bars();

    clear_text_screen();
    pt_screen_printf("1/5 HDMI VISUAL TEST\n\n");
    pt_screen_printf("Short press: picture was correct (PASS)\n");
    pt_screen_printf("Long press: picture was wrong (FAIL)\n");
    if (wait_for_action() == PT_BTN_ACTION_LONG) {
        set_result(TEST_HDMI, DEMO_FAIL, "operator rejected the picture");
    } else {
        set_result(TEST_HDMI, DEMO_PASS, "1280x720 @ 60 Hz");
    }
}

static void run_sd_demo(void)
{
    clear_text_screen();
    pt_screen_printf("2/5 MICROSD TEST\n\n");
    pt_screen_printf("Insert a FAT-formatted microSD card.\n");
    pt_screen_printf("The demo uses native 4-bit SDMMC at 20 MHz.\n");
    pt_screen_printf("It creates, verifies, then removes P4DEMO.TXT.\n\n");
    pt_screen_printf("Short press: run test\nLong press: skip\n");
    if (wait_for_action() == PT_BTN_ACTION_LONG) {
        set_result(TEST_SD, DEMO_SKIP, "operator skipped");
        return;
    }

    clear_text_screen();
    pt_screen_printf("2/5 MICROSD TEST\n\nMounting card...\n");
    esp_err_t err = sd_test_init();
    if (err != ESP_OK) {
        char detail[96];
        snprintf(detail, sizeof(detail), "mount failed: %s", esp_err_to_name(err));
        set_result(TEST_SD, DEMO_FAIL, detail);
    } else {
        char detail[128] = {0};
        err = sd_test_run(detail, sizeof(detail));
        set_result(TEST_SD, err == ESP_OK ? DEMO_PASS : DEMO_FAIL,
                   err == ESP_OK ? "4-bit read/write/verify at 20 MHz" : detail);
    }
    sd_test_deinit();

    pt_screen_printf("\nResult: %s\n%s\n", result_name(s_results[TEST_SD].result),
                     s_results[TEST_SD].detail);
    pt_screen_printf("Short press to continue.\n");
    (void)wait_for_action();
}

static void run_usb_demo(void)
{
    clear_text_screen();
    pt_screen_printf("3/5 USB HOST TEST\n\n");
    pt_screen_printf("Use any of the 4 USB-A ports\n(two stacked connectors).\n");
    pt_screen_printf("Drive must be FAT/FAT32, not exFAT or NTFS.\n");
    pt_screen_printf("The demo creates, verifies, then removes P4DEMO.TXT.\n\n");
    pt_screen_printf("Short press: run test\nLong press: skip\n");
    if (wait_for_action() == PT_BTN_ACTION_LONG) {
        set_result(TEST_USB, DEMO_SKIP, "operator skipped");
        return;
    }

    clear_text_screen();
    pt_screen_printf("3/5 USB HOST TEST\n\nWaiting up to 30 seconds for a drive...\n");
    char detail[128] = {0};
    esp_err_t err = usb_flash_test_init();
    if (err == ESP_OK) {
        err = usb_flash_test_run(30000, detail, sizeof(detail));
    } else {
        snprintf(detail, sizeof(detail), "USB init failed: %s", esp_err_to_name(err));
    }
    usb_flash_test_deinit();
    set_result(TEST_USB, err == ESP_OK ? DEMO_PASS : DEMO_FAIL,
               err == ESP_OK ? "MSC mount/read/write/verify" : detail);

    pt_screen_printf("\nResult: %s\n%s\n", result_name(s_results[TEST_USB].result),
                     s_results[TEST_USB].detail);
    pt_screen_printf("Short press to continue.\n");
    (void)wait_for_action();
}

static void run_audio_demo(void)
{
    clear_text_screen();
    pt_screen_printf("4/5 AUDIO TEST\n\n");
    pt_screen_printf("Connect headphones or a CTIA headset.\n");
    pt_screen_printf("First: listen for a 1 kHz test tone.\n\n");
    pt_screen_printf("Short press: start tone\nLong press: skip audio\n");
    if (wait_for_action() == PT_BTN_ACTION_LONG) {
        set_result(TEST_AUDIO, DEMO_SKIP, "operator skipped");
        return;
    }

    audio_loopback_test_cfg_t audio_cfg = {
        .sample_rate = CONFIG_AUDIO_TEST_SAMPLE_RATE,
        .channel = CONFIG_AUDIO_TEST_CHANNELS,
        .bits_per_sample = CONFIG_AUDIO_TEST_BITS_PER_SAMPLE,
        .tone_hz = 1000,
        .volume = 35,
        .continuous = true,
        /* Initialize both paths once.  The tone and loopback stages then reuse
         * the same codec/I2S session instead of reopening shared channels. */
        .use_microphone = true,
        .mic_gain = 30,
        .frame_samples = CONFIG_AUDIO_TEST_DMA_FRAME_SAMPLES,
    };

    esp_err_t err = audio_loopback_test_init(&audio_cfg);
    if (err == ESP_OK) {
        err = audio_loopback_test_start_tone();
    }
    if (err != ESP_OK) {
        char detail[96];
        snprintf(detail, sizeof(detail), "audio output error: %s", esp_err_to_name(err));
        set_result(TEST_AUDIO, DEMO_FAIL, detail);
        audio_loopback_test_deinit();
        return;
    }

    clear_text_screen();
    pt_screen_printf("4/5 AUDIO TEST - OUTPUT TONE\n\n");
    pt_screen_printf("A steady 1 kHz tone should be audible.\n\n");
    pt_screen_printf("Short press: tone is clear\n");
    pt_screen_printf("Long press: no/incorrect tone\n");
    bool output_ok = wait_for_action() == PT_BTN_ACTION_SHORT;
    audio_loopback_test_stop();

    if (!output_ok) {
        set_result(TEST_AUDIO, DEMO_FAIL, "operator rejected headphone output tone");
        audio_loopback_test_deinit();
        return;
    }

    clear_text_screen();
    pt_screen_printf("4/5 AUDIO TEST - MICROPHONE\n\n");
    pt_screen_printf("A 4-pole CTIA headset is required.\n");
    pt_screen_printf("Plain headphones have no microphone.\n\n");
    pt_screen_printf("Short press: start microphone loopback\n");
    pt_screen_printf("Long press: skip microphone check\n");
    if (wait_for_action() == PT_BTN_ACTION_LONG) {
        set_result(TEST_AUDIO, DEMO_WARN, "headphone output passed; microphone input skipped");
        audio_loopback_test_deinit();
        return;
    }

    err = audio_loopback_test_start_loopback();
    if (err != ESP_OK) {
        char detail[96];
        snprintf(detail, sizeof(detail), "codec/stream error: %s", esp_err_to_name(err));
        set_result(TEST_AUDIO, DEMO_FAIL, detail);
        audio_loopback_test_deinit();
    } else {
        clear_text_screen();
        pt_screen_printf("4/5 AUDIO TEST - LOOPBACK ACTIVE\n\n");
        pt_screen_printf("Speak into the headset microphone.\n");
        pt_screen_printf("Short press: audio is clear (PASS)\n");
        pt_screen_printf("Long press: no/incorrect audio (FAIL)\n");
        if (wait_for_action() == PT_BTN_ACTION_LONG) {
            set_result(TEST_AUDIO, DEMO_FAIL, "operator rejected loopback audio");
        } else if (audio_loopback_test_get_peak() < 256) {
            set_result(TEST_AUDIO, DEMO_FAIL, "no headset microphone signal detected");
        } else {
            set_result(TEST_AUDIO, DEMO_PASS, "headset microphone-to-headphone loopback");
        }
        audio_loopback_test_stop();
        audio_loopback_test_deinit();
    }
}

static void run_ethernet_demo(void)
{
    clear_text_screen();
    pt_screen_printf("5/5 ETHERNET TEST\n\n");
    pt_screen_printf("Connect Ethernet to a network with DHCP.\n");
    pt_screen_printf("Link and DHCP test the board hardware.\n");
    pt_screen_printf("Ping to %s is an optional Internet check.\n\n",
                     CONFIG_LAN_TEST_PING_TARGET);
    pt_screen_printf("Short press: run test\nLong press: skip\n");
    if (wait_for_action() == PT_BTN_ACTION_LONG) {
        set_result(TEST_ETHERNET, DEMO_SKIP, "operator skipped");
        return;
    }

    /* BOOT is GPIO35, which becomes RMII TXD1 while Ethernet is active. */
    gpio_reset_pin(CONFIG_PT_BTN_GPIO);

    clear_text_screen();
    pt_screen_printf("5/5 ETHERNET TEST\n\nWaiting for link...\n");

    esp_netif_t *netif = NULL;
    esp_netif_ip_info_t ip = {0};
    esp_err_t err = lan_test_eth_start(&netif);
    if (err != ESP_OK) {
        char detail[96];
        snprintf(detail, sizeof(detail), "link failed: %s", esp_err_to_name(err));
        set_result(TEST_ETHERNET, DEMO_FAIL, detail);
    } else {
        pt_screen_printf("Link UP. Waiting for DHCP...\n");
        err = lan_test_wait_dhcp(netif, CONFIG_LAN_TEST_DHCP_TIMEOUT_MS, &ip);
        if (err != ESP_OK) {
            set_result(TEST_ETHERNET, DEMO_FAIL, "link up, DHCP timed out");
        } else {
            char ip_text[20];
            snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&ip.ip));
            pt_screen_printf("IP address: %s\nPinging %s...\n",
                             ip_text, CONFIG_LAN_TEST_PING_TARGET);

            lan_ping_stats_t stats = {0};
            char ping_reason[96] = {0};
            esp_err_t ping_err = lan_test_run_stats(15000, 10, &stats,
                                                    ping_reason, sizeof(ping_reason));
            char detail[96];
            if (ping_err == ESP_OK) {
                snprintf(detail, sizeof(detail), "%s, ping %lu/%lu, avg %lu ms",
                         ip_text, (unsigned long)stats.received,
                         (unsigned long)stats.transmitted,
                         (unsigned long)stats.avg_time_ms);
                set_result(TEST_ETHERNET, DEMO_PASS, detail);
            } else {
                snprintf(detail, sizeof(detail), "%s; Internet ping unavailable", ip_text);
                set_result(TEST_ETHERNET, DEMO_WARN, detail);
            }
        }
    }

    /* Keep Ethernet initialized after a completed test.  The summary can
     * start the demo again, and lan_test_eth_start() deliberately reuses the
     * live interface.  This also avoids an ESP-IDF 5.5 EMAC/IGMP teardown
     * mismatch where lwIP removes a multicast filter that the EMAC no longer
     * has, producing misleading errors after an otherwise successful test. */
    restore_demo_button();

    pt_screen_printf("\nResult: %s\n%s\n", result_name(s_results[TEST_ETHERNET].result),
                     s_results[TEST_ETHERNET].detail);
    pt_screen_printf("Short press to show the summary.\n");
    (void)wait_for_action();
}

static void show_summary(void)
{
    bool any_fail = false;
    bool any_warn_or_skip = false;

    clear_text_screen();
    pt_screen_printf("ESP32-P4-PC DEMO SUMMARY\n\n");
    for (int i = 0; i < TEST_COUNT; ++i) {
        pt_screen_printf("%-10s  %-7s  %s\n", s_results[i].name,
                         result_name(s_results[i].result), s_results[i].detail);
        any_fail |= s_results[i].result == DEMO_FAIL;
        any_warn_or_skip |= s_results[i].result == DEMO_WARN ||
                            s_results[i].result == DEMO_SKIP;
    }

    pt_screen_printf("\nOVERALL: %s\n",
                     any_fail ? "FAIL" : (any_warn_or_skip ? "COMPLETE WITH WARNINGS" : "PASS"));
    pt_screen_printf("\nShort press: restart and run again\n");
    pt_screen_printf("Long press: stop here until reset\n");

    ESP_LOGI(TAG, "DEMO_RESULT_BEGIN");
    for (int i = 0; i < TEST_COUNT; ++i) {
        ESP_LOGI(TAG, "DEMO_RESULT,%s,%s,%s", s_results[i].name,
                 result_name(s_results[i].result), s_results[i].detail);
    }
    ESP_LOGI(TAG, "DEMO_RESULT_END,overall=%s",
             any_fail ? "FAIL" : (any_warn_or_skip ? "WARN" : "PASS"));

    if (wait_for_action() == PT_BTN_ACTION_LONG) {
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* Ethernet remains initialized after a successful test to avoid a known
     * ESP-IDF 5.5 EMAC/IGMP teardown mismatch.  Restarting gives a repeated
     * demo a completely clean peripheral and GPIO state. */
    esp_restart();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32-P4-PC board demo");
    ESP_ERROR_CHECK(pt_display_init());

    while (true) {
        for (int i = 0; i < TEST_COUNT; ++i) {
            s_results[i].result = DEMO_NOT_RUN;
            s_results[i].detail[0] = '\0';
        }

        print_board_information();
        run_hdmi_demo();
        run_sd_demo();
        run_usb_demo();
        run_audio_demo();
        run_ethernet_demo();
        show_summary();
    }
}
