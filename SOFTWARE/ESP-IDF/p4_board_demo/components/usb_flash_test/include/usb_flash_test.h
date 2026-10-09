#pragma once
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_flash_test_init(void);
esp_err_t usb_flash_test_run(int timeout_ms, char *out, size_t out_len);
void usb_flash_test_deinit(void);

#ifdef __cplusplus
}
#endif
