#ifndef __BT_APP_HF_H__
#define __BT_APP_HF_H__

#include <stdint.h>
#include "esp_hf_ag_api.h"
#include "esp_bt_defs.h"
#include "driver/i2s_std.h"

// Bluetooth address of connected device (used in GAP and HFP callbacks)
extern esp_bd_addr_t peer_bd_addr;

// Connection state tracking
extern bool is_connected;
extern bool is_connecting;

// I2S transmit channel handle (for audio output to Louder H6 TAS5805M)
extern i2s_chan_handle_t tx_chan;

/**
 * @brief Get target device name (runtime or config)
 */
const char* bt_app_hf_get_target_device_name(void);

/**
 * @brief Set target device name (overrides config)
 */
void bt_app_hf_set_target_device_name(const char *device_name);

/**
 * @brief Initialize Bluetooth HFP AG
 * @param target_device_name Optional target device to connect to
 */
esp_err_t bt_app_hf_init(const char *target_device_name);

/**
 * @brief Start Bluetooth discovery
 */
esp_err_t bt_app_hf_start(void);

/**
 * @brief HFP AG callback function
 */
void bt_app_hf_cb(esp_hf_cb_event_t event, esp_hf_cb_param_t *param);

/**
 * @brief Audio data callback
 */
void bt_app_hf_incoming_cb(const uint8_t *buf, uint32_t sz);

/**
 * @brief Audio data send callback
 */
uint32_t bt_hf_audio_data_send_cb(uint8_t *data, uint32_t len);

#endif /* __BT_APP_HF_H__ */
