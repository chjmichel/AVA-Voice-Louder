#include "louder_tas5805m.h"

#include <math.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "LOUDER_TAS5805M";

#define TAS5805M_I2C_TIMEOUT_MS        100

#define TAS5805M_REG_PAGE              0x00
#define TAS5805M_REG_BOOK              0x7F
#define TAS5805M_REG_DEVICE_CTRL_1     0x02
#define TAS5805M_REG_DEVICE_CTRL_2     0x03
#define TAS5805M_REG_DIGITAL_VOLUME    0x4C
#define TAS5805M_REG_ANALOG_GAIN       0x54

#define TAS5805M_STATE_HIZ             0x02
#define TAS5805M_STATE_PLAY            0x03

#define TAS5805M_VOLUME_0DB            48U
#define TAS5805M_VOLUME_MUTE           255U

static bool i2c_initialized = false;
static bool amp_ready = false;
static uint8_t cached_volume_percent = 0;
static SemaphoreHandle_t amp_mutex = NULL;
static i2c_master_bus_handle_t i2c_bus = NULL;
static i2c_master_dev_handle_t tas5805m_dev = NULL;

static esp_err_t tas5805m_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    if (tas5805m_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = i2c_master_transmit(
        tas5805m_dev,
        data,
        sizeof(data),
        TAS5805M_I2C_TIMEOUT_MS);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed: addr=0x%02X reg=0x%02X value=0x%02X (%s)",
                 CONFIG_LOUDER_TAS5805M_I2C_ADDR, reg, value, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t tas5805m_select_book0_page0(void)
{
    esp_err_t ret = tas5805m_write_reg(TAS5805M_REG_PAGE, 0x00);
    if (ret != ESP_OK) return ret;
    ret = tas5805m_write_reg(TAS5805M_REG_BOOK, 0x00);
    if (ret != ESP_OK) return ret;
    return tas5805m_write_reg(TAS5805M_REG_PAGE, 0x00);
}

/*
 * Flat stereo startup sequence for TAS5805M.
 * It intentionally leaves the DSP enabled (DEVICE_CTRL_2 bit 4 clear) and
 * configures the normal BTL stereo path used by Louder ESP32 Rev H6.
 * I2S clocks MUST already be present when this sequence is executed.
 */
static esp_err_t tas5805m_load_flat_dsp_startup(void)
{
    esp_err_t ret;

#define WR(reg, value) do { \
        ret = tas5805m_write_reg((reg), (value)); \
        if (ret != ESP_OK) return ret; \
    } while (0)

    WR(0x00, 0x00);
    WR(0x7F, 0x00);
    WR(0x03, 0x02);       /* Hi-Z while resetting/configuring DSP */
    WR(0x01, 0x11);       /* software reset / DSP reset sequence */
    WR(0x03, 0x02);
    vTaskDelay(pdMS_TO_TICKS(10));

    WR(0x03, 0x00);
    WR(0x46, 0x01);
    WR(0x03, 0x02);
    WR(0x61, 0x0B);
    WR(0x60, 0x01);
    WR(0x7D, 0x11);
    WR(0x7E, 0xFF);
    WR(0x00, 0x01);
    WR(0x51, 0x05);

    WR(0x00, 0x00);
    WR(0x7F, 0x00);
    WR(TAS5805M_REG_DEVICE_CTRL_1, 0x00); /* Stereo BTL, normal modulation */
    WR(0x30, 0x00);
    // Start muted; the cached potentiometer value is applied before audio data
    // callbacks are enabled.
    WR(TAS5805M_REG_DIGITAL_VOLUME, TAS5805M_VOLUME_MUTE);
    WR(0x53, 0x00);
    WR(TAS5805M_REG_ANALOG_GAIN, (uint8_t)CONFIG_LOUDER_ANALOG_GAIN_STEPS);
    WR(TAS5805M_REG_DEVICE_CTRL_2, TAS5805M_STATE_PLAY);
    WR(0x78, 0x80);

#undef WR
    return ESP_OK;
}

static uint8_t logical_percent_to_native_volume(uint8_t percent)
{
    if (percent == 0) {
        return TAS5805M_VOLUME_MUTE;
    }
    if (percent >= 100) {
        return TAS5805M_VOLUME_0DB;
    }

    /*
     * Convert linear amplitude to dB so hardware DSP volume matches the old
     * software multiplication: dB = 20*log10(percent/100).
     * Native code 48 is 0 dB and each increment is -0.5 dB.
     */
    float linear = (float)percent / 100.0f;
    float db = 20.0f * log10f(linear);
    long native = lroundf((float)TAS5805M_VOLUME_0DB - (2.0f * db));

    if (native < TAS5805M_VOLUME_0DB) native = TAS5805M_VOLUME_0DB;
    if (native > 254) native = 254;
    return (uint8_t)native;
}

static esp_err_t tas5805m_apply_cached_volume_locked(void)
{
    uint8_t native = logical_percent_to_native_volume(cached_volume_percent);
    esp_err_t ret = tas5805m_select_book0_page0();
    if (ret != ESP_OK) return ret;

    ret = tas5805m_write_reg(TAS5805M_REG_DIGITAL_VOLUME, native);
    if (ret == ESP_OK) {
        float db = (native == TAS5805M_VOLUME_MUTE)
                     ? -120.0f
                     : -0.5f * ((float)native - (float)TAS5805M_VOLUME_0DB);
        ESP_LOGI(TAG, "DSP volume: %u%% -> reg 0x%02X (%.1f dB%s)",
                 cached_volume_percent, native, db,
                 native == TAS5805M_VOLUME_MUTE ? ", mute" : "");
    }
    return ret;
}

esp_err_t louder_tas5805m_prepare(void)
{
    if (amp_mutex == NULL) {
        amp_mutex = xSemaphoreCreateMutex();
        if (amp_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (i2c_initialized) {
        return ESP_OK;
    }

    gpio_config_t pwdn_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_LOUDER_TAS5805M_PWDN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pwdn_cfg), TAG, "Failed to configure PWDN GPIO");
    gpio_set_level(CONFIG_LOUDER_TAS5805M_PWDN_GPIO, 0);

    gpio_config_t fault_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_LOUDER_TAS5805M_FAULT_GPIO),
        .mode = GPIO_MODE_INPUT,
        // GPIO34 is input-only on ESP32 and has no internal pull resistor.
        // Louder H6 provides the required board-level FAULT# wiring.
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&fault_cfg), TAG, "Failed to configure FAULT GPIO");

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = CONFIG_LOUDER_TAS5805M_SCL_GPIO,
        .sda_io_num = CONFIG_LOUDER_TAS5805M_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "I2C master bus init failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_LOUDER_TAS5805M_I2C_ADDR,
        .scl_speed_hz = CONFIG_LOUDER_TAS5805M_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &tas5805m_dev),
                        TAG, "Failed to add TAS5805M I2C device");

    i2c_initialized = true;
    ESP_LOGI(TAG, "Louder H6 control prepared: I2C SDA=%d SCL=%d, PWDN=%d, FAULT=%d, TAS5805M=0x%02X",
             CONFIG_LOUDER_TAS5805M_SDA_GPIO,
             CONFIG_LOUDER_TAS5805M_SCL_GPIO,
             CONFIG_LOUDER_TAS5805M_PWDN_GPIO,
             CONFIG_LOUDER_TAS5805M_FAULT_GPIO,
             CONFIG_LOUDER_TAS5805M_I2C_ADDR);
    return ESP_OK;
}

esp_err_t louder_tas5805m_start(void)
{
    esp_err_t ret = louder_tas5805m_prepare();
    if (ret != ESP_OK) return ret;

    if (xSemaphoreTake(amp_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    amp_ready = false;

    /* Hardware power-cycle. I2S is already enabled by the caller. */
    gpio_set_level(CONFIG_LOUDER_TAS5805M_PWDN_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(CONFIG_LOUDER_TAS5805M_PWDN_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Give the already-enabled I2S clocks additional time to settle. */
    vTaskDelay(pdMS_TO_TICKS(10));

    ret = i2c_master_probe(i2c_bus,
                           CONFIG_LOUDER_TAS5805M_I2C_ADDR,
                           TAS5805M_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TAS5805M not responding at I2C address 0x%02X: %s",
                 CONFIG_LOUDER_TAS5805M_I2C_ADDR, esp_err_to_name(ret));
        gpio_set_level(CONFIG_LOUDER_TAS5805M_PWDN_GPIO, 0);
        xSemaphoreGive(amp_mutex);
        return ret;
    }

    ret = tas5805m_load_flat_dsp_startup();
    if (ret == ESP_OK) {
        amp_ready = true;
        ret = tas5805m_apply_cached_volume_locked();
    }

    if (ret == ESP_OK) {
        int fault_level = gpio_get_level(CONFIG_LOUDER_TAS5805M_FAULT_GPIO);
        ESP_LOGI(TAG, "TAS5805M DSP ready, stereo BTL PLAY, FAULT#=%d", fault_level);
        if (fault_level == 0) {
            ESP_LOGW(TAG, "TAS5805M FAULT# is active (LOW). Check speaker wiring and PVDD.");
        }
    } else {
        gpio_set_level(CONFIG_LOUDER_TAS5805M_PWDN_GPIO, 0);
        amp_ready = false;
        ESP_LOGE(TAG, "TAS5805M startup failed; amplifier kept in power-down");
    }

    xSemaphoreGive(amp_mutex);
    return ret;
}

esp_err_t louder_tas5805m_stop(void)
{
    if (!i2c_initialized || amp_mutex == NULL) {
        return ESP_OK;
    }

    if (xSemaphoreTake(amp_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    if (amp_ready) {
        ret = tas5805m_select_book0_page0();
        if (ret == ESP_OK) {
            ret = tas5805m_write_reg(TAS5805M_REG_DEVICE_CTRL_2, TAS5805M_STATE_HIZ);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    gpio_set_level(CONFIG_LOUDER_TAS5805M_PWDN_GPIO, 0);
    amp_ready = false;
    xSemaphoreGive(amp_mutex);

    ESP_LOGI(TAG, "TAS5805M stopped and PWDN asserted");
    return ret;
}

esp_err_t louder_tas5805m_set_volume_percent(uint8_t volume_percent)
{
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    cached_volume_percent = volume_percent;

    if (!amp_ready || amp_mutex == NULL) {
        return ESP_OK;
    }

    if (xSemaphoreTake(amp_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = tas5805m_apply_cached_volume_locked();
    xSemaphoreGive(amp_mutex);
    return ret;
}

uint8_t louder_tas5805m_get_volume_percent(void)
{
    return cached_volume_percent;
}

bool louder_tas5805m_is_ready(void)
{
    return amp_ready;
}
