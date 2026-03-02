#pragma once

#include <stdbool.h> 
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate;        // e.g. 48000
    int channel;            // 1 or 2
    int bits_per_sample;    // 16
    int tone_hz;            // e.g. 1000 or 10000
    int volume;             // 0..100
    int duration_ms;        // if continuous==false, play this long
    bool continuous;        // true = keep playing until stop()

    // Loopback options (optional; defaults apply if omitted)
    bool use_microphone;     // true = MIC -> HP loopback, false = tone generator
    int  mic_gain;           // input gain (implementation-dependent), default 30
    int  frame_samples;      // samples per channel per read/write, default 512
} audio_loopback_test_cfg_t;

esp_err_t audio_loopback_test_init(const audio_loopback_test_cfg_t *cfg);
esp_err_t audio_loopback_test_start(void);
esp_err_t audio_loopback_test_stop(void);
esp_err_t audio_loopback_test_deinit(void);

#ifdef __cplusplus
}
#endif
