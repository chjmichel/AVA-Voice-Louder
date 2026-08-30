#include "notification_sound.h"
#include "bt_app_hf.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "NOTIF_SOUND";

#define NOTIFICATION_SAMPLE_RATE 44100
#define NOTIFICATION_DURATION_MS 500  // Total duration: 500ms

/**
 * @brief Generate a sine wave tone at specified frequency
 * 
 * @param frequency Frequency in Hz
 * @param duration_ms Duration in milliseconds
 * @param amplitude Volume level 0-32767 (max 16-bit)
 * @return Allocated buffer with samples (must be freed by caller)
 */
static int16_t* generate_tone(float frequency, int duration_ms, int16_t amplitude)
{
    int num_samples = (NOTIFICATION_SAMPLE_RATE * duration_ms) / 1000;
    int16_t *samples = (int16_t *)malloc(num_samples * sizeof(int16_t));
    
    if (samples == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for tone");
        return NULL;
    }
    
    float phase_increment = 2.0f * M_PI * frequency / NOTIFICATION_SAMPLE_RATE;
    float phase = 0.0f;
    
    // Generate sine wave with envelope (quick decay)
    for (int i = 0; i < num_samples; i++) {
        // Exponential decay envelope: e^(-3*t/duration)
        float decay = expf(-3.0f * i / num_samples);
        
        // Generate sine wave
        float sample = sinf(phase) * amplitude * decay;
        samples[i] = (int16_t)sample;
        
        phase += phase_increment;
        if (phase > 2.0f * M_PI) {
            phase -= 2.0f * M_PI;
        }
    }
    
    return samples;
}

/**
 * @brief Generate stereo samples from mono tone
 * @param mono_samples Mono audio samples
 * @param num_mono_samples Number of mono samples
 * @return Allocated stereo buffer (32-bit, must be freed by caller)
 */
static int32_t* mono_to_stereo_32bit(const int16_t *mono_samples, int num_mono_samples)
{
    int32_t *stereo = (int32_t *)malloc(num_mono_samples * 2 * sizeof(int32_t));
    
    if (stereo == NULL) {
        ESP_LOGE(TAG, "Failed to allocate stereo buffer");
        return NULL;
    }
    
    // Convert 16-bit mono to interleaved 32-bit stereo (L, R, L, R...).
    // The old implementation wrote only one slot per frame, which caused the
    // notification to alternate between channels instead of duplicating it.
    for (int i = 0; i < num_mono_samples; i++) {
        int32_t sample = ((int32_t)mono_samples[i]) << 16;
        stereo[i * 2] = sample;
        stereo[i * 2 + 1] = sample;
    }
    
    return stereo;
}

esp_err_t notification_sound_play_connection(void)
{
    if (tx_chan == NULL) {
        ESP_LOGE(TAG, "I2S not initialized, cannot play notification sound");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Playing connection notification sound...");
    
    // Generate the "badabummm" sound pattern:
    // - Low kick (80 Hz) for 150ms
    // - Medium tone (200 Hz) for 150ms  
    // - High tone (400 Hz) for 150ms
    // - Quick decay on each
    
    int16_t amplitude = 20000;  // ~60% of max 16-bit
    int16_t *kick_samples = generate_tone(80.0f, 150, amplitude);
    int16_t *mid_samples = generate_tone(200.0f, 150, amplitude);
    int16_t *high_samples = generate_tone(400.0f, 150, amplitude);
    
    if (kick_samples == NULL || mid_samples == NULL || high_samples == NULL) {
        ESP_LOGE(TAG, "Failed to generate tone samples");
        if (kick_samples) free(kick_samples);
        if (mid_samples) free(mid_samples);
        if (high_samples) free(high_samples);
        return ESP_ERR_NO_MEM;
    }
    
    // Get sample counts
    int kick_count = (NOTIFICATION_SAMPLE_RATE * 150) / 1000;
    int mid_count = (NOTIFICATION_SAMPLE_RATE * 150) / 1000;
    int high_count = (NOTIFICATION_SAMPLE_RATE * 150) / 1000;
    
    // Convert to stereo and write to I2S
    esp_err_t ret = ESP_OK;
    
    // Play kick
    int32_t *stereo_kick = mono_to_stereo_32bit(kick_samples, kick_count);
    if (stereo_kick) {
        ret = i2s_channel_write(tx_chan, stereo_kick, kick_count * 2 * 4, NULL, pdMS_TO_TICKS(500));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write kick sound to I2S");
        }
        free(stereo_kick);
    }
    
    // Play mid
    if (ret == ESP_OK) {
        int32_t *stereo_mid = mono_to_stereo_32bit(mid_samples, mid_count);
        if (stereo_mid) {
            ret = i2s_channel_write(tx_chan, stereo_mid, mid_count * 2 * 4, NULL, pdMS_TO_TICKS(500));
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write mid sound to I2S");
            }
            free(stereo_mid);
        }
    }
    
    // Play high
    if (ret == ESP_OK) {
        int32_t *stereo_high = mono_to_stereo_32bit(high_samples, high_count);
        if (stereo_high) {
            ret = i2s_channel_write(tx_chan, stereo_high, high_count * 2 * 4, NULL, pdMS_TO_TICKS(500));
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write high sound to I2S");
            }
            free(stereo_high);
        }
    }
    
    // Free mono buffers
    free(kick_samples);
    free(mid_samples);
    free(high_samples);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Notification sound played successfully");
    }
    
    return ret;
}
