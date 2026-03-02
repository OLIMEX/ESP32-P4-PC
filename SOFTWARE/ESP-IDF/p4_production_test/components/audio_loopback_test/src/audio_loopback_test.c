#include "audio_loopback_test.h"

#include "bsp/esp32_p4_function_ev_board.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "audio_loopback_test";

static esp_codec_dev_handle_t  s_spk = NULL;
static esp_codec_dev_handle_t  s_mic = NULL;
static audio_loopback_test_cfg_t s_cfg;
static TaskHandle_t            s_task = NULL;
static volatile bool           s_running = false;

#ifndef CONFIG_AUDIO_TEST_DMA_FRAME_SAMPLES
#define CONFIG_AUDIO_TEST_DMA_FRAME_SAMPLES 512
#endif

#ifndef CONFIG_AUDIO_TEST_TASK_STACK
#define CONFIG_AUDIO_TEST_TASK_STACK 4096
#endif

#ifndef CONFIG_AUDIO_TEST_TASK_PRIO
#define CONFIG_AUDIO_TEST_TASK_PRIO 12
#endif

static void loopback_task(void *arg)
{
    (void)arg;

    const int bytes_per_sample = (s_cfg.bits_per_sample / 8);
    const int ch_out = (s_cfg.channel <= 1) ? 2 : s_cfg.channel; // HP is typically stereo; keep stable
    const int ch_in  = 2; // FORCE 2ch to avoid reconfig collisions in BSP/I2S layers

    const int frame_samples = (s_cfg.frame_samples > 0) ? s_cfg.frame_samples : CONFIG_AUDIO_TEST_DMA_FRAME_SAMPLES;
    const int buf_samples_total = frame_samples * ch_in;
    const int buf_bytes = buf_samples_total * bytes_per_sample;

    int16_t *buf = (int16_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "No memory for audio buffer (%d bytes)", buf_bytes);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    // If output channels != input channels, allocate a small convert buffer.
    int16_t *out_buf = buf;
    int out_bytes = buf_bytes;

    if (ch_out != ch_in) {
        out_bytes = frame_samples * ch_out * bytes_per_sample;
        out_buf = (int16_t *)heap_caps_malloc(out_bytes, MALLOC_CAP_DEFAULT);
        if (!out_buf) {
            ESP_LOGE(TAG, "No memory for out buffer (%d bytes)", out_bytes);
            heap_caps_free(buf);
            s_running = false;
            vTaskDelete(NULL);
            return;
        }
    }

    uint32_t loops_no_data = 0;
    uint32_t loops_total = 0;
    int peak_last = 0;

    while (s_running) {
        // NOTE: esp_codec_dev_read() returns an esp_codec_dev error code (ESP_CODEC_DEV_OK == 0),
        // not "bytes read". The underlying I2S read is blocking with a timeout; on success we
        // assume the requested buffer was filled.
        int ret = esp_codec_dev_read(s_mic, buf, buf_bytes);
        if (ret != ESP_CODEC_DEV_OK) {
            loops_no_data++;
            loops_total++;

            if ((loops_total % 200) == 0) {
                ESP_LOGW(TAG,
                         "MIC read error=%d for %u loops (no_data=%u). If this keeps happening: no I2S RX / wrong pins / codec ADC not enabled.",
                         ret, loops_total, loops_no_data);
            }
            // Avoid spinning too hard if RX is dead.
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        const int r = buf_bytes; // "bytes produced" on success

        // Compute a quick peak meter (first channel only).
        int16_t *s = buf;
        int samples = r / 2; // int16 count
        int peak = 0;
        for (int i = 0; i < samples; i += ch_in) {
            int v = s[i];
            int a = (v < 0) ? -v : v;
            if (a > peak) peak = a;
        }
        peak_last = peak;

        if ((loops_total % 200) == 0) {
            ESP_LOGI(TAG, "MIC ok: bytes=%d peak=%d", r, peak_last);
        }

        // Channel conversion if needed
        if (ch_out == ch_in) {
            esp_codec_dev_write(s_spk, buf, r);
        } else if (ch_out == 2 && ch_in == 1) {
            // duplicate mono -> stereo
            int16_t *in = buf;
            int16_t *out = out_buf;
            int frames = r / (bytes_per_sample * ch_in);
            for (int i = 0; i < frames; i++) {
                int16_t v = in[i];
                out[2*i + 0] = v;
                out[2*i + 1] = v;
            }
            esp_codec_dev_write(s_spk, out_buf, frames * ch_out * bytes_per_sample);
        } else {
            // generic: copy min(ch_in, ch_out) and zero the rest
            int frames = r / (bytes_per_sample * ch_in);
            int16_t *in = buf;
            int16_t *out = out_buf;
            for (int i = 0; i < frames; i++) {
                for (int c = 0; c < ch_out; c++) {
                    int16_t v = 0;
                    if (c < ch_in) v = in[i*ch_in + c];
                    out[i*ch_out + c] = v;
                }
            }
            esp_codec_dev_write(s_spk, out_buf, frames * ch_out * bytes_per_sample);
        }

        loops_total++;
    }

    if (out_buf != buf) heap_caps_free(out_buf);
    heap_caps_free(buf);

    ESP_LOGI(TAG, "Loopback task stopped");
    vTaskDelete(NULL);
}

static void tone_task(void *arg)
{
    (void)arg;

    const int bytes_per_sample = (s_cfg.bits_per_sample / 8);
    const int channels = (s_cfg.channel <= 0) ? 1 : s_cfg.channel;
    const int sample_rate = (s_cfg.sample_rate <= 0) ? 48000 : s_cfg.sample_rate;

    const int frame_samples = (s_cfg.frame_samples > 0) ? s_cfg.frame_samples : CONFIG_AUDIO_TEST_DMA_FRAME_SAMPLES;
    const int buf_samples_total = frame_samples * channels;
    const int buf_bytes = buf_samples_total * bytes_per_sample;

    int16_t *buf = (int16_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "No memory for tone buffer (%d bytes)", buf_bytes);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    float phase = 0.0f;
    const float step = 2.0f * (float)M_PI * ((float)s_cfg.tone_hz / (float)sample_rate);
    const int16_t amp = 12000;

    int elapsed_ms = 0;

    while (s_running) {
        for (int i = 0; i < frame_samples; i++) {
            int16_t v = (int16_t)(sinf(phase) * amp);
            phase += step;
            if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;

            for (int c = 0; c < channels; c++) {
                buf[i*channels + c] = v;
            }
        }

        esp_codec_dev_write(s_spk, buf, buf_bytes);

        if (!s_cfg.continuous) {
            elapsed_ms += (frame_samples * 1000) / sample_rate;
            if (elapsed_ms >= s_cfg.duration_ms) break;
        }
    }

    heap_caps_free(buf);
    ESP_LOGI(TAG, "Tone task stopped");
    vTaskDelete(NULL);
}

esp_err_t audio_loopback_test_init(const audio_loopback_test_cfg_t *cfg)
{
    // Defaults
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.sample_rate = 48000;
    s_cfg.channel = 2;
    s_cfg.bits_per_sample = 16;
    s_cfg.tone_hz = 1000;
    s_cfg.volume = 50;
    s_cfg.duration_ms = 2000;
    s_cfg.continuous = true;

    // Loopback defaults
    s_cfg.use_microphone = true;
    s_cfg.mic_gain = 30;
    s_cfg.frame_samples = CONFIG_AUDIO_TEST_DMA_FRAME_SAMPLES;

    if (cfg) {
        // Copy user fields (including optional tail if present)
        s_cfg = *cfg;

        // Fill missing optional fields sensibly
        if (s_cfg.sample_rate <= 0) s_cfg.sample_rate = 48000;
        if (s_cfg.bits_per_sample <= 0) s_cfg.bits_per_sample = 16;
        if (s_cfg.channel <= 0) s_cfg.channel = 2;
        if (s_cfg.volume < 0) s_cfg.volume = 0;
        if (s_cfg.volume > 100) s_cfg.volume = 100;

        if (s_cfg.mic_gain == 0) s_cfg.mic_gain = 30;
        if (s_cfg.frame_samples <= 0) s_cfg.frame_samples = CONFIG_AUDIO_TEST_DMA_FRAME_SAMPLES;
        // If user didn't set use_microphone explicitly, assume true when tone_hz == 0
        // (This keeps old projects that set tone_hz intact.)
        if (!s_cfg.use_microphone && s_cfg.tone_hz == 0) s_cfg.use_microphone = true;
    }

    // IMPORTANT: keep channel count stable when enabling mic (prevents re-init fights in BSP/I2S)
    if (s_cfg.use_microphone) {
        s_cfg.channel = 2;
    }

    // Speaker codec
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGE(TAG, "bsp_audio_codec_speaker_init failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t spk_info = {
        .sample_rate     = s_cfg.sample_rate,
        .channel         = s_cfg.channel,
        .bits_per_sample = s_cfg.bits_per_sample,
    };

    ESP_ERROR_CHECK(esp_codec_dev_open(s_spk, &spk_info));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_spk, s_cfg.volume));

    if (s_cfg.use_microphone) {
        s_mic = bsp_audio_codec_microphone_init();
        if (!s_mic) {
            ESP_LOGE(TAG, "bsp_audio_codec_microphone_init failed");
            return ESP_FAIL;
        }

        // Force mic open as 2ch too, to avoid BSP reconfig collisions
        esp_codec_dev_sample_info_t mic_info = {
            .sample_rate     = s_cfg.sample_rate,
            .channel         = 2,
            .bits_per_sample = s_cfg.bits_per_sample,
        };

        ESP_ERROR_CHECK(esp_codec_dev_open(s_mic, &mic_info));

        // Best-effort input gain if supported by this codec driver
        esp_err_t gerr = esp_codec_dev_set_in_gain(s_mic, s_cfg.mic_gain);
        if (gerr != ESP_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_set_in_gain(%d) not supported (err=0x%x)", s_cfg.mic_gain, (int)gerr);
        }
    }

    return ESP_OK;
}

esp_err_t audio_loopback_test_start(void)
{
    if (s_running) return ESP_OK;

    if (!s_spk) return ESP_ERR_INVALID_STATE;
    if (s_cfg.use_microphone && !s_mic) return ESP_ERR_INVALID_STATE;

    s_running = true;

    if (s_cfg.use_microphone) {
        ESP_LOGI(TAG, "Loopback started: %d Hz, MIC->OUT, 16-bit, frames=%d",
                 s_cfg.sample_rate, s_cfg.frame_samples);
        xTaskCreate(loopback_task, "aud_loop", CONFIG_AUDIO_TEST_TASK_STACK, NULL, CONFIG_AUDIO_TEST_TASK_PRIO, &s_task);
    } else {
        ESP_LOGI(TAG, "Tone started: %d Hz, %d Hz tone", s_cfg.sample_rate, s_cfg.tone_hz);
        xTaskCreate(tone_task, "aud_tone", CONFIG_AUDIO_TEST_TASK_STACK, NULL, CONFIG_AUDIO_TEST_TASK_PRIO, &s_task);
    }

    return ESP_OK;
}

esp_err_t audio_loopback_test_stop(void)
{
    if (!s_running) return ESP_OK;
    s_running = false;

    // Give the task a moment to exit.
    for (int i = 0; i < 100; i++) {
        if (eTaskGetState(s_task) == eDeleted) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_task = NULL;
    return ESP_OK;
}

esp_err_t audio_loopback_test_deinit(void)
{
    audio_loopback_test_stop();

    if (s_mic) {
        esp_codec_dev_close(s_mic);
        s_mic = NULL;
    }
    if (s_spk) {
        esp_codec_dev_close(s_spk);
        s_spk = NULL;
    }
    return ESP_OK;
}
