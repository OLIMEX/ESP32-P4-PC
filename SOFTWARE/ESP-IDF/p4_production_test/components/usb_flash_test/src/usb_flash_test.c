/*
 * usb_flash_test - ESP-IDF component
 * USB Host MSC + FATFS: mount /usb, write+read+verify /usb/USBTEST.TXT
 *
 * Adds: HUB reset (FE1.1S) + reliable usb_host_lib_handle_events loop
 */

#include "usb_flash_test.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"

#include "usb/usb_host.h"
#include "msc_host.h"
#include "msc_host_vfs.h"
#include "esp_vfs_fat.h"

static const char *TAG = "usb_flash_test";

/* ==== НАГЛАСИ ПО ТВОЯ ХАРДУЕР ==== */
// По схемата ти: GPIO21_HUB_RST# (активно ниско)
#define USB_HUB_RST_GPIO        (21)
#define USB_HUB_RST_ACTIVE_LOW  (1)

// Ако имаш GPIO, който пуска +5V_USB_HOST, сложи го тук. Ако нямаш: -1.
#define USB_VBUS_EN_GPIO        (-1)
#define USB_VBUS_EN_ACTIVE_HIGH (1)

// Колко да чакаме след reset/power, преди да чакаме събития
#define USB_BRINGUP_DELAY_MS    (200)
/* ================================= */

#define USB_MOUNT_PATH "/usb"
#define USB_TEST_FILE  USB_MOUNT_PATH "/USBTEST.TXT"
#define USB_TEST_STR   "USB-FLASH TEST SUCCESSFUL!\n"

static QueueHandle_t s_evt_q = NULL;
static TaskHandle_t  s_usb_ev_task = NULL;
static bool s_inited = false;

static void set_out(char *out, size_t out_len, const char *msg)
{
    if (out && out_len) snprintf(out, out_len, "%s", msg ? msg : "");
}

static void hub_power_and_reset(void)
{
    if (USB_VBUS_EN_GPIO >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << USB_VBUS_EN_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = 0,
            .pull_down_en = 0,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&io);
        int on = USB_VBUS_EN_ACTIVE_HIGH ? 1 : 0;
        gpio_set_level(USB_VBUS_EN_GPIO, on);
        ESP_LOGI(TAG, "VBUS_EN GPIO%d -> %d", USB_VBUS_EN_GPIO, on);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (USB_HUB_RST_GPIO >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << USB_HUB_RST_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = 0,
            .pull_down_en = 0,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&io);

        int assert_level   = USB_HUB_RST_ACTIVE_LOW ? 0 : 1;
        int deassert_level = USB_HUB_RST_ACTIVE_LOW ? 1 : 0;

        // Assert reset
        gpio_set_level(USB_HUB_RST_GPIO, assert_level);
        vTaskDelay(pdMS_TO_TICKS(20));

        // Deassert reset
        gpio_set_level(USB_HUB_RST_GPIO, deassert_level);
        ESP_LOGI(TAG, "HUB_RST GPIO%d: assert=%d deassert=%d",
                 USB_HUB_RST_GPIO, assert_level, deassert_level);
    }

    vTaskDelay(pdMS_TO_TICKS(USB_BRINGUP_DELAY_MS));
}

static void usb_events_task(void *arg)
{
    (void)arg;

    while (1) {
        uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_lib_handle_events failed: %s", esp_err_to_name(err));
        } else {
            // Полезно за диагностика: виждаш че event loop реално се върти
            if (event_flags) {
                ESP_LOGI(TAG, "usb_host events: flags=0x%08lx", (unsigned long)event_flags);
            }
        }
    }
}

// MSC callback -> queue
static void msc_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (!event || !s_evt_q) return;
    xQueueSend(s_evt_q, event, 0);
}

static esp_err_t do_file_test(char *out, size_t out_len)
{
    // (по желание) директория тест
    const char *dir = USB_MOUNT_PATH "/test";
    struct stat st = {0};
    if (stat(dir, &st) != 0) {
        mkdir(dir, 0775);
    }

    FILE *f = fopen(USB_TEST_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "WRITE FAIL: fopen(w) errno=%d", errno);
        set_out(out, out_len, "WRITE FAIL: fopen(w)");
        return ESP_FAIL;
    }

    size_t wr = fwrite(USB_TEST_STR, 1, strlen(USB_TEST_STR), f);
    int ferr = ferror(f);
    fclose(f);

    if (wr != strlen(USB_TEST_STR) || ferr) {
        ESP_LOGE(TAG, "WRITE FAIL: fwrite wr=%u ferr=%d", (unsigned)wr, ferr);
        set_out(out, out_len, "WRITE FAIL: fwrite");
        return ESP_FAIL;
    }

    f = fopen(USB_TEST_FILE, "r");
    if (!f) {
        ESP_LOGE(TAG, "READ FAIL: fopen(r) errno=%d", errno);
        set_out(out, out_len, "READ FAIL: fopen(r)");
        return ESP_FAIL;
    }

    char buf[128] = {0};
    char *res = fgets(buf, sizeof(buf), f);
    fclose(f);

    if (!res) {
        ESP_LOGE(TAG, "READ FAIL: fgets NULL");
        set_out(out, out_len, "READ FAIL: fgets");
        return ESP_FAIL;
    }

    if (strcmp(buf, USB_TEST_STR) != 0) {
        ESP_LOGE(TAG, "VERIFY FAIL: got='%s'", buf);
        set_out(out, out_len, "VERIFY FAIL");
        return ESP_FAIL;
    }

    set_out(out, out_len, "USB flash test PASSED");
    return ESP_OK;
}

esp_err_t usb_flash_test_init(void)
{
    if (s_inited) return ESP_OK;

    s_evt_q = xQueueCreate(8, sizeof(msc_host_event_t));
    if (!s_evt_q) return ESP_ERR_NO_MEM;

    // 1) Power + reset HUB (важно при hub топология)
    hub_power_and_reset();

    // 2) USB Host install (peripheral_map=0 -> default; на P4 по принцип е HS)
    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .peripheral_map = 0,
        .root_port_unpowered = false,
    };

    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    // 3) Стартираме event loop task (без него няма enumeration)
    if (xTaskCreate(usb_events_task, "usb_events", 3072, NULL, 20, &s_usb_ev_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create usb_events task");
        return ESP_ERR_NO_MEM;
    }

    // 4) MSC driver
    msc_host_driver_config_t msc_cfg = {
        .create_backround_task = true,
        .task_priority = 10,
        .stack_size = 4096,
        .callback = msc_cb,
    };

    err = msc_host_install(&msc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "USB Host + MSC initialized");
    return ESP_OK;
}

esp_err_t usb_flash_test_run(int timeout_ms, char *out, size_t out_len)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    const TickType_t to = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS((uint32_t)timeout_ms);

    ESP_LOGI(TAG, "Waiting for USB MSC device (timeout=%d ms)...", timeout_ms);

    msc_host_event_t evt = {0};
    if (xQueueReceive(s_evt_q, &evt, to) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout waiting for MSC_DEVICE_CONNECTED");
        set_out(out, out_len, "Timeout waiting for USB flash");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "MSC event=%d addr=%d", (int)evt.event, (int)evt.device.address);

    if (evt.event != MSC_DEVICE_CONNECTED) {
        set_out(out, out_len, "Unexpected MSC event");
        return ESP_FAIL;
    }

    const uint8_t dev_addr = evt.device.address;

    msc_host_device_handle_t dev = NULL;
    esp_err_t err = msc_host_install_device(dev_addr, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install_device failed: %s", esp_err_to_name(err));
        set_out(out, out_len, "Install device failed");
        return err;
    }

    // Useful while bringing up hub topologies
    msc_host_print_descriptors(dev);

    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    msc_host_vfs_handle_t vfs = NULL;
    err = msc_host_vfs_register(dev, USB_MOUNT_PATH, &mount_cfg, &vfs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_vfs_register failed: %s", esp_err_to_name(err));
        set_out(out, out_len, "Mount failed");
        msc_host_uninstall_device(dev);
        return err;
    }

    ESP_LOGI(TAG, "USB mounted at %s", USB_MOUNT_PATH);

    esp_err_t test_err = do_file_test(out, out_len);

    msc_host_vfs_unregister(vfs);
    msc_host_uninstall_device(dev);

    return test_err;
}

void usb_flash_test_deinit(void)
{
    if (!s_inited) return;

    msc_host_uninstall();
    usb_host_uninstall();

    if (s_usb_ev_task) {
        vTaskDelete(s_usb_ev_task);
        s_usb_ev_task = NULL;
    }

    if (s_evt_q) {
        vQueueDelete(s_evt_q);
        s_evt_q = NULL;
    }

    s_inited = false;
}
