#include "volume_control.h"
#include "louder_tas5805m.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

static const char *TAG = "VOLUME_CTRL";

static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_unit_t poti_adc_unit = ADC_UNIT_1;
static adc_channel_t poti_adc_channel = ADC_CHANNEL_0;
static uint8_t current_volume = 0;
static TaskHandle_t volume_monitor_task = NULL;

static int adc_read_raw(void)
{
    int raw_value = 0;
    if (adc_handle != NULL) {
        esp_err_t ret = adc_oneshot_read(adc_handle, poti_adc_channel, &raw_value);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(ret));
            return 0;
        }
    }
    return raw_value;
}

/**
 * Custom response curve retained from the Amped version.
 * The curve is applied to a configurable maximum logical volume. By default
 * that maximum is 60%, matching the previous project behavior.
 */
static uint8_t adc_raw_to_volume(int raw_value)
{
    static const float pot2vol_values[101] = {
        0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0000, 0.0001, 0.0005, 0.0011, 0.0020,
        0.0031, 0.0044, 0.0060, 0.0079, 0.0100, 0.0123, 0.0149, 0.0178, 0.0209, 0.0242,
        0.0278, 0.0316, 0.0357, 0.0400, 0.0446, 0.0494, 0.0544, 0.0598, 0.0653, 0.0711,
        0.0772, 0.0835, 0.0900, 0.0968, 0.1038, 0.1111, 0.1186, 0.1264, 0.1344, 0.1427,
        0.1512, 0.1600, 0.1690, 0.1783, 0.1878, 0.1975, 0.2075, 0.2178, 0.2283, 0.2390,
        0.2500, 0.2612, 0.2727, 0.2844, 0.2964, 0.3086, 0.3211, 0.3338, 0.3468, 0.3600,
        0.3735, 0.3872, 0.4011, 0.4153, 0.4298, 0.4444, 0.4594, 0.4746, 0.4900, 0.5057,
        0.5216, 0.5378, 0.5542, 0.5709, 0.5878, 0.6049, 0.6223, 0.6400, 0.6579, 0.6760,
        0.6944, 0.7131, 0.7320, 0.7511, 0.7705, 0.7901, 0.8100, 0.8301, 0.8505, 0.8711,
        0.8920, 0.9131, 0.9344, 0.9560, 0.9779, 1.0000, 1.0000, 1.0000, 1.0000, 1.0000,
        1.0000
    };

    if (raw_value <= 0) {
        return 0;
    }
    if (raw_value > 4095) {
        raw_value = 4095;
    }

    int index = (raw_value * 100 + 2047) / 4095;
    if (index < 0) index = 0;
    if (index > 100) index = 100;

    float scaled = pot2vol_values[index] * (float)CONFIG_VOLUME_MAX_PERCENT;
    int volume = (int)(scaled + 0.5f);
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    return (uint8_t)volume;
}

static esp_err_t apply_volume(uint8_t volume)
{
    current_volume = volume;
    esp_err_t ret = louder_tas5805m_set_volume_percent(volume);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply TAS5805M DSP volume %u%%: %s",
                 volume, esp_err_to_name(ret));
    }
    return ret;
}

static void volume_monitor_task_func(void *arg)
{
    const int hysteresis_threshold = CONFIG_VOLUME_HYSTERESIS_PERCENT;
    ESP_LOGI(TAG, "Volume monitoring started on GPIO%d", CONFIG_VOLUME_POT_GPIO);

    while (1) {
        int raw_value = adc_read_raw();
        uint8_t new_volume = adc_raw_to_volume(raw_value);
        int delta = abs((int)new_volume - (int)current_volume);

        if (delta >= hysteresis_threshold) {
            uint8_t old_volume = current_volume;
            apply_volume(new_volume);
            ESP_LOGI(TAG, "Pot volume %u%% -> %u%% (ADC raw=%d)",
                     old_volume, new_volume, raw_value);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_VOLUME_POLL_INTERVAL_MS));
    }
}

esp_err_t volume_control_init(void)
{
    ESP_LOGI(TAG, "Initializing H6 potentiometer on GPIO%d", CONFIG_VOLUME_POT_GPIO);

    esp_err_t ret = adc_oneshot_io_to_channel(
        (gpio_num_t)CONFIG_VOLUME_POT_GPIO,
        &poti_adc_unit,
        &poti_adc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC-capable pin: %s",
                 CONFIG_VOLUME_POT_GPIO, esp_err_to_name(ret));
        return ret;
    }

    if (poti_adc_unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d maps to ADC unit %d. Use an ADC1 pin for reliable BT operation.",
                 CONFIG_VOLUME_POT_GPIO, poti_adc_unit);
        return ESP_ERR_INVALID_ARG;
    }

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = poti_adc_unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ret = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ADC1: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    ret = adc_oneshot_config_channel(adc_handle, poti_adc_channel, &chan_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(5));
    int raw = adc_read_raw();
    uint8_t initial_volume = adc_raw_to_volume(raw);
    apply_volume(initial_volume);

    ESP_LOGI(TAG,
             "Volume control ready: GPIO%d -> ADC1_CH%d, raw=%d, initial=%u%%, max=%d%%",
             CONFIG_VOLUME_POT_GPIO,
             poti_adc_channel,
             raw,
             current_volume,
             CONFIG_VOLUME_MAX_PERCENT);
    return ESP_OK;
}

uint8_t volume_control_get_volume(void)
{
    return current_volume;
}

esp_err_t volume_control_set_volume(uint8_t volume)
{
    if (volume > VOLUME_MAX) {
        volume = VOLUME_MAX;
    }
    esp_err_t ret = apply_volume(volume);
    ESP_LOGI(TAG, "Volume set to %u%%", current_volume);
    return ret;
}

esp_err_t volume_control_start_monitoring(void)
{
    if (volume_monitor_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreate(volume_monitor_task_func,
                    "volume_monitor",
                    3072,
                    NULL,
                    5,
                    &volume_monitor_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
