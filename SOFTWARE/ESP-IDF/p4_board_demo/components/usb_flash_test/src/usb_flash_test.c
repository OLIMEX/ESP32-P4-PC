/*
 * usb_flash_test - ESP-IDF component
 * USB Host MSC + FATFS: mount /usb, write+read+verify /usb/P4DEMO.TXT
 *
 * Adds: HUB reset (FE1.1S) + reliable usb_host_lib_handle_events loop
 */

#include "usb_flash_test.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* ESP32-P4-PC USB hub control signals. */
// GPIO21 drives HUB_RST# and is active low.
#define USB_HUB_RST_GPIO        (21)
#define USB_HUB_RST_ACTIVE_LOW  (1)

// Set this to the +5V_USB_HOST enable GPIO if a future board revision adds one.
#define USB_VBUS_EN_GPIO        (-1)
#define USB_VBUS_EN_ACTIVE_HIGH (1)

// Allow the hub to settle after power/reset before processing host events.
#define USB_BRINGUP_DELAY_MS    (200)
/* ================================= */

#define USB_MOUNT_PATH "/usb"
#define USB_TEST_FILE  USB_MOUNT_PATH "/P4DEMO.TXT"
#define USB_TEST_STR   "ESP32-P4-PC USB HOST DEMO SUCCESSFUL!\n"

static QueueHandle_t s_evt_q = NULL;
static TaskHandle_t  s_usb_ev_task = NULL;
static bool s_inited = false;
static bool s_host_installed = false;
static bool s_msc_installed = false;
static volatile bool s_stop_events = false;
static volatile bool s_shutdown_requested = false;
static volatile bool s_shutdown_complete = false;

static void set_out(char *out, size_t out_len, const char *msg)
{
    if (out && out_len) snprintf(out, out_len, "%s", msg ? msg : "");
}

static void hub_power_and_reset(void)
{
#if USB_VBUS_EN_GPIO >= 0
    {
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
#endif

#if USB_HUB_RST_GPIO >= 0
    {
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
#endif

    vTaskDelay(pdMS_TO_TICKS(USB_BRINGUP_DELAY_MS));
}

static void usb_events_task(void *arg)
{
    (void)arg;

    while (!s_stop_events) {
        uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(100), &event_flags);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "usb_host_lib_handle_events failed: %s", esp_err_to_name(err));
        } else {
            // This diagnostic confirms that the USB host event loop is running.
            if (event_flags) {
                ESP_LOGI(TAG, "usb_host events: flags=0x%08lx", (unsigned long)event_flags);
            }

            /* The host library may only be uninstalled after its last client
               has gone and every enumerated device (including the onboard
               external hub) has been released. */
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                esp_err_t free_err = usb_host_device_free_all();
                if (free_err == ESP_OK) {
                    s_shutdown_complete = true;
                } else if (free_err != ESP_ERR_NOT_FINISHED) {
                    ESP_LOGE(TAG, "usb_host_device_free_all failed: %s",
                             esp_err_to_name(free_err));
                }
            }
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
                s_shutdown_complete = true;
            }
            if (s_shutdown_requested && s_shutdown_complete) {
                break;
            }
        }
    }

    s_usb_ev_task = NULL;
    vTaskDelete(NULL);
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
    // Exercise directory creation as part of the filesystem test.
    const char *dir = USB_MOUNT_PATH "/test";
    struct stat st = {0};
    if (stat(dir, &st) != 0) {
        if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "Could not create optional test directory: errno=%d", errno);
        }
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

    if (remove(USB_TEST_FILE) != 0) {
        ESP_LOGE(TAG, "VERIFY OK but cleanup failed: errno=%d", errno);
        set_out(out, out_len, "VERIFY OK, file cleanup failed");
        return ESP_FAIL;
    }
    (void)rmdir(dir); /* Remove it only if it is empty. */

    set_out(out, out_len, "USB flash test PASSED");
    return ESP_OK;
}

esp_err_t usb_flash_test_init(void)
{
    if (s_inited) return ESP_OK;

    s_evt_q = xQueueCreate(8, sizeof(msc_host_event_t));
    if (!s_evt_q) return ESP_ERR_NO_MEM;
    s_stop_events = false;
    s_shutdown_requested = false;
    s_shutdown_complete = false;

    // 1) Power and reset the hub before USB host initialization.
    hub_power_and_reset();

    // 2) Install USB Host; peripheral_map=0 selects the ESP-IDF default mapping.
    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .peripheral_map = 0,
        .root_port_unpowered = false,
    };

    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        usb_flash_test_deinit();
        return err;
    }
    s_host_installed = true;

    // 3) Start the event task; device enumeration depends on it.
    if (xTaskCreate(usb_events_task, "usb_events", 3072, NULL, 20, &s_usb_ev_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create usb_events task");
        usb_flash_test_deinit();
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
        usb_flash_test_deinit();
        return err;
    }
    s_msc_installed = true;

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

    msc_host_device_info_t dev_info = {0};
    if (msc_host_get_device_info(dev, &dev_info) == ESP_OK) {
        uint64_t capacity = (uint64_t)dev_info.sector_count * dev_info.sector_size;
        ESP_LOGI(TAG, "MSC storage: %lu sectors x %lu bytes (%llu MiB)",
                 (unsigned long)dev_info.sector_count,
                 (unsigned long)dev_info.sector_size,
                 (unsigned long long)(capacity / (1024ULL * 1024ULL)));
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
        ESP_LOGE(TAG, "msc_host_vfs_register failed: %s (0x%x). FAT/FAT32 on MBR or GPT is supported; exFAT and NTFS are not",
                 esp_err_to_name(err), (unsigned)err);
        set_out(out, out_len, "FAT mount failed; see serial log for details");
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
    /* Keep the library event task alive while the MSC client is removed.  It
       must process NO_CLIENTS, free the hub/device tree, and observe ALL_FREE
       before usb_host_uninstall() is safe. */
    s_shutdown_requested = s_host_installed;

    if (s_msc_installed) {
        esp_err_t err = msc_host_uninstall();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "msc_host_uninstall failed: %s", esp_err_to_name(err));
        }
        s_msc_installed = false;
    } else if (s_host_installed) {
        esp_err_t err = usb_host_device_free_all();
        if (err == ESP_OK) {
            s_shutdown_complete = true;
        } else if (err != ESP_ERR_NOT_FINISHED) {
            ESP_LOGE(TAG, "usb_host_device_free_all failed: %s", esp_err_to_name(err));
        }
    }

    if (s_host_installed) {
        for (int i = 0; i < 150 && !s_shutdown_complete; ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* The event task normally exits itself after ALL_FREE. */
    for (int i = 0; i < 25 && s_usb_ev_task && s_shutdown_complete; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_host_installed && s_shutdown_complete) {
        esp_err_t err = usb_host_uninstall();
        if (err == ESP_OK) {
            s_host_installed = false;
        } else {
            ESP_LOGE(TAG, "usb_host_uninstall failed: %s", esp_err_to_name(err));
        }
    } else if (s_host_installed) {
        /* Never force an unsafe uninstall: ESP-IDF's hub teardown asserts and
           reboots if a port/device is still active. */
        ESP_LOGE(TAG, "USB shutdown timed out; leaving host installed to avoid reboot");
    }

    if (s_evt_q) {
        vQueueDelete(s_evt_q);
        s_evt_q = NULL;
    }

    s_inited = false;
    s_stop_events = false;
    s_shutdown_requested = false;
    s_shutdown_complete = false;
}
