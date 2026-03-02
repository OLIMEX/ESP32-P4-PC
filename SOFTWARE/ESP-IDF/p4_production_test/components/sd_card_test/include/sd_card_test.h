#pragma once
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sd_test_init(void);
void sd_test_deinit(void);

/* On failure, out_buf will contain a short human-readable reason. */
esp_err_t sd_test_run(char *out_buf, size_t out_buf_size);

#ifdef __cplusplus
}
#endif
