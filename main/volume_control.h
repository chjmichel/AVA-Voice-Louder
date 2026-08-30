#ifndef __VOLUME_CONTROL_H__
#define __VOLUME_CONTROL_H__

#include <stdint.h>
#include "esp_err.h"

#define VOLUME_MIN 0
#define VOLUME_MAX 100

/**
 * @brief Initialize the potentiometer volume control.
 *
 * On Louder ESP32 Rev H6 the default is GPIO36 (ADC1_CH0). The GPIO is
 * configurable via menuconfig. The initial ADC value is read immediately so
 * the amplifier never starts at an unrelated default volume.
 */
esp_err_t volume_control_init(void);

/** @brief Get current logical volume (0-100). */
uint8_t volume_control_get_volume(void);

/**
 * @brief Set logical volume and apply it to the TAS5805M hardware DSP.
 */
esp_err_t volume_control_set_volume(uint8_t volume);

/** @brief Start periodic potentiometer monitoring. */
esp_err_t volume_control_start_monitoring(void);

#endif /* __VOLUME_CONTROL_H__ */
