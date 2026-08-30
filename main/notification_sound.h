#ifndef __NOTIFICATION_SOUND_H__
#define __NOTIFICATION_SOUND_H__

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Start the embedded AVA brand jingle asynchronously.
 *
 * The source file is embedded at build time from:
 *   assets/audio/start_jingle.wav
 *
 * The WAV is PCM 16-bit mono at 32 kHz. Playback is resampled on the fly to
 * the Louder H6 master format (48 kHz, 32-bit stereo) and submitted to the
 * central audio mixer. The jingle task never writes to I2S directly.
 *
 * This function returns immediately and therefore does not block the HFP
 * callback/task while the jingle is playing.
 *
 * @return ESP_OK if playback was started (or is already active), otherwise an
 *         ESP-IDF error code.
 */
esp_err_t notification_sound_play_connection_async(void);

/**
 * @brief Request the currently playing jingle to stop.
 *
 * The playback task checks this flag between small mixer chunks and exits
 * promptly. This is used before shutting down the Louder audio path.
 */
void notification_sound_stop(void);

/**
 * @brief Return true while the brand jingle is feeding the central mixer.
 */
bool notification_sound_is_playing(void);

#endif /* __NOTIFICATION_SOUND_H__ */
