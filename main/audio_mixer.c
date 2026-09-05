#include "audio_mixer.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO_MIXER";

/* 5 ms output blocks keep monitoring latency small while avoiding tiny writes. */
#define MIX_FRAMES                 AUDIO_MIXER_BLOCK_FRAMES
#define MIX_CHANNELS               2U
#define MIX_BYTES_32               (MIX_FRAMES * MIX_CHANNELS * sizeof(int32_t))
#define AUDIO_MIXER_TASK_STACK_BYTES 6144U
#define USB_RX_TASK_STACK_BYTES    4096U
#define I2S_TX_TIMEOUT_MS          50U
#define I2S_RX_TIMEOUT_MS          20U
#define CM108B_RX_STATS_INTERVAL_US 2000000LL
#define CM108B_CLOCK_PROBE_PERIOD_MS 5000U
#define CM108B_CLOCK_PROBE_WINDOW_MS 100U
#define CM108B_CLOCK_PROBE_STACK_BYTES 3072U
#define USB_FIFO_BLOCKS            4U      /* 20 ms maximum queued USB audio */
#define USB_PREBUFFER_BLOCKS       2U      /* 10 ms start/restart cushion */
#define USB_FIFO_BYTES             (MIX_BYTES_32 * USB_FIFO_BLOCKS)

/* Bounded producer FIFOs. HFP stays intentionally small to avoid audible echo. */
#define HFP_FIFO_BYTES             (MIX_BYTES_32 * 2U)   /* ~10 ms */
#define JINGLE_FIFO_BYTES          (MIX_BYTES_32 * 12U)  /* ~60 ms */

#ifndef CONFIG_CM108B_BCLK_GPIO
#define CONFIG_CM108B_BCLK_GPIO 18
#endif
#ifndef CONFIG_CM108B_LRCLK_GPIO
#define CONFIG_CM108B_LRCLK_GPIO 19
#endif
#ifndef CONFIG_CM108B_DATA_GPIO
#define CONFIG_CM108B_DATA_GPIO 23
#endif
#ifndef CONFIG_AUDIO_MIX_USB_GAIN_PERCENT
#define CONFIG_AUDIO_MIX_USB_GAIN_PERCENT 70
#endif
#ifndef CONFIG_AUDIO_MIX_HFP_GAIN_PERCENT
#define CONFIG_AUDIO_MIX_HFP_GAIN_PERCENT 100
#endif
#ifndef CONFIG_HFP_MIC_DEFAULT_GAIN
#define CONFIG_HFP_MIC_DEFAULT_GAIN 10
#endif
#ifndef CONFIG_AUDIO_MIX_JINGLE_GAIN_PERCENT
#define CONFIG_AUDIO_MIX_JINGLE_GAIN_PERCENT 100
#endif
#ifndef CONFIG_AUDIO_MIX_JINGLE_DUCK_PERCENT
#define CONFIG_AUDIO_MIX_JINGLE_DUCK_PERCENT 0
#endif

/* CM108B diagnostics. Current baseline receives full 32-bit Philips-I2S
 * slots and extracts the 16-bit PCM payload in software. */
#ifndef CONFIG_CM108B_DIAG_RX_STATS
#define CONFIG_CM108B_DIAG_RX_STATS 1
#endif
#ifndef CONFIG_CM108B_DIAG_1KHZ_TONE
#define CONFIG_CM108B_DIAG_1KHZ_TONE 0
#endif
#ifndef CONFIG_CM108B_DIAG_CLOCK_PROBE
#define CONFIG_CM108B_DIAG_CLOCK_PROBE 1
#endif
#ifndef CONFIG_CM108B_RAW_USE_LOWER16
#define CONFIG_CM108B_RAW_USE_LOWER16 0
#endif

static i2s_chan_handle_t s_rx_chan = NULL;
static i2s_chan_handle_t s_tx_chan = NULL;
static StreamBufferHandle_t s_usb_fifo = NULL;
static StreamBufferHandle_t s_hfp_fifo = NULL;
static StreamBufferHandle_t s_jingle_fifo = NULL;
static TaskHandle_t s_usb_rx_task = NULL;
static TaskHandle_t s_clock_probe_task = NULL;
static TaskHandle_t s_mixer_task = NULL;

static volatile bool s_running = false;
static volatile bool s_jingle_active = false;

static uint32_t s_hfp_drop_chunks = 0U;
static uint32_t s_hfp_latency_flushes = 0U;
static uint32_t s_usb_rx_full_blocks = 0U;
static uint32_t s_usb_rx_partial_reads = 0U;
static uint32_t s_usb_rx_timeout_count = 0U;
static uint32_t s_usb_rx_error_count = 0U;
static uint32_t s_usb_rx_incomplete_blocks = 0U;
static uint32_t s_usb_fifo_drops = 0U;
static uint32_t s_usb_fifo_partial_reads = 0U;
static uint32_t s_usb_underflows = 0U;
static uint32_t s_tx_timeout_count = 0U;
static volatile bool s_usb_streaming = false;
static volatile bool s_hfp_flush_requested = false;

/*
 * HFP microphone gain follows the HFP VGM range 0..15. VGM 10 deliberately
 * equals the existing mixer level (1.0x), so the new control is neutral at
 * the requested default. Values above 10 provide headroom up to 2.0x at 15.
 * The Q15-style table is read once per 5 ms mixer block; the 32-bit value is
 * atomic on ESP32 and can safely be updated from the Bluetooth callback.
 */
static volatile int s_hfp_mic_gain = CONFIG_HFP_MIC_DEFAULT_GAIN;
static const int32_t s_hfp_mic_gain_q15[16] = {
        0,  /*  0 = mute */
     3277,  /*  1 = 0.10x */
     6554,  /*  2 = 0.20x */
     9830,  /*  3 = 0.30x */
    13107,  /*  4 = 0.40x */
    16384,  /*  5 = 0.50x */
    19661,  /*  6 = 0.60x */
    22938,  /*  7 = 0.70x */
    26214,  /*  8 = 0.80x */
    29491,  /*  9 = 0.90x */
    32768,  /* 10 = 1.00x / nominal */
    36864,  /* 11 = 1.125x */
    40960,  /* 12 = 1.25x */
    45056,  /* 13 = 1.375x */
    52429,  /* 14 = 1.60x */
    65536   /* 15 = 2.00x / about +6 dB */
};

/*
 * Keep the large 5 ms audio blocks out of the FreeRTOS task stack.
 * Keep all large audio blocks in static RAM rather than on the FreeRTOS
 * task stacks. The earlier stack-local buffers caused the observed overflow.
 */
/*
 * CM108B RAW diagnostic path:
 * - Philips I2S
 * - receive the complete 32-bit wire slot for each channel
 * - 64 BCLK per stereo frame
 *
 * This deliberately bypasses the original ESP32 HW-v1 16-bit/32-slot FIFO
 * packing rules. The valid CM108B 16-bit PCM word is extracted in software.
 */
static int32_t s_usb_rx_raw[MIX_FRAMES * MIX_CHANNELS];
static int32_t s_usb_rx_mix[MIX_FRAMES * MIX_CHANNELS];
static int32_t s_usb_block[MIX_FRAMES * MIX_CHANNELS];
static int32_t s_hfp_block[MIX_FRAMES * MIX_CHANNELS];
static int32_t s_jingle_block[MIX_FRAMES * MIX_CHANNELS];
static int32_t s_output_block[MIX_FRAMES * MIX_CHANNELS];

static int32_t clamp_i32(int64_t value)
{
    if (value > INT32_MAX) {
        return INT32_MAX;
    }
    if (value < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)value;
}

typedef struct {
    int64_t start_us;
    uint32_t full_blocks;
    uint32_t partial_reads;
    uint32_t timeouts;
    uint32_t errors;
    uint32_t incomplete;
    uint32_t fifo_drops;
} cm108b_rx_diag_window_t;

static void cm108b_log_rx_stats_if_due(cm108b_rx_diag_window_t *w)
{
#if CONFIG_CM108B_DIAG_RX_STATS
    if (w == NULL) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    const int64_t elapsed_us = now_us - w->start_us;
    if (elapsed_us < CM108B_RX_STATS_INTERVAL_US) {
        return;
    }

    const size_t fifo_bytes = s_usb_fifo != NULL
                                  ? xStreamBufferBytesAvailable(s_usb_fifo)
                                  : 0U;
    const uint32_t fifo_blocks = (uint32_t)(fifo_bytes / MIX_BYTES_32);

    ESP_LOGI(TAG,
             "CM108B RX stats/2s: full_blocks=%lu partial_reads=%lu "
             "timeouts=%lu errors=%lu incomplete=%lu fifo_drops=%lu "
             "mixer_underflows_total=%lu fifo_level=%lu/%u blocks",
             (unsigned long)w->full_blocks,
             (unsigned long)w->partial_reads,
             (unsigned long)w->timeouts,
             (unsigned long)w->errors,
             (unsigned long)w->incomplete,
             (unsigned long)w->fifo_drops,
             (unsigned long)s_usb_underflows,
             (unsigned long)fifo_blocks,
             (unsigned)USB_FIFO_BLOCKS);

    if (w->full_blocks == 0U && w->timeouts != 0U) {
        ESP_LOGW(TAG,
                 "CM108B RX received NO complete DMA block in this window; "
                 "check DASCLK GPIO%d and DALRCK GPIO%d. Clock probe below is authoritative.",
                 CONFIG_CM108B_BCLK_GPIO,
                 CONFIG_CM108B_LRCLK_GPIO);
    }

    memset(w, 0, sizeof(*w));
    w->start_us = now_us;
#else
    (void)w;
#endif
}

#if CONFIG_CM108B_DIAG_CLOCK_PROBE
static esp_err_t cm108b_create_pcnt_input(int gpio_num,
                                           pcnt_unit_handle_t *unit_out,
                                           pcnt_channel_handle_t *channel_out)
{
    if (unit_out == NULL || channel_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    pcnt_unit_config_t unit_cfg = {
        .low_limit = -32768,
        .high_limit = 32767,
        .flags = {
            .accum_count = true,
        },
    };

    pcnt_unit_handle_t unit = NULL;
    esp_err_t ret = pcnt_new_unit(&unit_cfg, &unit);
    if (ret != ESP_OK) {
        return ret;
    }

    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = gpio_num,
        .level_gpio_num = -1,
    };

    pcnt_channel_handle_t channel = NULL;
    ret = pcnt_new_channel(unit, &chan_cfg, &channel);
    if (ret != ESP_OK) {
        pcnt_del_unit(unit);
        return ret;
    }

    ret = pcnt_channel_set_edge_action(channel,
                                       PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_HOLD);
    if (ret == ESP_OK) {
        ret = pcnt_channel_set_level_action(channel,
                                            PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                            PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    }
    if (ret == ESP_OK) {
        ret = pcnt_unit_add_watch_point(unit, unit_cfg.high_limit);
    }
    if (ret == ESP_OK) {
        ret = pcnt_unit_add_watch_point(unit, unit_cfg.low_limit);
    }
    if (ret == ESP_OK) {
        ret = pcnt_unit_enable(unit);
    }

    if (ret != ESP_OK) {
        pcnt_del_channel(channel);
        pcnt_del_unit(unit);
        return ret;
    }

    *unit_out = unit;
    *channel_out = channel;
    return ESP_OK;
}

static void cm108b_clock_probe_task(void *arg)
{
    (void)arg;

    pcnt_unit_handle_t bclk_unit = NULL;
    pcnt_unit_handle_t lrclk_unit = NULL;
    pcnt_channel_handle_t bclk_channel = NULL;
    pcnt_channel_handle_t lrclk_channel = NULL;

    esp_err_t ret = cm108b_create_pcnt_input(CONFIG_CM108B_BCLK_GPIO,
                                              &bclk_unit,
                                              &bclk_channel);
    if (ret == ESP_OK) {
        ret = cm108b_create_pcnt_input(CONFIG_CM108B_LRCLK_GPIO,
                                       &lrclk_unit,
                                       &lrclk_channel);
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "CM108B clock probe unavailable: %s. I2S RX diagnostics continue.",
                 esp_err_to_name(ret));
        if (bclk_channel != NULL) {
            pcnt_del_channel(bclk_channel);
        }
        if (bclk_unit != NULL) {
            pcnt_unit_disable(bclk_unit);
            pcnt_del_unit(bclk_unit);
        }
        s_clock_probe_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG,
             "CM108B clock probe armed on DASCLK GPIO%d and DALRCK GPIO%d",
             CONFIG_CM108B_BCLK_GPIO,
             CONFIG_CM108B_LRCLK_GPIO);

    while (s_running) {
        pcnt_unit_clear_count(bclk_unit);
        pcnt_unit_clear_count(lrclk_unit);
        pcnt_unit_start(bclk_unit);
        pcnt_unit_start(lrclk_unit);

        const int64_t start_us = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(CM108B_CLOCK_PROBE_WINDOW_MS));
        const int64_t end_us = esp_timer_get_time();

        pcnt_unit_stop(bclk_unit);
        pcnt_unit_stop(lrclk_unit);

        int bclk_count = 0;
        int lrclk_count = 0;
        pcnt_unit_get_count(bclk_unit, &bclk_count);
        pcnt_unit_get_count(lrclk_unit, &lrclk_count);

        const int64_t elapsed_us = end_us - start_us;
        const double bclk_hz = elapsed_us > 0
                                   ? ((double)bclk_count * 1000000.0) / (double)elapsed_us
                                   : 0.0;
        const double lrclk_hz = elapsed_us > 0
                                    ? ((double)lrclk_count * 1000000.0) / (double)elapsed_us
                                    : 0.0;
        const double ratio = lrclk_hz > 1.0 ? bclk_hz / lrclk_hz : 0.0;

        ESP_LOGI(TAG,
                 "CM108B CLOCK probe: DASCLK(GPIO%d)=%.0f Hz, "
                 "DALRCK(GPIO%d)=%.0f Hz, ratio=%.2f (expected ~64 at 48 kHz)",
                 CONFIG_CM108B_BCLK_GPIO,
                 bclk_hz,
                 CONFIG_CM108B_LRCLK_GPIO,
                 lrclk_hz,
                 ratio);

        if (bclk_hz < 100000.0 || lrclk_hz < 1000.0) {
            ESP_LOGW(TAG,
                     "CM108B CLOCK missing/invalid: expected DASCLK ~3.072 MHz "
                     "and DALRCK ~48 kHz while Samsung playback is active");
        }

        for (uint32_t waited = CM108B_CLOCK_PROBE_WINDOW_MS;
             s_running && waited < CM108B_CLOCK_PROBE_PERIOD_MS;
             waited += 100U) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    pcnt_unit_stop(bclk_unit);
    pcnt_unit_stop(lrclk_unit);
    pcnt_unit_disable(bclk_unit);
    pcnt_unit_disable(lrclk_unit);
    pcnt_del_channel(bclk_channel);
    pcnt_del_channel(lrclk_channel);
    pcnt_del_unit(bclk_unit);
    pcnt_del_unit(lrclk_unit);

    s_clock_probe_task = NULL;
    vTaskDelete(NULL);
}
#endif


#if CONFIG_CM108B_DIAG_1KHZ_TONE
/* 48 samples = exactly one 1 kHz period at 48 kHz. About -12 dBFS. */
static const int16_t s_diag_sine_1khz[48] = {
      0,  1069,  2121,  3136,  4096,  4985,  5793,  6506,
   7094,  7567,  7909,  8117,  8192,  8117,  7909,  7567,
   7094,  6506,  5793,  4985,  4096,  3136,  2121,  1069,
      0, -1069, -2121, -3136, -4096, -4985, -5793, -6506,
  -7094, -7567, -7909, -8117, -8192, -8117, -7909, -7567,
  -7094, -6506, -5793, -4985, -4096, -3136, -2121, -1069
};
#endif

/*
 * StreamBuffer is deliberately used with one producer and one consumer per
 * source. To preserve stereo-frame alignment, a producer only writes when the
 * complete chunk fits; partial writes are never allowed.
 */
static esp_err_t stream_send_complete(StreamBufferHandle_t stream,
                                      const void *data,
                                      size_t bytes,
                                      TickType_t timeout_ticks)
{
    if (stream == NULL || data == NULL || bytes == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const TickType_t start = xTaskGetTickCount();

    while (xStreamBufferSpacesAvailable(stream) < bytes) {
        if (timeout_ticks == 0U) {
            return ESP_ERR_TIMEOUT;
        }

        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(1);
    }

    const size_t sent = xStreamBufferSend(stream, data, bytes, 0);
    return (sent == bytes) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_mixer_submit_hfp(const int32_t *stereo_pcm, size_t frames)
{
    if (!s_running || s_hfp_fifo == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (stereo_pcm == NULL || frames == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t bytes = frames * MIX_CHANNELS * sizeof(int32_t);
    if (bytes > HFP_FIFO_BYTES) {
        ESP_LOGW(TAG, "HFP chunk too large for low-latency FIFO: %u bytes",
                 (unsigned)bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_err_t ret = stream_send_complete(s_hfp_fifo, stereo_pcm, bytes, 0);
    if (ret == ESP_ERR_TIMEOUT) {
        /*
         * HFP is live microphone monitoring. Once PCM is late, keeping it only
         * increases the audible voice delay. Keep this Bluetooth callback
         * non-blocking and ask the mixer (the StreamBuffer consumer) to discard
         * queued stale HFP PCM on its next 5 ms cycle.
         */
        s_hfp_flush_requested = true;
        ++s_hfp_drop_chunks;
        if (s_hfp_drop_chunks == 1U || (s_hfp_drop_chunks % 100U) == 0U) {
            ESP_LOGW(TAG,
                     "HFP FIFO full; dropped current chunk and requested stale-audio flush "
                     "(drops=%lu)",
                     (unsigned long)s_hfp_drop_chunks);
        }
    }
    return ret;
}

esp_err_t audio_mixer_submit_jingle(const int32_t *stereo_pcm,
                                    size_t frames,
                                    TickType_t timeout_ticks)
{
    if (!s_running || s_jingle_fifo == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (stereo_pcm == NULL || frames == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t bytes = frames * MIX_CHANNELS * sizeof(int32_t);
    if (bytes > JINGLE_FIFO_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    return stream_send_complete(s_jingle_fifo, stereo_pcm, bytes, timeout_ticks);
}

void audio_mixer_set_jingle_active(bool active)
{
    s_jingle_active = active;
}

void audio_mixer_clear_hfp(void)
{
    /*
     * Do not reset the StreamBuffer directly from the Bluetooth callback while
     * the mixer task may be receiving from it. Request a consumer-side flush
     * instead. The mixer performs the actual drain on its next 5 ms cycle.
     */
    s_hfp_flush_requested = true;
}

void audio_mixer_set_hfp_mic_gain(int gain)
{
    if (gain < 0) {
        gain = 0;
    } else if (gain > 15) {
        gain = 15;
    }

    s_hfp_mic_gain = gain;

    const int32_t factor_q15 = s_hfp_mic_gain_q15[gain];
    const int factor_percent = (int)(((int64_t)factor_q15 * 100 + 16384) / 32768);
    ESP_LOGI(TAG,
             "HFP microphone mixer gain: level=%d/15, factor=%d%% (level 10=100%%)",
             gain,
             factor_percent);
}

int audio_mixer_get_hfp_mic_gain(void)
{
    return s_hfp_mic_gain;
}

esp_err_t audio_mixer_wait_jingle_drained(TickType_t timeout_ticks)
{
    if (!s_running || s_jingle_fifo == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const TickType_t start = xTaskGetTickCount();
    while (xStreamBufferBytesAvailable(s_jingle_fifo) != 0U) {
        if (timeout_ticks == 0U ||
            (xTaskGetTickCount() - start) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    /* Allow the last 5 ms block already handed to I2S DMA to leave the FIFO. */
    vTaskDelay(pdMS_TO_TICKS(6));
    return ESP_OK;
}

static size_t receive_source(StreamBufferHandle_t stream,
                             int32_t *dst,
                             size_t dst_bytes)
{
    memset(dst, 0, dst_bytes);
    if (stream == NULL) {
        return 0U;
    }

    const size_t got = xStreamBufferReceive(stream, dst, dst_bytes, 0);
    if (got < dst_bytes) {
        memset(((uint8_t *)dst) + got, 0, dst_bytes - got);
    }
    return got;
}

static void usb_rx_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG,
             "CM108B RX task started: RAW Philips I2S, 32-bit stereo slots, "
             "64 BCLK/frame; software PCM extraction=%s",
#if CONFIG_CM108B_RAW_USE_LOWER16
             "LOWER16 [15:0]"
#else
             "UPPER16 [31:16]"
#endif
    );

    bool first_audio_logged = false;
    bool first_raw_logged = false;
    cm108b_rx_diag_window_t diag = {
        .start_us = esp_timer_get_time(),
    };

    while (s_running) {
        size_t filled = 0U;
        bool read_failed = false;

        /*
         * Receive the complete 32-bit wire slots. This bypasses the classic
         * ESP32 HW-v1 16-bit-in-32-bit FIFO packing behavior entirely.
         */
        while (s_running && filled < sizeof(s_usb_rx_raw)) {
            size_t got = 0U;
            const size_t remaining = sizeof(s_usb_rx_raw) - filled;
            const esp_err_t ret = i2s_channel_read(
                s_rx_chan,
                ((uint8_t *)s_usb_rx_raw) + filled,
                remaining,
                &got,
                I2S_RX_TIMEOUT_MS);

            if (ret == ESP_ERR_TIMEOUT || (ret == ESP_OK && got == 0U)) {
                ++s_usb_rx_timeout_count;
                ++diag.timeouts;
                read_failed = true;
                break;
            }

            if (ret != ESP_OK) {
                ++s_usb_rx_error_count;
                ++diag.errors;
                if ((s_usb_rx_error_count % 100U) == 1U) {
                    ESP_LOGW(TAG,
                             "CM108B I2S RX read failed: %s (errors=%lu)",
                             esp_err_to_name(ret),
                             (unsigned long)s_usb_rx_error_count);
                }
                read_failed = true;
                break;
            }

            if (got < remaining) {
                ++s_usb_rx_partial_reads;
                ++diag.partial_reads;
            }

            filled += got;
        }

        if (!s_running) {
            break;
        }

        if (read_failed || filled != sizeof(s_usb_rx_raw)) {
            ++s_usb_rx_incomplete_blocks;
            ++diag.incomplete;
            cm108b_log_rx_stats_if_due(&diag);
            continue;
        }

        ++s_usb_rx_full_blocks;
        ++diag.full_blocks;

        int32_t peak_selected = 0;
        int32_t peak_upper = 0;
        int32_t peak_lower = 0;

        for (size_t i = 0; i < MIX_FRAMES * MIX_CHANNELS; ++i) {
            const int32_t raw = s_usb_rx_raw[i];
            const int16_t upper = (int16_t)((uint32_t)raw >> 16);
            const int16_t lower = (int16_t)((uint32_t)raw & 0xFFFFU);
#if CONFIG_CM108B_RAW_USE_LOWER16
            const int16_t sample = lower;
#else
            const int16_t sample = upper;
#endif
            s_usb_rx_mix[i] = (int32_t)sample * 65536;

            int32_t mag_selected = sample;
            int32_t mag_upper = upper;
            int32_t mag_lower = lower;
            if (mag_selected < 0) mag_selected = -mag_selected;
            if (mag_upper < 0) mag_upper = -mag_upper;
            if (mag_lower < 0) mag_lower = -mag_lower;
            if (mag_selected > peak_selected) peak_selected = mag_selected;
            if (mag_upper > peak_upper) peak_upper = mag_upper;
            if (mag_lower > peak_lower) peak_lower = mag_lower;
        }

        if (!first_raw_logged) {
            ESP_LOGI(TAG,
                     "CM108B RAW first slots: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                     (unsigned long)(uint32_t)s_usb_rx_raw[0],
                     (unsigned long)(uint32_t)s_usb_rx_raw[1],
                     (unsigned long)(uint32_t)s_usb_rx_raw[2],
                     (unsigned long)(uint32_t)s_usb_rx_raw[3],
                     (unsigned long)(uint32_t)s_usb_rx_raw[4],
                     (unsigned long)(uint32_t)s_usb_rx_raw[5],
                     (unsigned long)(uint32_t)s_usb_rx_raw[6],
                     (unsigned long)(uint32_t)s_usb_rx_raw[7]);
            ESP_LOGI(TAG,
                     "CM108B RAW peaks: upper16=%ld lower16=%ld selected=%ld (%s)",
                     (long)peak_upper,
                     (long)peak_lower,
                     (long)peak_selected,
#if CONFIG_CM108B_RAW_USE_LOWER16
                     "LOWER16"
#else
                     "UPPER16"
#endif
            );
            first_raw_logged = true;
        }

        if (!first_audio_logged && peak_selected > 32) {
            ESP_LOGI(TAG,
                     "CM108B digital PCM detected (selected peak=%ld / 32767)",
                     (long)peak_selected);
            first_audio_logged = true;
        }

        if (xStreamBufferSpacesAvailable(s_usb_fifo) < sizeof(s_usb_rx_mix)) {
            ++s_usb_fifo_drops;
            ++diag.fifo_drops;
            if ((s_usb_fifo_drops % 100U) == 1U) {
                ESP_LOGW(TAG,
                         "CM108B FIFO full; dropped newest 5 ms block (drops=%lu)",
                         (unsigned long)s_usb_fifo_drops);
            }
        } else {
            const size_t sent = xStreamBufferSend(s_usb_fifo,
                                                  s_usb_rx_mix,
                                                  sizeof(s_usb_rx_mix),
                                                  0);
            if (sent != sizeof(s_usb_rx_mix)) {
                ++s_usb_fifo_drops;
                ++diag.fifo_drops;
            }
        }

        cm108b_log_rx_stats_if_due(&diag);
    }

    s_usb_rx_task = NULL;
    ESP_LOGI(TAG,
             "CM108B RX task stopped: full=%lu partial_reads=%lu timeouts=%lu "
             "errors=%lu incomplete=%lu fifo_drops=%lu underflows=%lu",
             (unsigned long)s_usb_rx_full_blocks,
             (unsigned long)s_usb_rx_partial_reads,
             (unsigned long)s_usb_rx_timeout_count,
             (unsigned long)s_usb_rx_error_count,
             (unsigned long)s_usb_rx_incomplete_blocks,
             (unsigned long)s_usb_fifo_drops,
             (unsigned long)s_usb_underflows);
    vTaskDelete(NULL);
}

static void mixer_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG,
             "Central mixer started: CM108B I2S1 RX slave BCLK=%d LRCLK=%d DATA=%d; "
             "USB + HFP + jingle -> 48 kHz TAS5805M",
             CONFIG_CM108B_BCLK_GPIO,
             CONFIG_CM108B_LRCLK_GPIO,
             CONFIG_CM108B_DATA_GPIO);

    while (s_running) {
        memset(s_usb_block, 0, sizeof(s_usb_block));

        /*
         * CM108B and TAS5805M are independent clock domains. The dedicated
         * usb_rx_task() blocks on the CM108B clock and fills a small FIFO.
         * This output task is paced only by TAS5805M TX. Never call I2S RX
         * directly from here: a zero-timeout read creates alternating audio /
         * silence whenever the two 48 kHz clocks are a few microseconds apart.
         */
        const size_t usb_available = xStreamBufferBytesAvailable(s_usb_fifo);

        if (!s_usb_streaming) {
            if (usb_available >= (USB_PREBUFFER_BLOCKS * MIX_BYTES_32)) {
                s_usb_streaming = true;
                ESP_LOGI(TAG, "CM108B playback buffered; USB source enabled");
            }
        }

        if (s_usb_streaming) {
            if (xStreamBufferBytesAvailable(s_usb_fifo) >= MIX_BYTES_32) {
                const size_t usb_got = xStreamBufferReceive(s_usb_fifo,
                                                             s_usb_block,
                                                             sizeof(s_usb_block),
                                                             0);
                if (usb_got != sizeof(s_usb_block)) {
                    memset(s_usb_block, 0, sizeof(s_usb_block));
                    ++s_usb_fifo_partial_reads;
                }
            } else {
                ++s_usb_underflows;
                s_usb_streaming = false;
                if ((s_usb_underflows % 20U) == 1U) {
                    ESP_LOGW(TAG,
                             "CM108B FIFO underflow; rebuffering 10 ms (underflows=%lu)",
                             (unsigned long)s_usb_underflows);
                }
            }
        }

#if CONFIG_CM108B_DIAG_1KHZ_TONE
        /* Replace only the USB source with a known-good local tone. If this is
         * clean, I2S0/TAS5805M/speaker are proven and the remaining fault is
         * strictly on CM108B I2S1 RX/framing. */
        static size_t diag_phase = 0U;
        for (size_t frame = 0; frame < MIX_FRAMES; ++frame) {
            const int32_t tone = (int32_t)s_diag_sine_1khz[diag_phase] * 65536;
            s_usb_block[frame * 2U] = tone;
            s_usb_block[frame * 2U + 1U] = tone;
            diag_phase = (diag_phase + 1U) % 48U;
        }
#endif

        if (s_hfp_flush_requested) {
            /*
             * Consumer-side latency recovery. Do not reset/read the HFP
             * StreamBuffer from the Bluetooth producer callback: FreeRTOS
             * StreamBuffer is kept in its intended single-producer /
             * single-consumer model.
             */
            uint8_t discard[128];
            size_t discarded_bytes = 0U;
            size_t discarded_now = 0U;
            do {
                discarded_now = xStreamBufferReceive(s_hfp_fifo,
                                                      discard,
                                                      sizeof(discard),
                                                      0);
                discarded_bytes += discarded_now;
            } while (discarded_now != 0U);

            s_hfp_flush_requested = false;
            ++s_hfp_latency_flushes;

            if (s_hfp_latency_flushes == 1U ||
                (s_hfp_latency_flushes % 25U) == 0U) {
                ESP_LOGW(TAG,
                         "HFP stale audio flushed for low latency "
                         "(bytes=%u, flushes=%lu)",
                         (unsigned)discarded_bytes,
                         (unsigned long)s_hfp_latency_flushes);
            }
        }

        receive_source(s_hfp_fifo, s_hfp_block, sizeof(s_hfp_block));
        receive_source(s_jingle_fifo, s_jingle_block, sizeof(s_jingle_block));

        const int source_duck = s_jingle_active
                                    ? CONFIG_AUDIO_MIX_JINGLE_DUCK_PERCENT
                                    : 100;

        int hfp_vgm = s_hfp_mic_gain;
        if (hfp_vgm < 0) {
            hfp_vgm = 0;
        } else if (hfp_vgm > 15) {
            hfp_vgm = 15;
        }
        const int32_t hfp_gain_q15 = s_hfp_mic_gain_q15[hfp_vgm];

        for (size_t i = 0U; i < MIX_FRAMES * MIX_CHANNELS; ++i) {
            /* CM108B int16_t PCM was promoted to the common 32-bit
             * full-scale mixer domain by usb_rx_task(). */
            const int32_t usb32 = s_usb_block[i];

            int64_t mixed = 0;
            mixed += ((int64_t)usb32 * CONFIG_AUDIO_MIX_USB_GAIN_PERCENT * source_duck) / 10000;
            mixed += ((int64_t)s_hfp_block[i] * CONFIG_AUDIO_MIX_HFP_GAIN_PERCENT *
                      source_duck * hfp_gain_q15) / (10000LL * 32768LL);
            mixed += ((int64_t)s_jingle_block[i] * CONFIG_AUDIO_MIX_JINGLE_GAIN_PERCENT) / 100;

            s_output_block[i] = clamp_i32(mixed);
        }

        size_t written = 0U;

        /*
         * IMPORTANT: ESP-IDF 5.3 i2s_channel_write() expects timeout_ms in
         * MILLISECONDS, not FreeRTOS ticks. With CONFIG_FREERTOS_HZ=100,
         * pdMS_TO_TICKS(20) evaluates to 2. Passing that value here meant
         * "2 ms", which the driver rounds to zero scheduler ticks and can
         * therefore return ESP_ERR_TIMEOUT in a tight loop.
         *
         * A 5 ms mixer block normally needs only one DMA completion. 50 ms is
         * ample scheduling margin and does not add normal audio latency; it is
         * only the maximum time to wait for a free TX DMA buffer.
         */
        const esp_err_t tx_ret = i2s_channel_write(s_tx_chan,
                                                   s_output_block,
                                                   sizeof(s_output_block),
                                                   &written,
                                                   I2S_TX_TIMEOUT_MS);
        if (tx_ret != ESP_OK && s_running) {
            ++s_tx_timeout_count;

            /* Never flood UART from a real-time task. The old per-block WARN
             * loop starved IDLE1 and eventually triggered the task watchdog. */
            if (s_tx_timeout_count == 1U || (s_tx_timeout_count % 100U) == 0U) {
                ESP_LOGW(TAG,
                         "TAS5805M I2S write failed: %s (written=%u/%u, timeouts=%lu)",
                         esp_err_to_name(tx_ret),
                         (unsigned)written,
                         (unsigned)sizeof(s_output_block),
                         (unsigned long)s_tx_timeout_count);
            }

            /* Defensive yield if TX is genuinely unhealthy. */
            vTaskDelay(1);
        }
    }

    ESP_LOGI(TAG,
             "Central mixer stopped (HFP drops=%lu latency_flushes=%lu; "
             "CM108B full=%lu partial_reads=%lu "
             "timeouts=%lu errors=%lu incomplete=%lu fifo_drops=%lu fifo_partial=%lu "
             "underflows=%lu; TX timeouts=%lu)",
             (unsigned long)s_hfp_drop_chunks,
             (unsigned long)s_hfp_latency_flushes,
             (unsigned long)s_usb_rx_full_blocks,
             (unsigned long)s_usb_rx_partial_reads,
             (unsigned long)s_usb_rx_timeout_count,
             (unsigned long)s_usb_rx_error_count,
             (unsigned long)s_usb_rx_incomplete_blocks,
             (unsigned long)s_usb_fifo_drops,
             (unsigned long)s_usb_fifo_partial_reads,
             (unsigned long)s_usb_underflows,
             (unsigned long)s_tx_timeout_count);

    s_mixer_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_mixer_start(i2s_chan_handle_t tx_chan)
{
    if (s_running) {
        return ESP_OK;
    }
    if (tx_chan == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_tx_chan = tx_chan;
    s_hfp_drop_chunks = 0U;
    s_hfp_latency_flushes = 0U;
    s_usb_rx_full_blocks = 0U;
    s_usb_rx_partial_reads = 0U;
    s_usb_rx_timeout_count = 0U;
    s_usb_rx_error_count = 0U;
    s_usb_rx_incomplete_blocks = 0U;
    s_usb_fifo_drops = 0U;
    s_usb_fifo_partial_reads = 0U;
    s_usb_underflows = 0U;
    s_tx_timeout_count = 0U;
    s_usb_streaming = false;
    s_jingle_active = false;
    s_hfp_flush_requested = false;
    audio_mixer_set_hfp_mic_gain(CONFIG_HFP_MIC_DEFAULT_GAIN);

    if (s_usb_fifo == NULL) {
        s_usb_fifo = xStreamBufferCreate(USB_FIFO_BYTES, 1);
        if (s_usb_fifo == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        xStreamBufferReset(s_usb_fifo);
    }

    if (s_hfp_fifo == NULL) {
        s_hfp_fifo = xStreamBufferCreate(HFP_FIFO_BYTES, 1);
        if (s_hfp_fifo == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        xStreamBufferReset(s_hfp_fifo);
    }

    if (s_jingle_fifo == NULL) {
        s_jingle_fifo = xStreamBufferCreate(JINGLE_FIFO_BYTES, 1);
        if (s_jingle_fifo == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        xStreamBufferReset(s_jingle_fifo);
    }

    /* CM108B is I2S master, therefore ESP32 I2S1 RX must be slave. */
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    chan_cfg.dma_frame_num = MIX_FRAMES;
    chan_cfg.dma_desc_num = 4;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not allocate I2S1 RX: %s", esp_err_to_name(ret));
        s_rx_chan = NULL;
        return ret;
    }

    /*
     * Diagnostic/raw baseline: receive the complete 32-bit slots from the
     * CM108B. This avoids all original-ESP32 HW-v1 16/32 FIFO packing rules.
     * The valid 16-bit PCM payload is extracted in usb_rx_task().
     */
    i2s_std_slot_config_t cm108b_slot =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_STEREO);
    cm108b_slot.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    cm108b_slot.ws_width = 32U;
    cm108b_slot.slot_mask = I2S_STD_SLOT_BOTH;
    cm108b_slot.bit_shift = true;
    cm108b_slot.msb_right = false;

    i2s_std_config_t rx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_MIXER_SAMPLE_RATE_HZ),
        .slot_cfg = cm108b_slot,
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_CM108B_BCLK_GPIO,
            .ws = CONFIG_CM108B_LRCLK_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_CM108B_DATA_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_LOGI(TAG,
             "CM108B I2S RX config: RAW Philips; data=32 bit; slot=32 bit; "
             "WS=32 BCLK; NORMAL BCLK; extract=%s%s",
#if CONFIG_CM108B_RAW_USE_LOWER16
             "LOWER16",
#else
             "UPPER16",
#endif
#if CONFIG_CM108B_DIAG_1KHZ_TONE
             "; INTERNAL 1 kHz USB-replacement tone ENABLED"
#else
             ""
#endif
    );

    ret = i2s_channel_init_std_mode(s_rx_chan, &rx_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not configure CM108B I2S RX: %s",
                 esp_err_to_name(ret));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not enable CM108B I2S RX: %s",
                 esp_err_to_name(ret));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    s_running = true;

#if CONFIG_CM108B_DIAG_CLOCK_PROBE
    BaseType_t probe_ret = xTaskCreatePinnedToCore(cm108b_clock_probe_task,
                                                   "cm108b_clk",
                                                   CM108B_CLOCK_PROBE_STACK_BYTES,
                                                   NULL,
                                                   5,
                                                   &s_clock_probe_task,
                                                   0);
    if (probe_ret != pdPASS) {
        s_clock_probe_task = NULL;
        ESP_LOGW(TAG, "Could not create CM108B clock probe task");
    }
#endif

    BaseType_t task_ret = xTaskCreatePinnedToCore(usb_rx_task,
                                                  "cm108b_rx",
                                                  USB_RX_TASK_STACK_BYTES,
                                                  NULL,
                                                  19,
                                                  &s_usb_rx_task,
                                                  1);
    if (task_ret != pdPASS) {
        s_running = false;
        i2s_channel_disable(s_rx_chan);
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        s_tx_chan = NULL;
        return ESP_ERR_NO_MEM;
    }

    task_ret = xTaskCreatePinnedToCore(mixer_task,
                                       "audio_mixer",
                                       AUDIO_MIXER_TASK_STACK_BYTES,
                                       NULL,
                                       20,
                                       &s_mixer_task,
                                       1);
    if (task_ret != pdPASS) {
        s_running = false;
        for (int i = 0; i < 10 && s_usb_rx_task != NULL; ++i) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        i2s_channel_disable(s_rx_chan);
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        s_tx_chan = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void audio_mixer_stop(void)
{
    if (!s_running && s_rx_chan == NULL) {
        return;
    }

    s_running = false;
    s_jingle_active = false;

    /* TX waits at most 50 ms and RX at most 20 ms. Let both tasks leave
     * their driver calls before deleting the I2S RX channel. */
    for (int i = 0; i < 60 && (s_mixer_task != NULL || s_usb_rx_task != NULL || s_clock_probe_task != NULL); ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (s_rx_chan != NULL) {
        i2s_channel_disable(s_rx_chan);
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
    }

    if (s_usb_fifo != NULL) {
        xStreamBufferReset(s_usb_fifo);
    }
    if (s_hfp_fifo != NULL) {
        xStreamBufferReset(s_hfp_fifo);
    }
    if (s_jingle_fifo != NULL) {
        xStreamBufferReset(s_jingle_fifo);
    }

    s_tx_chan = NULL;
}

bool audio_mixer_is_running(void)
{
    return s_running;
}
