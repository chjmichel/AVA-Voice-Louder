#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_task_wdt.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/uart.h"
#include "linenoise/linenoise.h"
#include "argtable3/argtable3.h"
#include "bt_app_hf.h"
#include "volume_control.h"
#include "louder_tas5805m.h"

static const char *TAG = "MAIN";

// NVS keys
#define NVS_NAMESPACE "amped_cfg"
#define NVS_KEY_BT_NAME "bt_name"

// Default Bluetooth device name to connect to
#define DEFAULT_BT_DEVICE_NAME ""  // Empty means connect to any device

/**
 * @brief Load Bluetooth device name from NVS
 * 
 * @param device_name Buffer to store device name
 * @param max_len Maximum length of buffer
 * @return esp_err_t ESP_OK on success
 */
static esp_err_t load_bt_device_name(char *device_name, size_t max_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    
    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found, using default");
        strncpy(device_name, DEFAULT_BT_DEVICE_NAME, max_len);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    
    size_t required_size = max_len;
    ret = nvs_get_str(nvs_handle, NVS_KEY_BT_NAME, device_name, &required_size);
    nvs_close(nvs_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BT name not found in NVS, using default");
        strncpy(device_name, DEFAULT_BT_DEVICE_NAME, max_len);
        return ret;
    }
    
    ESP_LOGI(TAG, "Loaded BT device name from NVS: %s", device_name);
    return ESP_OK;
}

/**
 * @brief Console command: set target device name
 */
static int setdevice_cmd(int argc, char **argv)
{
    if (argc < 2) {
        const char *current = bt_app_hf_get_target_device_name();
        printf("Current target device: %s\n", current ? current : "(none)");
        printf("Usage: setdevice <device_name>\n");
        printf("Example: setdevice My Headset\n");
        printf("Note: Restart discovery after changing device name\n");
        return 0;
    }
    
    // Concatenate all arguments to support device names with spaces
    char device_name[256] = {0};
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            strcat(device_name, " ");
        }
        strcat(device_name, argv[i]);
    }
    
    bt_app_hf_set_target_device_name(device_name);
    printf("Target device set to: %s\n", device_name);
    printf("Discovery will now look for this device\n");
    
    return 0;
}

/**
 * @brief Initialize console
 */
static void initialize_console(void)
{
    // Disable buffering on stdin
    setvbuf(stdin, NULL, _IONBF, 0);

    // Initialize the console
    esp_console_config_t console_config = {
        .max_cmdline_args = 8,
        .max_cmdline_length = 256,
    };
    ESP_ERROR_CHECK(esp_console_init(&console_config));

    // Configure linenoise
    linenoiseSetMultiLine(0);  // Disable multi-line to avoid terminal issues
    linenoiseSetCompletionCallback(NULL);
    linenoiseSetHintsCallback(NULL);
    linenoiseSetDumbMode(1);  // Use dumb mode to avoid ANSI escape codes

    const esp_console_cmd_t setdevice_command = {
        .command = "setdevice",
        .help = "Set target Bluetooth device (usage: setdevice <name>)",
        .hint = NULL,
        .func = &setdevice_cmd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&setdevice_command));

    ESP_LOGI(TAG, "Console initialized. Commands: setdevice");
}

/**
 * @brief Console task
 */
static void console_task(void *arg)
{
    const char* prompt = "esp32> ";
    
    // Remove this task from watchdog - it intentionally blocks waiting for user input
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (esp_task_wdt_delete(task) == ESP_OK) {
        ESP_LOGI(TAG, "Console task removed from watchdog");
    }
    
    while (1) {
        char* line = linenoise(prompt);
        if (line == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Add to history
        linenoiseHistoryAdd(line);

        // Execute command
        int ret;
        esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("Unrecognized command\n");
        } else if (err == ESP_ERR_INVALID_ARG) {
            // Command was empty
        } else if (err == ESP_OK && ret != ESP_OK) {
            printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        } else if (err != ESP_OK) {
            printf("Internal error: %s\n", esp_err_to_name(err));
        }

        linenoiseFree(line);
    }
}

/**
 * @brief Save Bluetooth device name to NVS
 * 
 * @param device_name Device name to save
 * @return esp_err_t ESP_OK on success
 */
__attribute__((unused)) static esp_err_t save_bt_device_name(const char *device_name)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS handle");
        return ret;
    }
    
    ret = nvs_set_str(nvs_handle, NVS_KEY_BT_NAME, device_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set BT name in NVS");
        nvs_close(nvs_handle);
        return ret;
    }
    
    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "BT device name saved to NVS: %s", device_name);
    }
    
    return ret;
}

void app_main(void)
{
    esp_err_t ret;
    char bt_device_name[64] = {0};
    
    ESP_LOGI(TAG, "ESP32 HFP Audio Gateway - Louder ESP32 Rev H6 / TAS5805M");
    
    // Initialize NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Prepare Louder H6 amplifier control early and keep TAS5805M in PWDN.
    // Actual DSP initialization is performed only after I2S clocks are running.
    ESP_LOGI(TAG, "Preparing Louder H6 TAS5805M control...");
    ret = louder_tas5805m_prepare();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to prepare TAS5805M control: %s", esp_err_to_name(ret));
        return;
    }
    
    // Initialize Bluetooth controller
    ESP_LOGI(TAG, "Initializing Bluetooth controller...");
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth controller init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth controller enable failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Initialize Bluedroid
    ESP_LOGI(TAG, "Initializing Bluedroid...");
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Priority: 1) CONFIG_HEADSET_NAME (new) 2) NVS 3) legacy config
    #ifdef CONFIG_HEADSET_NAME
    strncpy(bt_device_name, CONFIG_HEADSET_NAME, sizeof(bt_device_name) - 1);
    ESP_LOGI(TAG, "Target headset from Kconfig: '%s'", bt_device_name);
    #endif
    
    // NVS can override the Kconfig setting if configured
    char nvs_device_name[64] = {0};
    if (load_bt_device_name(nvs_device_name, sizeof(nvs_device_name)) == ESP_OK) {
        if (strlen(nvs_device_name) > 0) {
            strncpy(bt_device_name, nvs_device_name, sizeof(bt_device_name) - 1);
            ESP_LOGI(TAG, "Target device overridden by NVS: '%s'", bt_device_name);
        }
    }
    
    ESP_LOGI(TAG, "Initializing Bluetooth HFP...");
    ret = bt_app_hf_init(strlen(bt_device_name) > 0 ? bt_device_name : NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Bluetooth HFP: %s", esp_err_to_name(ret));
        return;
    }
    
    // Initialize volume control
    ESP_LOGI(TAG, "Initializing potentiometer volume control...");
    ret = volume_control_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize volume control: %s", esp_err_to_name(ret));
    } else {
        ret = volume_control_start_monitoring();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start volume monitoring: %s", esp_err_to_name(ret));
        }
    }

    /*
     * Start the local audio engine independently of Bluetooth/HFP.
     * Samsung -> CM108B audio must work even with no headset connected.
     */
    ESP_LOGI(TAG, "Starting central audio engine (CM108B + mixer + TAS5805M)...");
    ret = bt_app_audio_engine_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start central audio engine: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "Starting Bluetooth discovery...");
    ret = bt_app_hf_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Bluetooth discovery: %s", esp_err_to_name(ret));
        return;
    }
    
    if (strlen(bt_device_name) > 0) {
        ESP_LOGI(TAG, "System ready. Searching for Bluetooth device: %s", bt_device_name);
    } else {
        ESP_LOGI(TAG, "System ready. Will connect to any HFP device found.");
    }
    
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "ESP32 HFP AG is ready!");
    ESP_LOGI(TAG, "Waiting for Bluetooth connection...");
    ESP_LOGI(TAG, "====================================");
    
    // Initialize and start console in a separate task
    // Note: We don't initialize the console here to avoid watchdog issues
    // The console blocks on linenoise() which triggers the watchdog
    
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "System is running!");
    ESP_LOGI(TAG, "Use 'setdevice <name>' via serial to change target");
    ESP_LOGI(TAG, "Current target: %s", strlen(bt_device_name) > 0 ? bt_device_name : "(any device)");
    ESP_LOGI(TAG, "====================================");
    
    // Main loop - just monitor the system
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        // You can add status monitoring here if needed
    }
}
