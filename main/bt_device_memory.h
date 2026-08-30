#ifndef __BT_DEVICE_MEMORY_H__
#define __BT_DEVICE_MEMORY_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Initialize NVS for Bluetooth device storage
 * @return ESP_OK on success
 */
esp_err_t bt_device_memory_init(void);

/**
 * @brief Save Bluetooth device MAC address to NVS
 * @param mac_addr Pointer to 6-byte MAC address
 * @param device_name Optional device name (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t bt_device_memory_save_device(const uint8_t *mac_addr, const char *device_name);

/**
 * @brief Load saved Bluetooth device MAC address from NVS
 * @param mac_addr Pointer to 6-byte buffer to store MAC address
 * @param device_name Pointer to buffer for device name (can be NULL, min 64 bytes if used)
 * @return ESP_OK if device found, ESP_ERR_NVS_NOT_FOUND if no saved device
 */
esp_err_t bt_device_memory_load_device(uint8_t *mac_addr, char *device_name);

/**
 * @brief Clear saved Bluetooth device from NVS
 * @return ESP_OK on success
 */
esp_err_t bt_device_memory_clear_device(void);

/**
 * @brief Check if a device is saved
 * @return true if device is saved, false otherwise
 */
bool bt_device_memory_has_saved_device(void);

#endif /* __BT_DEVICE_MEMORY_H__ */
