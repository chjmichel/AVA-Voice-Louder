#ifndef __LOUDER_TAS5805M_H__
#define __LOUDER_TAS5805M_H__

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Prepare Louder ESP32 Rev H6 amplifier control.
 *
 * Configures the H6 I2C bus and keeps TAS5805M in power-down until I2S
 * clocks are available. Safe to call once during application startup.
 */
esp_err_t louder_tas5805m_prepare(void);

/**
 * @brief Start and configure TAS5805M after I2S has been enabled.
 *
 * The TAS5805M needs a stable I2S clock before DSP/register configuration.
 * This function power-cycles the amplifier, loads a flat stereo DSP startup
 * configuration, applies the cached volume and enters PLAY state.
 */
esp_err_t louder_tas5805m_start(void);

/**
 * @brief Put TAS5805M into Hi-Z and power it down.
 */
esp_err_t louder_tas5805m_stop(void);

/**
 * @brief Set output gain from a linear amplitude percentage.
 *
 * 100% corresponds to 0 dB digital gain. Values below 100% are converted to
 * the TAS5805M native 0.5 dB volume steps, so 60% closely matches the old
 * software sample scaling of 0.60. 0% uses the TAS5805M mute code.
 *
 * The value is cached even if the amplifier is not started yet.
 */
esp_err_t louder_tas5805m_set_volume_percent(uint8_t volume_percent);

/**
 * @brief Get the cached logical volume percentage.
 */
uint8_t louder_tas5805m_get_volume_percent(void);

/**
 * @brief True after the DSP startup sequence completed successfully.
 */
bool louder_tas5805m_is_ready(void);

#endif /* __LOUDER_TAS5805M_H__ */
