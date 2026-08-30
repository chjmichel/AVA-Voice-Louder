#include "notification_sound.h"
#include "audio_mixer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "BRAND_JINGLE";

#define LOUDER_SAMPLE_RATE          AUDIO_MIXER_SAMPLE_RATE_HZ
#define JINGLE_EXPECTED_RATE        32000U
#define JINGLE_OUTPUT_CHUNK_FRAMES  120U
#define JINGLE_TASK_STACK_SIZE      4096U
#define JINGLE_TASK_PRIORITY        5U
#define JINGLE_FIFO_TIMEOUT_MS      100U
#define JINGLE_DRAIN_TIMEOUT_MS     250U

/*
 * Embedded by the project-level CMakeLists.txt with:
 *   target_add_binary_data(... RENAME_TO start_jingle_wav)
 */
extern const uint8_t start_jingle_wav_start[] asm("_binary_start_jingle_wav_start");
extern const uint8_t start_jingle_wav_end[]   asm("_binary_start_jingle_wav_end");

typedef struct {
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    const uint8_t *data;
    size_t data_size;
} wav_info_t;

static volatile bool s_jingle_playing = false;
static volatile bool s_stop_requested = false;
static TaskHandle_t s_jingle_task = NULL;

/* 120 frames x 2 channels x 32-bit = 960 bytes in internal BSS. */
static int32_t s_mixer_buffer[JINGLE_OUTPUT_CHUNK_FRAMES * 2U];

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int16_t read_pcm16_sample(const uint8_t *data, size_t sample_index)
{
    const uint8_t *p = data + (sample_index * 2U);
    return (int16_t)read_le16(p);
}

static esp_err_t parse_wav(const uint8_t *start, const uint8_t *end, wav_info_t *info)
{
    if (start == NULL || end == NULL || info == NULL || end <= start) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t file_size = (size_t)(end - start);
    if (file_size < 12U || memcmp(start, "RIFF", 4) != 0 || memcmp(start + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Embedded start_jingle.wav is not a RIFF/WAVE file");
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(info, 0, sizeof(*info));
    size_t pos = 12U;
    bool have_fmt = false;
    bool have_data = false;

    while (pos + 8U <= file_size) {
        const uint8_t *chunk = start + pos;
        const uint32_t chunk_size = read_le32(chunk + 4);
        const size_t data_pos = pos + 8U;

        if (data_pos > file_size || (size_t)chunk_size > (file_size - data_pos)) {
            ESP_LOGE(TAG, "Invalid WAV chunk length at offset %u", (unsigned)pos);
            return ESP_ERR_INVALID_SIZE;
        }

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16U) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            const uint8_t *fmt = start + data_pos;
            info->audio_format = read_le16(fmt + 0);
            info->channels = read_le16(fmt + 2);
            info->sample_rate = read_le32(fmt + 4);
            info->bits_per_sample = read_le16(fmt + 14);
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            info->data = start + data_pos;
            info->data_size = chunk_size;
            have_data = true;
        }

        if (have_fmt && have_data) {
            break;
        }

        pos = data_pos + (size_t)chunk_size + ((size_t)chunk_size & 1U);
    }

    if (!have_fmt || !have_data) {
        ESP_LOGE(TAG, "WAV is missing fmt or data chunk");
        return ESP_ERR_NOT_FOUND;
    }

    if (info->audio_format != 1U || info->channels != 1U || info->bits_per_sample != 16U) {
        ESP_LOGE(TAG, "Unsupported WAV format: format=%u channels=%u bits=%u",
                 info->audio_format, info->channels, info->bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (info->sample_rate == 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (info->sample_rate != JINGLE_EXPECTED_RATE) {
        ESP_LOGW(TAG, "Jingle sample rate is %lu Hz (expected %u Hz); resampling anyway",
                 (unsigned long)info->sample_rate, JINGLE_EXPECTED_RATE);
    }

    return ESP_OK;
}

static void notification_sound_task(void *arg)
{
    (void)arg;

    wav_info_t wav;
    esp_err_t ret = parse_wav(start_jingle_wav_start, start_jingle_wav_end, &wav);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot play brand jingle: %s", esp_err_to_name(ret));
        goto done;
    }

    if (!audio_mixer_is_running()) {
        ESP_LOGE(TAG, "Audio mixer is not running");
        goto done;
    }

    const size_t input_samples = wav.data_size / 2U;
    if (input_samples == 0U) {
        ESP_LOGE(TAG, "Brand jingle contains no PCM samples");
        goto done;
    }

    const uint64_t total_output_frames =
        (((uint64_t)input_samples * LOUDER_SAMPLE_RATE) + wav.sample_rate - 1U) / wav.sample_rate;
    const uint64_t phase_step_q32 = ((uint64_t)wav.sample_rate << 32) / LOUDER_SAMPLE_RATE;
    uint64_t phase_q32 = 0U;
    uint64_t output_frame = 0U;

    audio_mixer_set_jingle_active(true);

    ESP_LOGI(TAG, "Playing AVA brand jingle through mixer: %lu Hz mono PCM -> 48 kHz stereo, %lu ms",
             (unsigned long)wav.sample_rate,
             (unsigned long)(((uint64_t)input_samples * 1000U) / wav.sample_rate));

    while (output_frame < total_output_frames && !s_stop_requested) {
        size_t chunk_frames = JINGLE_OUTPUT_CHUNK_FRAMES;
        const uint64_t remaining = total_output_frames - output_frame;
        if (remaining < chunk_frames) {
            chunk_frames = (size_t)remaining;
        }

        for (size_t i = 0; i < chunk_frames; ++i) {
            size_t index = (size_t)(phase_q32 >> 32);
            const uint32_t frac_q32 = (uint32_t)phase_q32;

            if (index >= input_samples) {
                index = input_samples - 1U;
            }

            const int32_t s0 = read_pcm16_sample(wav.data, index);
            const int32_t s1 = (index + 1U < input_samples)
                ? read_pcm16_sample(wav.data, index + 1U)
                : s0;
            const int32_t delta = s1 - s0;
            const int32_t interpolated = s0 +
                (int32_t)(((int64_t)delta * (int64_t)frac_q32) >> 32);

            const int32_t sample32 = interpolated * 65536;
            s_mixer_buffer[i * 2U] = sample32;
            s_mixer_buffer[i * 2U + 1U] = sample32;

            phase_q32 += phase_step_q32;
        }

        ret = audio_mixer_submit_jingle(s_mixer_buffer,
                                        chunk_frames,
                                        pdMS_TO_TICKS(JINGLE_FIFO_TIMEOUT_MS));
        if (ret != ESP_OK) {
            if (!s_stop_requested) {
                ESP_LOGE(TAG, "Mixer rejected brand-jingle PCM: %s", esp_err_to_name(ret));
            }
            break;
        }

        output_frame += chunk_frames;
    }

    if (!s_stop_requested && output_frame >= total_output_frames) {
        /* Keep ducking enabled until the last queued PCM has actually left I2S. */
        ret = audio_mixer_wait_jingle_drained(pdMS_TO_TICKS(JINGLE_DRAIN_TIMEOUT_MS));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Timed out waiting for final jingle PCM to drain: %s", esp_err_to_name(ret));
        }
        ESP_LOGI(TAG, "AVA brand jingle finished");
    } else if (s_stop_requested) {
        ESP_LOGI(TAG, "Brand jingle stopped");
    }

    audio_mixer_set_jingle_active(false);

done:
    s_stop_requested = false;
    s_jingle_playing = false;
    s_jingle_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t notification_sound_play_connection_async(void)
{
    if (s_jingle_playing) {
        ESP_LOGD(TAG, "Brand jingle already playing");
        return ESP_OK;
    }

    if (!audio_mixer_is_running()) {
        ESP_LOGE(TAG, "Audio mixer not initialized, cannot play brand jingle");
        return ESP_ERR_INVALID_STATE;
    }

    s_stop_requested = false;
    s_jingle_playing = true;

    BaseType_t task_ret = xTaskCreate(notification_sound_task,
                                      "brand_jingle",
                                      JINGLE_TASK_STACK_SIZE,
                                      NULL,
                                      JINGLE_TASK_PRIORITY,
                                      &s_jingle_task);
    if (task_ret != pdPASS) {
        s_jingle_task = NULL;
        s_jingle_playing = false;
        ESP_LOGE(TAG, "Failed to create brand jingle playback task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void notification_sound_stop(void)
{
    if (s_jingle_playing) {
        s_stop_requested = true;
    }
}

bool notification_sound_is_playing(void)
{
    return s_jingle_playing;
}
