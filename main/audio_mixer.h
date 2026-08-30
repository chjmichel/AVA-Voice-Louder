#ifndef __AUDIO_MIXER_H__
#define __AUDIO_MIXER_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_MIXER_SAMPLE_RATE_HZ 48000U

/**
 * @brief Start the single-writer 48 kHz stereo mixer for the Louder H6.
 *
 * From the moment this succeeds, only the mixer task may write to the I2S TX
 * channel. HFP and local sounds feed PCM into the mixer instead.
 */
esp_err_t audio_mixer_start(i2s_chan_handle_t tx_chan);

/**
 * @brief Stop the mixer task and clear all pending source PCM.
 */
void audio_mixer_stop(void);

/**
 * @brief Submit already-resampled 48 kHz stereo signed 32-bit HFP PCM.
 *
 * This call is non-blocking and is safe for the HFP data callback. If the HFP
 * FIFO cannot accept the complete block, the complete block is dropped to
 * avoid ever-growing live-monitor latency.
 *
 * @param stereo_pcm Interleaved L/R samples.
 * @param frames Number of stereo frames.
 * @return ESP_OK, ESP_ERR_TIMEOUT if the complete block could not be queued,
 *         or ESP_ERR_INVALID_STATE if the mixer is not running.
 */
esp_err_t audio_mixer_submit_hfp(const int32_t *stereo_pcm, size_t frames);

/**
 * @brief Submit 48 kHz stereo signed 32-bit local-jingle PCM.
 *
 * The local jingle runs in its own task and may wait briefly for FIFO space.
 * The HFP callback never waits on the jingle path.
 */
esp_err_t audio_mixer_submit_jingle(const int32_t *stereo_pcm,
                                    size_t frames,
                                    TickType_t timeout_ticks);

/**
 * @brief Tell the mixer that the local connection jingle is active.
 *
 * While active, HFP is ducked to 20 percent and the jingle remains at 100
 * percent. When false, HFP returns to 100 percent.
 */
void audio_mixer_set_jingle_active(bool active);

/**
 * @brief Wait until all queued jingle PCM has been rendered by the mixer.
 */
esp_err_t audio_mixer_wait_jingle_drained(TickType_t timeout_ticks);

bool audio_mixer_is_running(void);

#ifdef __cplusplus
}
#endif

#endif /* __AUDIO_MIXER_H__ */
