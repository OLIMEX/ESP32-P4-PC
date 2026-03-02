#include "sd_card_test.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "ff.h"

#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *TAG = "sd_card_test";

static sdmmc_card_t *g_sd_card = NULL;
static bool g_mounted = false;

#if SOC_SDMMC_IO_POWER_EXTERNAL && CONFIG_SD_TEST_USE_ONCHIP_LDO
static sd_pwr_ctrl_handle_t g_pwr_ctrl_handle = NULL;
#endif

#define SD_MOUNT_POINT   "/sdcard"
// #define SD_TEST_FILE     SD_MOUNT_POINT "/TE.txt"
#define SD_TEST_FILE "0:/SDT1.TXT"
#define SD_TEST_CONTENT  "SD-CARD TEST SUCCESSFULL!\n"

/* Optional external power switch (PMOS/load switch) */
static void sd_power_init_gpio(void)
{
#if CONFIG_SD_TEST_PWR_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_SD_TEST_PWR_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
#endif
}

static void sd_power_set(bool on)
{
#if CONFIG_SD_TEST_PWR_GPIO >= 0
    gpio_set_level((gpio_num_t)CONFIG_SD_TEST_PWR_GPIO,
                   on ? CONFIG_SD_TEST_PWR_ON_LEVEL : !CONFIG_SD_TEST_PWR_ON_LEVEL);
#else
    (void)on;
#endif
}

static esp_err_t sd_setup_onchip_ldo(sdmmc_host_t *host)
{
#if SOC_SDMMC_IO_POWER_EXTERNAL && CONFIG_SD_TEST_USE_ONCHIP_LDO
    if (g_pwr_ctrl_handle) {
        host->pwr_ctrl_handle = g_pwr_ctrl_handle;
        return ESP_OK;
    }

    sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = CONFIG_SD_TEST_LDO_CHAN_ID,
    };

    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &g_pwr_ctrl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sd_pwr_ctrl_new_on_chip_ldo failed: %s (0x%x)",
                 esp_err_to_name(err), (unsigned)err);
        return err;
    }

    host->pwr_ctrl_handle = g_pwr_ctrl_handle;
    ESP_LOGI(TAG, "On-chip LDO power control enabled (chan_id=%d)", CONFIG_SD_TEST_LDO_CHAN_ID);
    return ESP_OK;
#else
    (void)host;
    return ESP_OK;
#endif
}

static void sd_teardown_onchip_ldo(void)
{
#if SOC_SDMMC_IO_POWER_EXTERNAL && CONFIG_SD_TEST_USE_ONCHIP_LDO
    if (g_pwr_ctrl_handle) {
        sd_pwr_ctrl_del_on_chip_ldo(g_pwr_ctrl_handle);
        g_pwr_ctrl_handle = NULL;
    }
#endif
}

static esp_err_t mount_sdmmc_once(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = CONFIG_SD_TEST_MAX_FREQ_KHZ;

    /* Enable internal LDO power control when required (ESP32-P4 often needs this for specific IOs/domains) */
    esp_err_t err = sd_setup_onchip_ldo(&host);
    if (err != ESP_OK) {
        return err;
    }

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = CONFIG_SD_TEST_BUS_WIDTH;

    /* Helpful when external pull-ups are missing/weak. Still needs correct IO voltage domain. */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    /* If your board routes SDMMC pins via GPIO matrix, set them explicitly.
       Set these in menuconfig (sd_card_test component). */
    if (CONFIG_SD_TEST_PIN_CLK >= 0) slot.clk = CONFIG_SD_TEST_PIN_CLK;
    if (CONFIG_SD_TEST_PIN_CMD >= 0) slot.cmd = CONFIG_SD_TEST_PIN_CMD;
    if (CONFIG_SD_TEST_PIN_D0  >= 0) slot.d0  = CONFIG_SD_TEST_PIN_D0;
#if CONFIG_SD_TEST_BUS_WIDTH == 4
    if (CONFIG_SD_TEST_PIN_D1  >= 0) slot.d1  = CONFIG_SD_TEST_PIN_D1;
    if (CONFIG_SD_TEST_PIN_D2  >= 0) slot.d2  = CONFIG_SD_TEST_PIN_D2;
    if (CONFIG_SD_TEST_PIN_D3  >= 0) slot.d3  = CONFIG_SD_TEST_PIN_D3;
#endif
#endif

    /* Optional card detect / write protect */
#if CONFIG_SD_TEST_PIN_CD >= 0
    slot.gpio_cd = CONFIG_SD_TEST_PIN_CD;
#endif
#if CONFIG_SD_TEST_PIN_WP >= 0
    slot.gpio_wp = CONFIG_SD_TEST_PIN_WP;
#endif

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG,
             "Mount SDMMC: width=%d max_freq=%dkHz CLK=%d CMD=%d D0=%d",
             slot.width, host.max_freq_khz, slot.clk, slot.cmd, slot.d0);

    err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &cfg, &g_sd_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_sdmmc_mount failed: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
    }
    return err;
}

esp_err_t sd_test_init(void)
{
    if (g_mounted) {
        return ESP_ERR_INVALID_STATE;   
    }
    sd_power_init_gpio();

    /* Power on + stabilization delay */
    sd_power_set(true);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_SD_TEST_INIT_DELAY_MS));

    esp_err_t err = ESP_FAIL;

    for (int attempt = 0; attempt <= CONFIG_SD_TEST_RETRY_COUNT; ++attempt) {
        err = mount_sdmmc_once();
        if (err == ESP_OK) {
            g_mounted = true;
            ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);
            return ESP_OK;
        }

        /* Power-cycle retry */
        if (attempt < CONFIG_SD_TEST_RETRY_COUNT) {
            ESP_LOGW(TAG, "Retry SD init (attempt %d/%d) with power-cycle", attempt + 1, CONFIG_SD_TEST_RETRY_COUNT);
            sd_test_deinit(); /* unmount if partially mounted, also tears down LDO if enabled */
            sd_power_set(false);
            vTaskDelay(pdMS_TO_TICKS(150));
            sd_power_set(true);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_SD_TEST_INIT_DELAY_MS));
        }
    }

    return err;
}

void sd_test_deinit(void)
{
    if (g_mounted) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, g_sd_card);
        g_sd_card = NULL;
        g_mounted = false;
    }

    /* Optional: power off */
    sd_power_set(false);

    /* Tear down on-chip LDO handle (safe to call even if not created) */
    sd_teardown_onchip_ldo();
}

static esp_err_t fatfs_write_test(char *why, size_t why_sz)
{
    FILINFO info;
    FRESULT fr2 = f_stat(SD_TEST_FILE, &info);
    ESP_LOGW(TAG, "f_stat=%d attr=0x%02x", fr2, info.fattrib);

    FIL f;
    FRESULT fr = f_open(&f, SD_TEST_FILE, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        snprintf(why, why_sz, "f_open(w) failed: FR=%d", (int)fr);
        return ESP_FAIL;
    }

    UINT bw = 0;
    fr = f_write(&f, SD_TEST_CONTENT, (UINT)strlen(SD_TEST_CONTENT), &bw);
    if (fr != FR_OK || bw != (UINT)strlen(SD_TEST_CONTENT)) {
        snprintf(why, why_sz, "f_write failed: FR=%d bw=%u", (int)fr, (unsigned)bw);
        f_close(&f);
        return ESP_FAIL;
    }

    fr = f_sync(&f); // това е “fsync”-ът в FatFs
    if (fr != FR_OK) {
        snprintf(why, why_sz, "f_sync failed: FR=%d", (int)fr);
        f_close(&f);
        return ESP_FAIL;
    }

    fr = f_close(&f);
    if (fr != FR_OK) {
        snprintf(why, why_sz, "f_close failed: FR=%d", (int)fr);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t fatfs_read_test(char *out, size_t out_sz, char *why, size_t why_sz)
{
    FIL f;
    FRESULT fr = f_open(&f, SD_TEST_FILE, FA_READ);
    if (fr != FR_OK) {
        snprintf(why, why_sz, "f_open(r) failed: FR=%d", (int)fr);
        return ESP_FAIL;
    }

    UINT br = 0;
    fr = f_read(&f, out, (UINT)(out_sz - 1), &br);
    if (fr != FR_OK) {
        snprintf(why, why_sz, "f_read failed: FR=%d br=%u", (int)fr, (unsigned)br);
        f_close(&f);
        return ESP_FAIL;
    }

    out[br] = 0;

    fr = f_close(&f);
    if (fr != FR_OK) {
        snprintf(why, why_sz, "f_close failed: FR=%d", (int)fr);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t sd_test_run(char *out, size_t sz)
{
    if (!g_mounted) return ESP_ERR_INVALID_STATE;
    if (!out || sz < 64) return ESP_ERR_INVALID_ARG;

    char why[96] = {0};
    char readback[96] = {0};

    if (fatfs_write_test(why, sizeof(why)) != ESP_OK) {
        snprintf(out, sz, "WRITE FAIL: %s", why);
        return ESP_FAIL;
    }

    if (fatfs_read_test(readback, sizeof(readback), why, sizeof(why)) != ESP_OK) {
        snprintf(out, sz, "READ FAIL: %s", why);
        return ESP_FAIL;
    }

    if (strcmp(readback, SD_TEST_CONTENT) != 0) {
        snprintf(out, sz, "VERIFY FAIL: got='%s'", readback);
        return ESP_FAIL;
    }

    snprintf(out, sz, "OK");
    return ESP_OK;
}


