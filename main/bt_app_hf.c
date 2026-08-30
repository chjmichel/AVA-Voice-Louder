#include "bt_app_hf.h"
#include "volume_control.h"
#include "louder_tas5805m.h"
#include "notification_sound.h"
#include "audio_mixer.h"
#include "bt_device_memory.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_ag_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_console.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "BT_HF";

#define RESET_HOLD_TIME_MS 5000  // 5 seconds

#if CONFIG_ENABLE_HEADSET_RESET_GPIO
#define RESET_GPIO_PIN ((gpio_num_t)CONFIG_HEADSET_RESET_GPIO)
#if CONFIG_HEADSET_RESET_ACTIVE_HIGH
#define RESET_ACTIVE_LEVEL 1
#define RESET_INACTIVE_LEVEL 0
#else
#define RESET_ACTIVE_LEVEL 0
#define RESET_INACTIVE_LEVEL 1
#endif
#endif

// Target device name from Kconfig
#ifdef CONFIG_HEADSET_NAME
static const char *target_device_name = CONFIG_HEADSET_NAME;
#else
static const char *target_device_name = "";
#endif

// Legacy target device name filter (NULL means connect to any device)
static char *runtime_target_device_name = NULL;

// Bluetooth address of connected device
esp_bd_addr_t peer_bd_addr;
bool is_connected = false;
bool is_connecting = false;
static uint8_t audio_retry_count = 0;
#define MAX_AUDIO_RETRIES 3

// Codec negotiation tracking to prevent infinite loops
static uint32_t codec_negotiation_count = 0;
static TickType_t last_codec_negotiation_time = 0;
#define MAX_CODEC_NEGOTIATIONS 20  // Stop after 20 attempts
#define CODEC_NEGOTIATION_TIMEOUT_MS 10000  // Reset counter after 10 seconds

// Last negotiated HFP codec (ESP_HF_WBS_NONE/NO/YES).
// mSBC/WBS is preferred for best HFP speech quality; CVSD remains the
// standards-compliant fallback when the headset cannot negotiate WBS.
static esp_hf_wbs_config_t negotiated_mode = ESP_HF_WBS_NONE;
#define HFP_CVSD_SAMPLE_RATE 8000U
#define HFP_MSBC_SAMPLE_RATE 16000U
static volatile uint32_t hfp_input_sample_rate = HFP_CVSD_SAMPLE_RATE;

// Play the AVA brand jingle once per full HFP/SLC connection.
// A SCO retry within the same SLC session must not replay the brand song.
static bool connection_jingle_pending = false;

static const char *bt_app_hfp_codec_name(esp_hf_wbs_config_t mode)
{
    switch (mode) {
    case ESP_HF_WBS_YES:
        return "mSBC/WBS";
    case ESP_HF_WBS_NO:
        return "CVSD";
    case ESP_HF_WBS_NONE:
    default:
        return "not negotiated";
    }
}

// Track if audio connection was requested for current codec
static bool audio_connect_requested = false;

// Connection manager: keep Bluetooth callbacks non-blocking and centralize retries.
#define DIRECT_CONNECT_TIMEOUT_MS 5000
#define DIRECT_RETRY_DELAY_MS 600
#define MAX_DIRECT_RETRIES 3
#define DISCOVERY_INQ_LEN 4              // 4 * 1.28 s = 5.12 s inquiry window
#define DISCOVERY_RESTART_DELAY_MS 1000
#define AUDIO_SETUP_DELAY_MS 300
#define AUDIO_RETRY_DELAY_MS 1000

typedef enum {
    BT_MGR_EVT_TRY_SAVED = 1,
    BT_MGR_EVT_CONNECT_TIMEOUT,
    BT_MGR_EVT_START_DISCOVERY,
    BT_MGR_EVT_RECONNECT,
    BT_MGR_EVT_AUDIO_SETUP,
    BT_MGR_EVT_AUDIO_RETRY,
} bt_mgr_event_t;

static QueueHandle_t bt_mgr_queue = NULL;
static TaskHandle_t bt_mgr_task_handle = NULL;
static esp_timer_handle_t saved_mac_timer = NULL;
static esp_timer_handle_t reconnect_timer = NULL;
static esp_timer_handle_t discovery_timer = NULL;
static esp_timer_handle_t audio_setup_timer = NULL;
static esp_timer_handle_t audio_retry_timer = NULL;
static bool trying_saved_mac = false;
static uint8_t direct_retry_count = 0;
static bool suppress_disconnect_reconnect_once = false;

// Forward declarations
static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
static void bt_app_mgr_task(void *arg);
static void bt_app_timer_event_cb(void *arg);
static void bt_app_post_event(bt_mgr_event_t event);
static void bt_app_start_one_shot(esp_timer_handle_t timer, uint32_t delay_ms);

static void bt_app_post_event(bt_mgr_event_t event)
{
    if (bt_mgr_queue == NULL) {
        return;
    }
    if (xQueueSend(bt_mgr_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "BT manager queue full, dropping event %d", event);
    }
}

static void bt_app_timer_event_cb(void *arg)
{
    bt_app_post_event((bt_mgr_event_t)(intptr_t)arg);
}

static void bt_app_start_one_shot(esp_timer_handle_t timer, uint32_t delay_ms)
{
    if (timer == NULL) {
        return;
    }
    esp_timer_stop(timer);
    esp_err_t ret = esp_timer_start_once(timer, (uint64_t)delay_ms * 1000ULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start timer: %s", esp_err_to_name(ret));
    }
}

static esp_err_t bt_app_create_event_timer(esp_timer_handle_t *handle,
                                           const char *name,
                                           bt_mgr_event_t event)
{
    esp_timer_create_args_t args = {
        .callback = bt_app_timer_event_cb,
        .arg = (void *)(intptr_t)event,
        .name = name,
    };
    return esp_timer_create(&args, handle);
}

static esp_err_t bt_app_connection_manager_init(void)
{
    if (bt_mgr_queue != NULL) {
        return ESP_OK;
    }

    bt_mgr_queue = xQueueCreate(12, sizeof(bt_mgr_event_t));
    if (bt_mgr_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (bt_app_create_event_timer(&saved_mac_timer, "bt_conn_timeout", BT_MGR_EVT_CONNECT_TIMEOUT) != ESP_OK ||
        bt_app_create_event_timer(&reconnect_timer, "bt_reconnect", BT_MGR_EVT_RECONNECT) != ESP_OK ||
        bt_app_create_event_timer(&discovery_timer, "bt_discovery", BT_MGR_EVT_START_DISCOVERY) != ESP_OK ||
        bt_app_create_event_timer(&audio_setup_timer, "bt_audio_setup", BT_MGR_EVT_AUDIO_SETUP) != ESP_OK ||
        bt_app_create_event_timer(&audio_retry_timer, "bt_audio_retry", BT_MGR_EVT_AUDIO_RETRY) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create Bluetooth manager timers");
        return ESP_FAIL;
    }

    if (xTaskCreate(bt_app_mgr_task, "bt_conn_mgr", 4096, NULL, 5, &bt_mgr_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Bluetooth connection manager task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Bluetooth connection manager initialized");
    return ESP_OK;
}

static void bt_app_mgr_task(void *arg)
{
    bt_mgr_event_t event;

    while (xQueueReceive(bt_mgr_queue, &event, portMAX_DELAY) == pdTRUE) {
        switch (event) {
        case BT_MGR_EVT_TRY_SAVED: {
            if (is_connected || is_connecting) {
                break;
            }

            uint8_t saved_mac[6];
            char saved_name[64] = {0};
            if (!bt_device_memory_has_saved_device() ||
                bt_device_memory_load_device(saved_mac, saved_name) != ESP_OK) {
                bt_app_post_event(BT_MGR_EVT_START_DISCOVERY);
                break;
            }

            memcpy(peer_bd_addr, saved_mac, ESP_BD_ADDR_LEN);
            trying_saved_mac = true;
            is_connecting = true;
            direct_retry_count++;

            ESP_LOGI(TAG, "[AUTO-CONNECT] Direct MAC attempt %u/%u to %02X:%02X:%02X:%02X:%02X:%02X",
                     direct_retry_count, MAX_DIRECT_RETRIES,
                     saved_mac[0], saved_mac[1], saved_mac[2],
                     saved_mac[3], saved_mac[4], saved_mac[5]);

            esp_err_t ret = esp_hf_ag_slc_connect(peer_bd_addr);
            if (ret == ESP_OK) {
                bt_app_start_one_shot(saved_mac_timer, DIRECT_CONNECT_TIMEOUT_MS);
            } else {
                ESP_LOGW(TAG, "[AUTO-CONNECT] SLC request failed: %s", esp_err_to_name(ret));
                is_connecting = false;
                trying_saved_mac = false;
                if (direct_retry_count < MAX_DIRECT_RETRIES) {
                    bt_app_start_one_shot(reconnect_timer, DIRECT_RETRY_DELAY_MS);
                } else {
                    bt_app_post_event(BT_MGR_EVT_START_DISCOVERY);
                }
            }
            break;
        }

        case BT_MGR_EVT_CONNECT_TIMEOUT:
            if (trying_saved_mac && !is_connected) {
                ESP_LOGW(TAG, "[AUTO-CONNECT] Direct MAC attempt timed out after %d ms", DIRECT_CONNECT_TIMEOUT_MS);
                // Ask the stack to abort a potentially half-open attempt before retrying.
                // The resulting DISCONNECTED event must not schedule an additional reconnect.
                suppress_disconnect_reconnect_once = true;
                esp_hf_ag_slc_disconnect(peer_bd_addr);
                is_connecting = false;
                trying_saved_mac = false;

                if (direct_retry_count < MAX_DIRECT_RETRIES) {
                    bt_app_start_one_shot(reconnect_timer, DIRECT_RETRY_DELAY_MS);
                } else {
                    ESP_LOGW(TAG, "[AUTO-CONNECT] Direct retries exhausted; falling back to short discovery");
                    bt_app_post_event(BT_MGR_EVT_START_DISCOVERY);
                }
            }
            break;

        case BT_MGR_EVT_RECONNECT:
            if (!is_connected && !is_connecting) {
                bt_app_post_event(BT_MGR_EVT_TRY_SAVED);
            }
            break;

        case BT_MGR_EVT_START_DISCOVERY:
            if (is_connected || is_connecting) {
                break;
            }
            trying_saved_mac = false;
            ESP_LOGI(TAG, "Starting short device discovery (%0.2f s max)...", DISCOVERY_INQ_LEN * 1.28f);
            {
                esp_err_t ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, DISCOVERY_INQ_LEN, 0);
                if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "Failed to start discovery: %s", esp_err_to_name(ret));
                    bt_app_start_one_shot(discovery_timer, DISCOVERY_RESTART_DELAY_MS);
                }
            }
            break;

        case BT_MGR_EVT_AUDIO_SETUP:
            if (is_connected && !audio_connect_requested) {
                esp_hf_ag_ciev_report(peer_bd_addr, ESP_HF_IND_TYPE_CALL, 1);
                esp_hf_ag_ciev_report(peer_bd_addr, ESP_HF_IND_TYPE_CALLSETUP, 0);
                // Do not force a codec here. With CONFIG_BT_HFP_WBS_ENABLE=y,
                // Bluedroid negotiates mSBC when the peer supports it and
                // automatically falls back to CVSD otherwise.
                ESP_LOGI(TAG, "Call active; requesting HFP audio (mSBC preferred, CVSD fallback)");
                audio_retry_count = 1;
                audio_connect_requested = true;
                esp_err_t ret = esp_hf_ag_audio_connect(peer_bd_addr);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to request audio connection: %s", esp_err_to_name(ret));
                    audio_connect_requested = false;
                    bt_app_start_one_shot(audio_retry_timer, AUDIO_RETRY_DELAY_MS);
                }
            }
            break;

        case BT_MGR_EVT_AUDIO_RETRY:
            if (is_connected && !audio_connect_requested && audio_retry_count < MAX_AUDIO_RETRIES) {
                audio_retry_count++;
                ESP_LOGI(TAG, "Retrying audio connection (%u/%u)", audio_retry_count, MAX_AUDIO_RETRIES);
                audio_connect_requested = true;
                esp_err_t ret = esp_hf_ag_audio_connect(peer_bd_addr);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Audio retry request failed: %s", esp_err_to_name(ret));
                    audio_connect_requested = false;
                    if (audio_retry_count < MAX_AUDIO_RETRIES) {
                        bt_app_start_one_shot(audio_retry_timer, AUDIO_RETRY_DELAY_MS);
                    }
                }
            }
            break;
        }
    }
}

/**
 * @brief Print saved Bluetooth device MAC address from permanent memory
 */
static void bt_app_print_saved_device(void)
{
    uint8_t saved_mac[6];
    char saved_name[64] = {0};
    
    if (bt_device_memory_has_saved_device()) {
        if (bt_device_memory_load_device(saved_mac, saved_name) == ESP_OK) {
            ESP_LOGI(TAG, "╔════════════════════════════════════════════════════════════╗");
            ESP_LOGI(TAG, "║        SAVED BLUETOOTH DEVICE (from permanent memory)     ║");
            ESP_LOGI(TAG, "╠════════════════════════════════════════════════════════════╣");
            ESP_LOGI(TAG, "║ Device Name: %-47s ║", saved_name);
            ESP_LOGI(TAG, "║ MAC Address: %02X:%02X:%02X:%02X:%02X:%02X                           ║",
                     saved_mac[0], saved_mac[1], saved_mac[2],
                     saved_mac[3], saved_mac[4], saved_mac[5]);
            ESP_LOGI(TAG, "║ Status: Ready for fast reconnection                       ║");
            ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════╝");
            
            return;
        }
    }
    
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║        SAVED BLUETOOTH DEVICE (from permanent memory)     ║");
    ESP_LOGI(TAG, "╠════════════════════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║ Status: No saved device found                             ║");
    ESP_LOGI(TAG, "║ Action: Connect to a Bluetooth device to save its MAC     ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════╝");
}

/**
 * @brief Perform the actual MAC address reset
 */
static void bt_app_perform_reset(void)
{
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║              HEADSET RESET SEQUENCE INITIATED              ║");
    ESP_LOGI(TAG, "╠════════════════════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║ Clearing saved MAC address from permanent memory...       ║");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════╝");
    
    esp_err_t ret = bt_device_memory_clear_device();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ Saved MAC address cleared successfully!");
        ESP_LOGI(TAG, "   Next boot will require device discovery");
    } else {
        ESP_LOGE(TAG, "❌ Failed to clear MAC address: %s", esp_err_to_name(ret));
    }
}

/**
 * @brief Optional external long-press reset for saved headset MAC.
 *
 * Louder ESP32 Rev H6 reserves GPIO12 for the RGB LED, so the old hard-coded
 * GPIO12 reset input is disabled by default. The serial `headset-reset` command
 * is always available.
 */
static void bt_app_check_reset_gpio(void)
{
#if CONFIG_ENABLE_HEADSET_RESET_GPIO
    ESP_LOGI(TAG, "Checking GPIO%d for headset reset (hold for 5 seconds)...",
             CONFIG_HEADSET_RESET_GPIO);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RESET_GPIO_PIN),
        .mode = GPIO_MODE_INPUT,
#if CONFIG_HEADSET_RESET_ACTIVE_HIGH
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#else
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    if (gpio_get_level(RESET_GPIO_PIN) != RESET_ACTIVE_LEVEL) {
        return;
    }

    ESP_LOGI(TAG, "Headset reset input active; keep held for 5 seconds...");
    uint32_t hold_duration_ms = 0;
    while (hold_duration_ms < RESET_HOLD_TIME_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        hold_duration_ms += 100;
        if (gpio_get_level(RESET_GPIO_PIN) == RESET_INACTIVE_LEVEL) {
            ESP_LOGI(TAG, "Headset reset input released - reset cancelled");
            return;
        }
    }

    if (gpio_get_level(RESET_GPIO_PIN) == RESET_ACTIVE_LEVEL) {
        bt_app_perform_reset();
    }
#else
    ESP_LOGI(TAG, "External headset reset GPIO disabled (H6-safe); use console command 'headset-reset'");
#endif
}

/**
 * @brief Console command handler for "headset-reset"
 */
static int bt_app_headset_reset_cmd(int argc, char **argv)
{
    ESP_LOGI(TAG, "");
    bt_app_perform_reset();
    ESP_LOGI(TAG, "");
    return 0;
}

/**
 * @brief Register console command for headset reset
 */
static void bt_app_register_reset_command(void)
{
    const esp_console_cmd_t reset_cmd = {
        .command = "headset-reset",
        .help = "Clear saved Bluetooth headset MAC address from permanent memory",
        .hint = NULL,
        .func = &bt_app_headset_reset_cmd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reset_cmd));
}

const char* bt_app_hf_get_target_device_name(void)
{
    // Return runtime target if set, otherwise return config target
    if (runtime_target_device_name) {
        return runtime_target_device_name;
    }
    return target_device_name;
}

void bt_app_hf_set_target_device_name(const char *device_name)
{
    if (runtime_target_device_name) {
        free(runtime_target_device_name);
        runtime_target_device_name = NULL;
    }
    
    if (device_name && strlen(device_name) > 0) {
        runtime_target_device_name = strdup(device_name);
        ESP_LOGI(TAG, "Runtime target device name set to: %s", runtime_target_device_name);
    } else {
        ESP_LOGI(TAG, "Runtime target device name cleared (will use config: %s)", target_device_name);
    }
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        // Get Bluetooth address
        char bda_str[18];
        uint8_t *bda = param->disc_res.bda;
        sprintf(bda_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        
        ESP_LOGI(TAG, "Device found: %s", bda_str);
        
        // In ESP-IDF v5.3, EIR data is in the prop array
        // Extract device name from EIR data if available
        char device_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
        uint8_t *eir = NULL;
        
        // Find EIR data in properties
        for (int i = 0; i < param->disc_res.num_prop; i++) {
            if (param->disc_res.prop[i].type == ESP_BT_GAP_DEV_PROP_EIR) {
                eir = (uint8_t *)(param->disc_res.prop[i].val);
                break;
            }
        }
        
        if (eir) {
            // Parse EIR data for device name
            uint8_t name_len = 0;
            uint8_t *name_ptr = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &name_len);
            if (!name_ptr) {
                name_ptr = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &name_len);
            }
            if (name_ptr && name_len > 0) {
                // Limit copy to buffer size
                uint8_t copy_len = (name_len > ESP_BT_GAP_MAX_BDNAME_LEN) ? ESP_BT_GAP_MAX_BDNAME_LEN : name_len;
                memcpy(device_name, name_ptr, copy_len);
                device_name[copy_len] = '\0';
                ESP_LOGI(TAG, "Remote device name: %s", device_name);
            } else {
                ESP_LOGI(TAG, "No device name in EIR data");
            }
        }
        
        // Skip if already connected or connecting
        if (is_connected || is_connecting) {
            ESP_LOGI(TAG, "Already connected/connecting, skipping");
            break;
        }
        
        // Only connect if we have a device name from EIR
        if (strlen(device_name) == 0) {
            ESP_LOGD(TAG, "No device name available, skipping");
            break;
        }
        
        // Get the effective target device name (runtime or config)
        const char *effective_target = bt_app_hf_get_target_device_name();
        
        // Check if target device name is configured
        if (effective_target == NULL || strlen(effective_target) == 0) {
            ESP_LOGW(TAG, "No target device configured. Set HEADSET_NAME in menuconfig or use bt_app_hf_set_target_device_name()");
            break;
        }
        
        // Only connect if device name matches the configured target
        if (strcmp(device_name, effective_target) != 0) {
            ESP_LOGD(TAG, "Device '%s' doesn't match target '%s', skipping", device_name, effective_target);
            break;
        }
        
        // Store the address for connection
        memcpy(peer_bd_addr, param->disc_res.bda, ESP_BD_ADDR_LEN);
        
        // Target device found - initiate connection
        ESP_LOGI(TAG, "Target device '%s' found! Connecting...", effective_target);
        is_connecting = true;
        esp_bt_gap_cancel_discovery();
        esp_err_t connect_ret = esp_hf_ag_slc_connect(peer_bd_addr);
        if (connect_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to initiate SLC connection: %s", esp_err_to_name(connect_ret));
            is_connecting = false;
            bt_app_start_one_shot(discovery_timer, DISCOVERY_RESTART_DELAY_MS);
        }
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "Discovery stopped");
            if (!is_connected && !is_connecting) {
                ESP_LOGI(TAG, "Scheduling discovery restart...");
                bt_app_start_one_shot(discovery_timer, DISCOVERY_RESTART_DELAY_MS);
            } else if (is_connecting) {
                ESP_LOGI(TAG, "Connection in progress, waiting...");
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(TAG, "Discovery started");
        }
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Authentication success: %s", param->auth_cmpl.device_name);
            ESP_LOGI(TAG, "Authenticated device address: %02x:%02x:%02x:%02x:%02x:%02x",
                     param->auth_cmpl.bda[0], param->auth_cmpl.bda[1], param->auth_cmpl.bda[2],
                     param->auth_cmpl.bda[3], param->auth_cmpl.bda[4], param->auth_cmpl.bda[5]);
        } else {
            ESP_LOGE(TAG, "Authentication failed, status: %d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT:
        ESP_LOGI(TAG, "PIN code request for device: %s", param->pin_req.bda);
        ESP_LOGI(TAG, "Please enter PIN code 0000");
        esp_bt_pin_code_t pin_code = {0};
        pin_code[0] = '0';
        pin_code[1] = '0';
        pin_code[2] = '0';
        pin_code[3] = '0';
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP confirmation request for: %s, numeric value: %ld", 
                 param->cfm_req.bda, param->cfm_req.num_val);
        ESP_LOGI(TAG, "Auto-accepting pairing");
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey notification: %06ld", param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "SSP passkey request");
        break;
    default:
        ESP_LOGD(TAG, "GAP event: %d", event);
        break;
    }
}

#if CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI

#include "driver/i2s_std.h"
#if CONFIG_ENABLE_SD_RECORDING
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#endif

#define SAMPLE_RATE 48000  // Fixed high-quality mixer/output rate for Louder H6 / TAS5805M
#define I2S_PORT I2S_NUM_0 // I2S Peripheral
#define BUFFER_SIZE 512    // DMA Buffer Size

// Louder ESP32 Rev H6 I2S connections to TAS5805M
#define I2S_WS CONFIG_I2S_LRCLK_GPIO  // I2S LRCLK (Word Select)
#define I2S_BCK CONFIG_I2S_BCLK_GPIO  // I2S BCLK (Bit Clock)
#define I2S_DOUT CONFIG_I2S_DOUT_GPIO // I2S DOUT (Data Out to TAS5805M)

// I2S handle
i2s_chan_handle_t tx_chan;
static bool i2s_initialized = false;
static bool i2s_enabled = false;

#if CONFIG_ENABLE_SD_RECORDING
#define RINGBUF_SIZE (BUFFER_SIZE * 100) // 50 KB Buffer
static RingbufHandle_t audio_ringbuf = NULL;
static TaskHandle_t audio_record_task_handle = NULL;
static bool current_file_is_a = true;

#define MOUNT_POINT "/sdcard"
#define FILE_A_PATH "/sdcard/FileA.wav"
#define FILE_B_PATH "/sdcard/FileB.wav"
#define RECORD_DURATION (30 * 60) // 30 Minutes

// SD Card Variables
static sdmmc_card_t *card = NULL;
static uint32_t total_bytes = 0;
static FILE *record_file = NULL;
static uint64_t start_time = 0;
static const char mount_point[] = MOUNT_POINT;
static bool sdcard_initialized = false;

typedef struct
{
    char riff[4];             // "RIFF"
    uint32_t chunk_size;      // Total file size - 8 bytes
    char wave[4];             // "WAVE"
    char fmt[4];              // "fmt "
    uint32_t subchunk1_size;  // Size of FMT chunk (16 bytes)
    uint16_t audio_format;    // 1 = PCM
    uint16_t num_channels;    // 1 = Mono
    uint32_t sample_rate;     // 16000 Hz
    uint32_t byte_rate;       // sample_rate * num_channels * bits_per_sample / 8
    uint16_t block_align;     // num_channels * bits_per_sample / 8
    uint16_t bits_per_sample; // 16-bit
    char data[4];             // "data"
    uint32_t data_size;       // PCM Data Size
} wav_header_t;

void start_timer()
{
    start_time = esp_timer_get_time();
}

bool is_30_minutes_passed()
{
    uint64_t elapsed = (esp_timer_get_time() - start_time) / 1000000; // Convert to seconds
    return (elapsed >= RECORD_DURATION);
}

void wav_header_init(wav_header_t *header, uint32_t data_size, uint32_t sample_rate)
{
    memcpy(header->riff, "RIFF", 4);
    header->chunk_size = data_size + sizeof(wav_header_t) - 8; // File size - 8 bytes
    memcpy(header->wave, "WAVE", 4);
    memcpy(header->fmt, "fmt ", 4);
    header->subchunk1_size = 16;
    header->audio_format = 1;      // PCM Format
    header->num_channels = 1;      // Mono
    header->sample_rate = sample_rate;
    header->byte_rate = sample_rate * 2; // 16-bit mono
    header->block_align = 2;       // 2 bytes/sample
    header->bits_per_sample = 16;  // 16 bits per sample
    memcpy(header->data, "data", 4);
    header->data_size = data_size; // Final PCM Data Size
}

void finalize_wav_file(FILE *file, uint32_t total_bytes)
{
    rewind(file); // Go back to the start of the file

    wav_header_t wav_header;
    wav_header_init(&wav_header, total_bytes, hfp_input_sample_rate);
    fwrite(&wav_header, sizeof(wav_header_t), 1, file);

    ESP_LOGI("SDCARD", "WAV Header Updated: %ld bytes", total_bytes);
}

void open_new_file()
{
    if (record_file)
    {
        finalize_wav_file(record_file, total_bytes);
        fclose(record_file);
        record_file = NULL;
    }

    total_bytes = 0;

    if (current_file_is_a)
    {
        ESP_LOGI("SDCARD", "Opening File: %s", FILE_A_PATH);
        record_file = fopen(FILE_A_PATH, "wb");
        current_file_is_a = false;
    }
    else
    {
        ESP_LOGI("SDCARD", "Opening File: %s", FILE_B_PATH);
        record_file = fopen(FILE_B_PATH, "wb");
        current_file_is_a = true;
    }

    if (!record_file)
    {
        ESP_LOGE("SDCARD", "Failed to open file for writing.");
        return;
    }

    wav_header_t wav_header;
    wav_header_init(&wav_header, 0, hfp_input_sample_rate);
    fwrite(&wav_header, sizeof(wav_header), 1, record_file);
}

// SD Card Initialization
esp_err_t init_sdcard()
{
    ESP_LOGI("SDCARD", "Initializing SD Card...");
    esp_err_t ret;

    // Setting for my Board
    // gpio_reset_pin(GPIO_NUM_13);
    // gpio_set_direction(GPIO_NUM_13, GPIO_MODE_DISABLE);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#if CONFIG_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 5,
        .disk_status_check_enable = true,
        .allocation_unit_size = 16 * 1024};

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
#if CONFIG_SDMMC_BUS_WIDTH_1
    slot_config.width = 1;
#elif CONFIG_SDMMC_BUS_WIDTH_4
    slot_config.width = 4;
#endif
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI("SDCARD", "Mounting filesystem");
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE("SDCARD", "Failed to mount filesystem. If you want the card to be formatted, set the CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        }
        else
        {
            ESP_LOGE("SDCARD", "Failed to initialize the card (%s). Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return ret;
    }
    ESP_LOGI("SDCARD", "Filesystem mounted");

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);

    ESP_LOGI("SDCARD", "SD Card Mounted Successfully.");
    return ret;
}

#endif /* CONFIG_ENABLE_SD_RECORDING */

// Resample HFP mono PCM to the fixed 48 kHz TAS5805M stereo master rate.
// CVSD delivers 8 kHz mono, while mSBC/WBS delivers 16 kHz mono. Keeping
// the amplifier side fixed at 48 kHz avoids re-clocking the DSP when a future
// high-quality 48 kHz source is mixed with HFP speech.
void resample_linear_stereo(const int16_t *input, int input_size, uint32_t input_rate,
                            int32_t *output, int *output_size)
{
    if (input_rate == 0) {
        input_rate = HFP_CVSD_SAMPLE_RATE;
    }

    float ratio = (float)SAMPLE_RATE / (float)input_rate;
    int output_samples = (int)(input_size * ratio);

    for (int i = 0; i < output_samples; i++)
    {
        float index = i / ratio;
        int idx = (int)index;
        float frac = index - idx;

        int16_t sample;
        if (idx + 1 < input_size)
        {
            sample = (int16_t)((1.0f - frac) * input[idx] + frac * input[idx + 1]);
        }
        else
        {
            sample = input[idx]; // Edge case
        }
        
        // Convert 16-bit to 32-bit and duplicate for stereo (L+R channels)
        int32_t sample_32 = ((int32_t)sample) << 16; // Scale to 32-bit
        output[i * 2] = sample_32;     // Left channel
        output[i * 2 + 1] = sample_32; // Right channel
    }
    *output_size = output_samples * 2; // Stereo samples count
}

// Bluetooth Incoming Audio Callback Function - TAS5805M stereo output
void bt_app_hf_incoming_cb(const uint8_t *buf, uint32_t sz)
{
    const int16_t *input = (const int16_t *)buf;
    int input_size = sz / 2; // Convert bytes to samples

    // Volume is intentionally NOT applied to PCM samples here. Louder H6 uses
    // the TAS5805M DSP digital-volume register, avoiding double attenuation and
    // preserving the original HFP PCM data for optional recording.

    static int32_t resampled_buffer[BUFFER_SIZE * 6]; // Larger buffer for 32-bit stereo
    int output_size;

    const uint32_t input_rate = hfp_input_sample_rate;
    resample_linear_stereo(input, input_size, input_rate, resampled_buffer, &output_size);

    // HFP never writes directly to I2S. The central 48 kHz mixer is the only
    // I2S writer and combines this speech with the local brand jingle.
    esp_err_t mix_ret = audio_mixer_submit_hfp(resampled_buffer, (size_t)output_size / 2U);
    if (mix_ret != ESP_OK && mix_ret != ESP_ERR_TIMEOUT)
    {
        ESP_LOGW("AUDIO_MIXER", "Could not queue HFP PCM: %s", esp_err_to_name(mix_ret));
    }

    // ESP_LOGI("AUDIO", "Received %d bytes, Resampled to %d bytes", input_size * 2, output_size * 4);

#if CONFIG_ENABLE_SD_RECORDING
    if (sdcard_initialized && record_file)
    {
        BaseType_t result = xRingbufferSend(audio_ringbuf, (void *)buf, sz, pdMS_TO_TICKS(10));
        if (result != pdTRUE)
        {
            ESP_LOGW("RINGBUF", "Ringbuffer Overflow!");
        }
    }
#endif
}

// Bluetooth Outgoing Audio Callback Function - Not used in AG mode (we only receive from headset mic)
uint32_t bt_hf_audio_data_send_cb(uint8_t *data, uint32_t len)
{
    // In AG mode, we receive audio from the headset's microphone
    // We don't send audio back to the headset
    return 0;
}

#if CONFIG_ENABLE_SD_RECORDING
// Audio Record Task
void audio_record_task(void *arg)
{
    open_new_file();
    start_timer();
    total_bytes = 0;
    void *data = NULL;
    size_t size;
    vTaskDelay(pdMS_TO_TICKS(10));
    while (1)
    {
        data = xRingbufferReceive(audio_ringbuf, &size, pdMS_TO_TICKS(100));
        if (data != NULL)
        {
            fwrite(data, 1, size, record_file);
            total_bytes += size;
            // fflush(record_file);
            vRingbufferReturnItem(audio_ringbuf, data);
        }

        if (is_30_minutes_passed())
        {
            ESP_LOGI("SDCARD", "30 Minutes Passed. Rotating File...");
            open_new_file();
            start_timer();
        }
    }
}

#endif /* CONFIG_ENABLE_SD_RECORDING */

// Initialize I2S & Start Tasks
static esp_err_t bt_app_send_data(void)
{
    esp_err_t ret;

    // A duplicate CONNECTED event must never create a second writer or prime
    // I2S while the mixer already owns the channel.
    if (audio_mixer_is_running() && i2s_enabled && louder_tas5805m_is_ready()) {
        ESP_LOGD("AUDIO", "Audio mixer already running");
        return ESP_OK;
    }

    if (!i2s_initialized)
    {
        ESP_LOGI("I2S", "Initializing I2S for Louder H6 TAS5805M (32-bit stereo)...");
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
        ret = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE("I2S", "Failed to create I2S TX channel: %s", esp_err_to_name(ret));
            return ret;
        }

        i2s_std_config_t i2s_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .bclk = I2S_BCK,
                .ws = I2S_WS,
                .dout = I2S_DOUT,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };

        ret = i2s_channel_init_std_mode(tx_chan, &i2s_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE("I2S", "Failed to configure I2S standard mode: %s", esp_err_to_name(ret));
            i2s_del_channel(tx_chan);
            tx_chan = NULL;
            return ret;
        }

        i2s_initialized = true;
        ESP_LOGI("I2S", "TAS5805M I2S configured: BCLK=%d WS=%d DATA=%d, 32-bit stereo @ %d Hz",
                 I2S_BCK, I2S_WS, I2S_DOUT, SAMPLE_RATE);
    }

    if (!i2s_enabled) {
        ret = i2s_channel_enable(tx_chan);
        if (ret != ESP_OK) {
            ESP_LOGE("I2S", "Failed to enable I2S TX channel: %s", esp_err_to_name(ret));
            return ret;
        }
        i2s_enabled = true;
    }

    // Prime DMA with silence. TAS5805M DSP settings can be ignored if applied
    // before it has seen a stable I2S clock for a few milliseconds.
    static const int32_t silence[128] = {0};
    size_t bytes_written = 0;
    esp_err_t i2s_ret = i2s_channel_write(tx_chan, silence, sizeof(silence),
                                          &bytes_written, 100U);
    if (i2s_ret != ESP_OK) {
        ESP_LOGW("I2S", "Could not prime TAS5805M clock with silence: %s", esp_err_to_name(i2s_ret));
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_err_t amp_ret = louder_tas5805m_start();
    if (amp_ret != ESP_OK) {
        ESP_LOGE("AUDIO", "TAS5805M DSP initialization failed: %s", esp_err_to_name(amp_ret));
        if (i2s_enabled) {
            i2s_channel_disable(tx_chan);
            i2s_enabled = false;
        }
        return amp_ret;
    }

    // From this point on the mixer task is the ONLY I2S writer. HFP, jingles
    // and future high-quality sources feed 48 kHz stereo PCM into it.
    esp_err_t mixer_ret = audio_mixer_start(tx_chan);
    if (mixer_ret != ESP_OK) {
        ESP_LOGE("AUDIO", "Audio mixer failed to start: %s", esp_err_to_name(mixer_ret));
        louder_tas5805m_stop();
        if (i2s_enabled) {
            i2s_channel_disable(tx_chan);
            i2s_enabled = false;
        }
        return mixer_ret;
    }

#if CONFIG_ENABLE_SD_RECORDING
    if (!sdcard_initialized)
    {
        // Initialize SD Card and Start Tasks
        if (init_sdcard() != ESP_OK)
        {
            ESP_LOGW("SDCARD", "Failed to initialize SD Card; audio playback continues without recording.");
            return ESP_OK;
        }
        sdcard_initialized = true;
    }

    // Create Ringbuffer
    audio_ringbuf = xRingbufferCreate(RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!audio_ringbuf)
    {
        ESP_LOGW("RINGBUF", "Failed to create recording ringbuffer; audio playback continues.");
        return ESP_OK;
    }

    // Start SD Card Task
    xTaskCreate(audio_record_task, "record_task", 30720, NULL, 20, &audio_record_task_handle);
    if (audio_record_task_handle == NULL)
    {
        ESP_LOGW("TASK", "Failed to create audio record task; audio playback continues.");
        vRingbufferDelete(audio_ringbuf);
        audio_ringbuf = NULL;
        return ESP_OK;
    }
#else
    ESP_LOGI("SDCARD", "Legacy SDMMC recording disabled for Louder H6 GPIO safety");
#endif

    return ESP_OK;
}

// Stop Recording & Audio Playback
void bt_app_send_data_shut_down(void)
{
    ESP_LOGI("AUDIO", "Shutting down audio...");

    // Stop local producers first, then stop the central single I2S writer.
    if (notification_sound_is_playing()) {
        notification_sound_stop();
        for (int i = 0; i < 20 && notification_sound_is_playing(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    audio_mixer_stop();

    // Put the TAS5805M in Hi-Z/PWDN while I2S clocks are still available.
    esp_err_t amp_ret = louder_tas5805m_stop();
    if (amp_ret != ESP_OK) {
        ESP_LOGW("AUDIO", "TAS5805M shutdown returned: %s", esp_err_to_name(amp_ret));
    }

    // Stop I2S
    if (i2s_enabled) {
        esp_err_t i2s_ret = i2s_channel_disable(tx_chan);
        if (i2s_ret != ESP_OK) {
            ESP_LOGW("I2S", "Failed to disable I2S TX channel: %s", esp_err_to_name(i2s_ret));
        } else {
            i2s_enabled = false;
        }
    }

#if CONFIG_ENABLE_SD_RECORDING
    // Stop Audio Record Task
    if (audio_record_task_handle)
    {
        vTaskDelete(audio_record_task_handle);
        audio_record_task_handle = NULL;
    }

    // Finalize WAV File
    if (record_file)
    {
        finalize_wav_file(record_file, total_bytes);
        fclose(record_file);
        record_file = NULL;
    }

    // Free Ringbuffer
    if (audio_ringbuf)
    {
        vRingbufferDelete(audio_ringbuf);
        audio_ringbuf = NULL;
    }

    // Close SD Card File
    if (sdcard_initialized)
    {
        // All done, unmount partition and disable SPI peripheral
        esp_vfs_fat_sdcard_unmount(mount_point, card);
        ESP_LOGI("SDCARD", "Card unmounted");
        sdcard_initialized = false;
    }
#endif

    ESP_LOGI("AUDIO", "Audio Stopped.");
}

#endif /* #if CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI */

const char *c_hf_evt_str[] = {
    "CONNECTION_STATE_EVT", /*!< SERVICE LEVEL CONNECTION STATE CONTROL */
    "AUDIO_STATE_EVT",      /*!< AUDIO CONNECTION STATE CONTROL */
    "VR_STATE_CHANGE_EVT",  /*!< VOICE RECOGNITION CHANGE */
    "VOLUME_CONTROL_EVT",   /*!< AUDIO VOLUME CONTROL */
    "UNKNOW_AT_CMD",        /*!< UNKNOW AT COMMAND RECIEVED */
    "IND_UPDATE",           /*!< INDICATION UPDATE */
    "CIND_RESPONSE_EVT",    /*!< CALL & DEVICE INDICATION */
    "COPS_RESPONSE_EVT",    /*!< CURRENT OPERATOR EVENT */
    "CLCC_RESPONSE_EVT",    /*!< LIST OF CURRENT CALL EVENT */
    "CNUM_RESPONSE_EVT",    /*!< SUBSCRIBER INFORTMATION OF CALL EVENT */
    "DTMF_RESPONSE_EVT",    /*!< DTMF TRANSFER EVT */
    "NREC_RESPONSE_EVT",    /*!< NREC RESPONSE EVT */
    "ANSWER_INCOMING_EVT",  /*!< ANSWER INCOMING EVT */
    "REJECT_INCOMING_EVT",  /*!< AREJECT INCOMING EVT */
    "DIAL_EVT",             /*!< DIAL INCOMING EVT */
    "WBS_EVT",              /*!< CURRENT CODEC EVT */
    "BCS_EVT",              /*!< CODEC NEGO EVT */
    "PKT_STAT_EVT",         /*!< REQUEST PACKET STATUS EVT */
};

// esp_hf_connection_state_t
const char *c_connection_state_str[] = {
    "DISCONNECTED",
    "CONNECTING",
    "CONNECTED",
    "SLC_CONNECTED",
    "DISCONNECTING",
};

// esp_hf_audio_state_t
const char *c_audio_state_str[] = {
    "disconnected",
    "connecting",
    "connected",
    "connected_msbc",
};

/// esp_hf_vr_state_t
const char *c_vr_state_str[] = {
    "Disabled",
    "Enabled",
};

// esp_hf_nrec_t
const char *c_nrec_status_str[] = {
    "NREC DISABLE",
    "NREC ABLE",
};

// esp_hf_control_target_t
const char *c_volume_control_target_str[] = {
    "SPEAKER",
    "MICROPHONE",
};

// esp_hf_subscriber_service_type_t
char *c_operator_name_str[] = {
    "China Mobile",
    "China Unicom",
    "China Telecom",
};

// esp_hf_subscriber_service_type_t
char *c_subscriber_service_type_str[] = {
    "UNKNOWN",
    "VOICE",
    "FAX",
};

// esp_hf_nego_codec_status_t
const char *c_codec_mode_str[] = {
    "CVSD Only",
    "Use CVSD",
    "Use MSBC",
};

void bt_app_hf_cb(esp_hf_cb_event_t event, esp_hf_cb_param_t *param)
{
    switch (event) {
    case ESP_HF_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "HFP connection state: %d", param->conn_stat.state);
        if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "HFP CONNECTED");
            is_connected = true;
            is_connecting = false;
            direct_retry_count = 0;

            if (saved_mac_timer != NULL) {
                esp_timer_stop(saved_mac_timer);
            }
            trying_saved_mac = false;

            memcpy(peer_bd_addr, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);

            // Save the real configured target name when available.
            const char *saved_device_name = bt_app_hf_get_target_device_name();
            if (saved_device_name == NULL || saved_device_name[0] == '\0') {
                saved_device_name = "HFP Headset";
            }
            esp_err_t save_ret = bt_device_memory_save_device(
                param->conn_stat.remote_bda, saved_device_name);
            if (save_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to save MAC address: %s", esp_err_to_name(save_ret));
            }

            esp_hf_ag_cind_response(peer_bd_addr, 1, 0, 0, 0, 5, 0, 5);
        } else if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_SLC_CONNECTED) {
            ESP_LOGI(TAG, "HFP SLC fully connected - preparing audio");
            is_connected = true;
            is_connecting = false;
            direct_retry_count = 0;
            trying_saved_mac = false;
            if (saved_mac_timer != NULL) {
                esp_timer_stop(saved_mac_timer);
            }

            esp_bt_gap_cancel_discovery();
            audio_retry_count = 0;
            audio_connect_requested = false;
            codec_negotiation_count = 0;
            negotiated_mode = ESP_HF_WBS_NONE;
            hfp_input_sample_rate = HFP_CVSD_SAMPLE_RATE;
            connection_jingle_pending = true;

            // Trigger call setup immediately, finish setup asynchronously.
            esp_hf_ag_ciev_report(peer_bd_addr, ESP_HF_IND_TYPE_CALLSETUP, 1);
            ESP_LOGI(TAG, "Sent incoming call indicator (CALLSETUP=1)");
            bt_app_start_one_shot(audio_setup_timer, AUDIO_SETUP_DELAY_MS);
        } else if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_CONNECTING) {
            ESP_LOGI(TAG, "HFP connecting...");
            is_connecting = true;
        } else if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "HFP disconnected");

            bool was_saved_attempt = trying_saved_mac;
            if (saved_mac_timer != NULL) {
                esp_timer_stop(saved_mac_timer);
            }
            trying_saved_mac = false;

#if CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI
            bt_app_send_data_shut_down();
#endif

            is_connected = false;
            is_connecting = false;
            audio_retry_count = 0;
            audio_connect_requested = false;
            negotiated_mode = ESP_HF_WBS_NONE;
            hfp_input_sample_rate = HFP_CVSD_SAMPLE_RATE;
            connection_jingle_pending = false;

            if (suppress_disconnect_reconnect_once) {
                suppress_disconnect_reconnect_once = false;
                ESP_LOGD(TAG, "Disconnect belongs to manager timeout abort; reconnect already scheduled");
            } else if (was_saved_attempt) {
                // A direct MAC attempt failed before its timeout. Preserve retry count.
                if (direct_retry_count < MAX_DIRECT_RETRIES) {
                    bt_app_start_one_shot(reconnect_timer, DIRECT_RETRY_DELAY_MS);
                } else {
                    bt_app_post_event(BT_MGR_EVT_START_DISCOVERY);
                }
            } else if (bt_device_memory_has_saved_device()) {
                // Established connection was lost: restart the fast-MAC strategy from attempt 1.
                direct_retry_count = 0;
                bt_app_start_one_shot(reconnect_timer, DIRECT_RETRY_DELAY_MS);
            } else {
                bt_app_start_one_shot(discovery_timer, DISCOVERY_RESTART_DELAY_MS);
            }
        }
        break;
        
    case ESP_HF_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "Audio state: %d", param->audio_stat.state);
        // The audio-state event is authoritative for the active transport.
        if (param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED ||
            param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) {
            if (param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) {
                negotiated_mode = ESP_HF_WBS_YES;
                hfp_input_sample_rate = HFP_MSBC_SAMPLE_RATE;
            } else {
                negotiated_mode = ESP_HF_WBS_NO;
                hfp_input_sample_rate = HFP_CVSD_SAMPLE_RATE;
            }

            ESP_LOGI(TAG, "=== HFP AUDIO CONNECTED: %s, %lu Hz mono -> %d Hz stereo ===",
                     bt_app_hfp_codec_name(negotiated_mode),
                     (unsigned long)hfp_input_sample_rate, SAMPLE_RATE);
            
#if CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI
            ESP_LOGI(TAG, "Microphone audio will now stream to Louder H6");
            
            // Initialize I2S first, then configure the TAS5805M DSP while the
            // I2S clocks are already stable. Do not accept PCM callbacks if the
            // amplifier setup failed.
            esp_err_t audio_init_ret = bt_app_send_data();
            if (audio_init_ret == ESP_OK) {
                // Start the brand song without blocking the Bluetooth callback. The
                // incoming HFP callback is registered immediately, but suppresses
                // local microphone playback while the jingle owns I2S.
                if (connection_jingle_pending) {
                    esp_err_t jingle_ret = notification_sound_play_connection_async();
                    if (jingle_ret == ESP_OK) {
                        connection_jingle_pending = false;
                    } else {
                        ESP_LOGW(TAG, "Could not start AVA brand jingle: %s",
                                 esp_err_to_name(jingle_ret));
                    }
                }

                esp_hf_ag_register_data_callback(bt_app_hf_incoming_cb, bt_hf_audio_data_send_cb);
                ESP_LOGI(TAG, "Audio data callback registered; Louder H6 output ready");

                audio_retry_count = 0;
                audio_connect_requested = false;
            } else {
                ESP_LOGE(TAG, "Louder H6 audio path failed to start: %s", esp_err_to_name(audio_init_ret));
                audio_connect_requested = false;
                // Tear down the SCO link so the existing retry path can try again.
                esp_hf_ag_audio_disconnect(peer_bd_addr);
            }
#else
            ESP_LOGW(TAG, "Audio connected but CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI not enabled");
#endif
        } else if (param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTING) {
            ESP_LOGI(TAG, "HFP audio connecting...");
        } else if (param->audio_stat.state == ESP_HF_AUDIO_STATE_DISCONNECTED) {
            ESP_LOGW(TAG, "Audio disconnected (attempt %d/%d)", audio_retry_count, MAX_AUDIO_RETRIES);
            
#if CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI
            // Shutdown I2S and SD card recording
            bt_app_send_data_shut_down();
#endif
            
            // Reset the connect requested flag to allow retry
            audio_connect_requested = false;
            
            // Retry asynchronously so the HFP callback remains non-blocking.
            if (is_connected && audio_retry_count < MAX_AUDIO_RETRIES) {
                ESP_LOGI(TAG, "Scheduling audio reconnect (attempt %d/%d)",
                         audio_retry_count + 1, MAX_AUDIO_RETRIES);
                bt_app_start_one_shot(audio_retry_timer, AUDIO_RETRY_DELAY_MS);
            } else if (audio_retry_count >= MAX_AUDIO_RETRIES) {
                ESP_LOGE(TAG, "Audio connection failed after %d retries", MAX_AUDIO_RETRIES);
            }
        }
        break;
        
    case ESP_HF_WBS_RESPONSE_EVT:
        // Codec status from the HF peer. This is not necessarily the final
        // selection, but is useful for diagnosing WBS/eSCO negotiation.
        ESP_LOGI(TAG, "HFP codec status: %s (%d)",
                 bt_app_hfp_codec_name(param->wbs_rep.codec),
                 (int)param->wbs_rep.codec);
        if (param->wbs_rep.codec == ESP_HF_WBS_YES) {
            hfp_input_sample_rate = HFP_MSBC_SAMPLE_RATE;
        } else if (param->wbs_rep.codec == ESP_HF_WBS_NO) {
            hfp_input_sample_rate = HFP_CVSD_SAMPLE_RATE;
        }
        break;

    case ESP_HF_BCS_RESPONSE_EVT:
        // Final codec chosen by HFP codec negotiation. mSBC remains preferred
        // when both sides support WBS; CVSD is the transparent fallback.
        negotiated_mode = param->bcs_rep.mode;
        hfp_input_sample_rate = (negotiated_mode == ESP_HF_WBS_YES)
                                    ? HFP_MSBC_SAMPLE_RATE
                                    : HFP_CVSD_SAMPLE_RATE;
        ESP_LOGI(TAG, "HFP final codec: %s (%d), input sample rate %lu Hz",
                 bt_app_hfp_codec_name(negotiated_mode),
                 (int)negotiated_mode,
                 (unsigned long)hfp_input_sample_rate);
        break;

    case ESP_HF_CIND_RESPONSE_EVT:
        ESP_LOGI(TAG, "CIND (Indicator) response event - sending status");
        // Respond with indicator values: service=1, call=0, callsetup=0, callheld=0, signal=5, roam=0, batt=5
        esp_hf_ag_cind_response(peer_bd_addr, 1, 0, 0, 0, 5, 0, 5);
        break;
    
    case ESP_HF_COPS_RESPONSE_EVT:
        ESP_LOGI(TAG, "COPS (Operator Selection) response event - sending operator");
        // Respond with operator name
        esp_hf_ag_cops_response(peer_bd_addr, "ESP32-Net");
        break;
        
    case ESP_HF_VOLUME_CONTROL_EVT:
        ESP_LOGI(TAG, "Volume control - Type: %d, Volume: %d", 
                 param->volume_control.type, param->volume_control.volume);
        // Volume is controlled by headset, just acknowledge
        break;
        
    case ESP_HF_BVRA_RESPONSE_EVT:
        ESP_LOGI(TAG, "Voice recognition response");
        break;
        
    default:
        ESP_LOGD(TAG, "Unhandled HFP event: %d", event);
        break;
    }
}

/**
 * @brief Initialize Bluetooth HFP AG
 */
esp_err_t bt_app_hf_init(const char *target_device_name)
{
    esp_err_t ret;
    
    ESP_LOGI(TAG, "Initializing Bluetooth HFP AG...");
    
    // Initialize persistent device memory before checking the reset input,
    // otherwise a reset cannot erase the saved device.
    ESP_LOGI(TAG, "Initializing device memory for MAC address persistence...");
    ret = bt_device_memory_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Device memory init failed (non-critical): %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "");
    bt_app_check_reset_gpio();
    ESP_LOGI(TAG, "");

    bt_app_register_reset_command();

    ret = bt_app_connection_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Bluetooth connection manager: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // ✅ NEW: Print saved device MAC address on startup
    ESP_LOGI(TAG, "");
    bt_app_print_saved_device();
    ESP_LOGI(TAG, "");
    
    // Set target device if provided
    if (target_device_name) {
        bt_app_hf_set_target_device_name(target_device_name);
    }
    
    // Register HFP AG callback
    esp_hf_ag_register_callback(bt_app_hf_cb);
    
    // Initialize HFP AG
    ret = esp_hf_ag_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize HFP AG: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Register GAP callback
    esp_bt_gap_register_callback(bt_app_gap_cb);
    
    // Set pin code (0000)
    esp_bt_pin_code_t pin_code;
    pin_code[0] = '0';
    pin_code[1] = '0';
    pin_code[2] = '0';
    pin_code[3] = '0';
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin_code);
    
    // Enable SSP for Bluetooth pairing
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
    
    // Set Device name
    char local_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = "ESP32-HFP-AG";
    esp_bt_gap_set_device_name(local_name);
    
    ESP_LOGI(TAG, "HFP AG initialized successfully");
    
    return ESP_OK;
}

/**
 * @brief Start Bluetooth discovery
 */
esp_err_t bt_app_hf_start(void)
{
    ESP_LOGI(TAG, "Starting Bluetooth connection manager...");

    esp_err_t ret = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set scan mode: %s", esp_err_to_name(ret));
    }

    is_connected = false;
    is_connecting = false;
    trying_saved_mac = false;
    direct_retry_count = 0;

    if (bt_device_memory_has_saved_device()) {
        ESP_LOGI(TAG, "Saved device found: trying direct MAC reconnect first");
        bt_app_post_event(BT_MGR_EVT_TRY_SAVED);
    } else {
        ESP_LOGI(TAG, "No saved MAC address: starting short discovery");
        bt_app_post_event(BT_MGR_EVT_START_DISCOVERY);
    }

    return ESP_OK;
}
