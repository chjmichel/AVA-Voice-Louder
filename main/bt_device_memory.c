#include "bt_device_memory.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "BT_DEVICE_MEM";
static const char *NVS_NAMESPACE = "bt_device";
static const char *NVS_KEY_MAC = "mac_addr";
static const char *NVS_KEY_NAME = "dev_name";

static nvs_handle_t nvs_h = 0;

esp_err_t bt_device_memory_init(void)
{
    // NVS flash is initialized centrally in app_main().
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "NVS namespace opened for Bluetooth device storage");
    return ESP_OK;
}

esp_err_t bt_device_memory_save_device(const uint8_t *mac_addr, const char *device_name)
{
    esp_err_t ret = ESP_OK;
    
    if (mac_addr == NULL) {
        ESP_LOGE(TAG, "MAC address cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (nvs_h == 0) {
        ESP_LOGE(TAG, "NVS not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Save MAC address (6 bytes)
    ret = nvs_set_blob(nvs_h, NVS_KEY_MAC, mac_addr, 6);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save MAC address: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Save device name if provided
    if (device_name != NULL) {
        ret = nvs_set_str(nvs_h, NVS_KEY_NAME, device_name);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save device name: %s", esp_err_to_name(ret));
            // Continue anyway, MAC is the critical part
        }
    }
    
    // Commit changes to flash
    ret = nvs_commit(nvs_h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Saved BT device: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac_addr[0], mac_addr[1], mac_addr[2], 
             mac_addr[3], mac_addr[4], mac_addr[5]);
    
    if (device_name != NULL) {
        ESP_LOGI(TAG, "Device name: %s", device_name);
    }
    
    return ESP_OK;
}

esp_err_t bt_device_memory_load_device(uint8_t *mac_addr, char *device_name)
{
    esp_err_t ret = ESP_OK;
    
    if (mac_addr == NULL) {
        ESP_LOGE(TAG, "MAC address buffer cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (nvs_h == 0) {
        ESP_LOGE(TAG, "NVS not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Load MAC address
    size_t mac_len = 6;
    ret = nvs_get_blob(nvs_h, NVS_KEY_MAC, mac_addr, &mac_len);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No saved Bluetooth device found");
        } else {
            ESP_LOGE(TAG, "Failed to load MAC address: %s", esp_err_to_name(ret));
        }
        return ret;
    }
    
    // Load device name if buffer provided
    if (device_name != NULL) {
        size_t name_len = 64;
        ret = nvs_get_str(nvs_h, NVS_KEY_NAME, device_name, &name_len);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            device_name[0] = '\0';  // Empty string if no name saved
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to load device name: %s", esp_err_to_name(ret));
            device_name[0] = '\0';
        }
    }
    
    ESP_LOGI(TAG, "Loaded saved BT device: %02X:%02X:%02X:%02X:%02X:%02X", 
             mac_addr[0], mac_addr[1], mac_addr[2], 
             mac_addr[3], mac_addr[4], mac_addr[5]);
    
    return ESP_OK;
}

esp_err_t bt_device_memory_clear_device(void)
{
    esp_err_t ret = ESP_OK;
    
    if (nvs_h == 0) {
        ESP_LOGE(TAG, "NVS not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ret = nvs_erase_key(nvs_h, NVS_KEY_MAC);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to clear MAC address: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_erase_key(nvs_h, NVS_KEY_NAME);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to clear device name: %s", esp_err_to_name(ret));
    }
    
    ret = nvs_commit(nvs_h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Cleared saved Bluetooth device");
    return ESP_OK;
}

bool bt_device_memory_has_saved_device(void)
{
    uint8_t mac_addr[6];
    size_t mac_len = 6;
    
    if (nvs_h == 0) {
        return false;
    }
    
    esp_err_t ret = nvs_get_blob(nvs_h, NVS_KEY_MAC, mac_addr, &mac_len);
    return (ret == ESP_OK);
}
