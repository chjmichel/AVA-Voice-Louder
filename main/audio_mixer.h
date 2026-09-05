#ifndef AVA_AUDIO_MIXER_H
#define AVA_AUDIO_MIXER_H

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
#define AUDIO_MIXER_CHANNELS       2U
#define AUDIO_MIXER_BLOCK_FRAMES   240U  /* 5 ms at 48 kHz */

/**
 * Start the central 48 kHz stereo mixer.
 *
 * tx_chan is the already configured/enabled I2S0 TX channel that drives the
 * Louder H6 TAS5805M (GPIO26 BCLK, GPIO25 LRCLK, GPIO22 DATA).
 *
 * The mixer additionally creates I2S1 RX as a SLAVE for the CM108B:
 *   GPIO18 <- CM108B DASCLK/BCLK
 *   GPIO19 <- CM108B DALRCK/LRCLK
 *   GPIO23 <- CM108B SDOUT/DATA
 *
 * The mixer task is the ONLY writer to tx_chan after this function succeeds.
 */
esp_err_t audio_mixer_start(i2s_chan_handle_t tx_chan);

/** Stop mixer and CM108B I2S1 RX. */
void audio_mixer_stop(void);

/** Returns true while the central mixer is running. */
bool audio_mixer_is_running(void);

/**
 * Queue already-resampled HFP PCM.
 *
 * Format: 48 kHz, stereo, signed 32-bit I2S samples, one frame = L + R.
 * The current bt_app_hf.c resampler produces exactly this format.
 *
 * This function is non-blocking. ESP_ERR_TIMEOUT means the bounded low-latency
 * HFP FIFO was full and this newest chunk was dropped instead of increasing
 * monitoring latency.
 */
esp_err_t audio_mixer_submit_hfp(const int32_t *stereo_pcm, size_t frames);

/**
 * Queue 48 kHz stereo signed 32-bit jingle PCM.
 * This may wait up to timeout_ticks for FIFO space.
 */
esp_err_t audio_mixer_submit_jingle(const int32_t *stereo_pcm,
                                    size_t frames,
                                    TickType_t timeout_ticks);

/** Mark whether a local jingle is active (used for source ducking). */
void audio_mixer_set_jingle_active(bool active);

/**
 * Drop any queued HFP PCM without stopping the central audio engine.
 * CM108B USB audio and TAS5805M output continue running.
 */
void audio_mixer_clear_hfp(void);

/**
 * Set the HFP headset microphone mixer gain. The HFP AG maps both incoming
 * AT+VGM and AT+VGS volume controls to this level for AVA.
 *
 * Range: 0..15. Gain 10 is the nominal 1.0 mixer level, gain 0 mutes the
 * HFP source, and gain 15 provides approximately +6 dB (2.0x) boost.
 * This affects only the HFP microphone source; USB/CM108B and jingle levels
 * remain unchanged.
 */
void audio_mixer_set_hfp_mic_gain(int gain);

/** Return the currently active HFP microphone gain (0..15). */
int audio_mixer_get_hfp_mic_gain(void);

/** Wait until all queued jingle PCM has been consumed by the mixer. */
esp_err_t audio_mixer_wait_jingle_drained(TickType_t timeout_ticks);

#ifdef __cplusplus
}
#endif

#endif
