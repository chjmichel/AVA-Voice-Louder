#ifndef __NOTIFICATION_SOUND_H__
#define __NOTIFICATION_SOUND_H__

#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Play a "badabummm" connection notification sound
 * Plays a short, ascending drum-like sound pattern through I2S
 * 
 * The sound consists of:
 * - Low tone (kick): 80 Hz
 * - Mid tone (snare): 200 Hz
 * - High tone (cymbal): 400 Hz
 * Each tone lasts ~150ms with a quick decay envelope
 * 
 * @return ESP_OK on success, or error code if I2S not initialized
 */
esp_err_t notification_sound_play_connection(void);

#endif /* __NOTIFICATION_SOUND_H__ */
