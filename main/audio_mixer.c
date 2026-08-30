#include "audio_mixer.h"

#include <limits.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO_MIXER";

/* 120 stereo frames are 2.5 ms at 48 kHz; three chunks match a 7.5 ms HFP block. */
#define MIX_CHUNK_FRAMES          120U
#define MIX_TASK_STACK_SIZE       4096U
#define MIX_TASK_PRIORITY         8U
#define MIX_I2S_TIMEOUT_MS        100U

/*
 * FIFO sizes are powers of two to keep the SPSC implementation inexpensive.
 * HFP:     512 frames = 10.7 ms maximum queued live audio.
 * Jingle: 2048 frames = 42.7 ms, allowing its producer task to run in bursts.
 */
#define HFP_FIFO_FRAMES           512U
#define JINGLE_FIFO_FRAMES        2048U
#define HFP_FIFO_MASK             (HFP_FIFO_FRAMES - 1U)
#define JINGLE_FIFO_MASK          (JINGLE_FIFO_FRAMES - 1U)

#define HFP_DUCK_NUMERATOR        1LL
#define HFP_DUCK_DENOMINATOR      5LL   /* 20 percent while jingle is active */

typedef struct {
    int32_t *storage;
    uint32_t capacity_frames;
    uint32_t mask;
    volatile uint32_t head;
    volatile uint32_t tail;
} stereo_spsc_fifo_t;

static i2s_chan_handle_t s_tx_chan = NULL;
static volatile bool s_running = false;
static volatile bool s_jingle_active = false;
static TaskHandle_t s_mixer_task = NULL;

/* Static internal DRAM. No PSRAM is used in the live playback path. */
static int32_t s_hfp_storage[HFP_FIFO_FRAMES * 2U];
static int32_t s_jingle_storage[JINGLE_FIFO_FRAMES * 2U];
static stereo_spsc_fifo_t s_hfp_fifo = {
    .storage = s_hfp_storage,
    .capacity_frames = HFP_FIFO_FRAMES,
    .mask = HFP_FIFO_MASK,
};
static stereo_spsc_fifo_t s_jingle_fifo = {
    .storage = s_jingle_storage,
    .capacity_frames = JINGLE_FIFO_FRAMES,
    .mask = JINGLE_FIFO_MASK,
};

static int32_t s_hfp_chunk[MIX_CHUNK_FRAMES * 2U];
static int32_t s_jingle_chunk[MIX_CHUNK_FRAMES * 2U];
static int32_t s_mix_chunk[MIX_CHUNK_FRAMES * 2U];

static uint32_t s_hfp_drop_blocks = 0U;
static uint32_t s_jingle_drop_blocks = 0U;

static inline uint32_t fifo_load_head(const stereo_spsc_fifo_t *fifo)
{
    return __atomic_load_n(&fifo->head, __ATOMIC_ACQUIRE);
}

static inline uint32_t fifo_load_tail(const stereo_spsc_fifo_t *fifo)
{
    return __atomic_load_n(&fifo->tail, __ATOMIC_ACQUIRE);
}

static inline void fifo_store_head(stereo_spsc_fifo_t *fifo, uint32_t value)
{
    __atomic_store_n(&fifo->head, value, __ATOMIC_RELEASE);
}

static inline void fifo_store_tail(stereo_spsc_fifo_t *fifo, uint32_t value)
{
    __atomic_store_n(&fifo->tail, value, __ATOMIC_RELEASE);
}

static void fifo_reset(stereo_spsc_fifo_t *fifo)
{
    fifo_store_tail(fifo, 0U);
    fifo_store_head(fifo, 0U);
}

static uint32_t fifo_available(const stereo_spsc_fifo_t *fifo)
{
    const uint32_t head = fifo_load_head(fifo);
    const uint32_t tail = fifo_load_tail(fifo);
    return head - tail;
}

static uint32_t fifo_free(const stereo_spsc_fifo_t *fifo)
{
    const uint32_t used = fifo_available(fifo);
    return (used >= fifo->capacity_frames) ? 0U : (fifo->capacity_frames - used);
}

/* Single producer, single consumer. Publishes head only after all PCM is copied. */
static bool fifo_push_all(stereo_spsc_fifo_t *fifo, const int32_t *pcm, size_t frames)
{
    if (frames == 0U) {
        return true;
    }
    if (pcm == NULL || frames > fifo->capacity_frames || fifo_free(fifo) < frames) {
        return false;
    }

    const uint32_t head = fifo_load_head(fifo);
    for (size_t frame = 0; frame < frames; ++frame) {
        const uint32_t dst_frame = (head + (uint32_t)frame) & fifo->mask;
        fifo->storage[dst_frame * 2U] = pcm[frame * 2U];
        fifo->storage[dst_frame * 2U + 1U] = pcm[frame * 2U + 1U];
    }
    fifo_store_head(fifo, head + (uint32_t)frames);
    return true;
}

/* Pop up to requested frames and zero-fill the remainder for real-time mixing. */
static size_t fifo_pop_partial_with_silence(stereo_spsc_fifo_t *fifo,
                                    int32_t *dst,
                                    size_t requested_frames)
{
    const uint32_t head = fifo_load_head(fifo);
    const uint32_t tail = fifo_load_tail(fifo);
    const uint32_t available = head - tail;
    size_t frames = requested_frames;
    if (frames > available) {
        frames = available;
    }

    for (size_t frame = 0; frame < frames; ++frame) {
        const uint32_t src_frame = (tail + (uint32_t)frame) & fifo->mask;
        dst[frame * 2U] = fifo->storage[src_frame * 2U];
        dst[frame * 2U + 1U] = fifo->storage[src_frame * 2U + 1U];
    }

    if (frames < requested_frames) {
        memset(dst + frames * 2U,
               0,
               (requested_frames - frames) * 2U * sizeof(int32_t));
    }

    if (frames > 0U) {
        fifo_store_tail(fifo, tail + (uint32_t)frames);
    }
    return frames;
}

/*
 * HFP is packetized. Never consume a partial 2.5 ms mixer chunk and pad the
 * rest with zeroes, because that would introduce a gap inside the speech
 * stream. Wait for a complete 120-frame chunk instead.
 */
static bool fifo_pop_full_or_silence(stereo_spsc_fifo_t *fifo,
                                     int32_t *dst,
                                     size_t requested_frames)
{
    const uint32_t head = fifo_load_head(fifo);
    const uint32_t tail = fifo_load_tail(fifo);
    const uint32_t available = head - tail;
    if (available < requested_frames) {
        memset(dst, 0, requested_frames * 2U * sizeof(int32_t));
        return false;
    }

    for (size_t frame = 0; frame < requested_frames; ++frame) {
        const uint32_t src_frame = (tail + (uint32_t)frame) & fifo->mask;
        dst[frame * 2U] = fifo->storage[src_frame * 2U];
        dst[frame * 2U + 1U] = fifo->storage[src_frame * 2U + 1U];
    }
    fifo_store_tail(fifo, tail + (uint32_t)requested_frames);
    return true;
}

static inline int32_t saturate_i64_to_i32(int64_t value)
{
    if (value > INT32_MAX) {
        return INT32_MAX;
    }
    if (value < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)value;
}

static void audio_mixer_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Single I2S writer started: 48 kHz stereo, %u-frame / 2.5 ms chunks",
             MIX_CHUNK_FRAMES);

    while (s_running) {
        fifo_pop_full_or_silence(&s_hfp_fifo, s_hfp_chunk, MIX_CHUNK_FRAMES);
        fifo_pop_partial_with_silence(&s_jingle_fifo, s_jingle_chunk, MIX_CHUNK_FRAMES);

        const bool duck_hfp = __atomic_load_n(&s_jingle_active, __ATOMIC_ACQUIRE);
        for (size_t i = 0; i < MIX_CHUNK_FRAMES * 2U; ++i) {
            int64_t hfp = s_hfp_chunk[i];
            if (duck_hfp) {
                hfp = (hfp * HFP_DUCK_NUMERATOR) / HFP_DUCK_DENOMINATOR;
            }
            const int64_t mixed = hfp + (int64_t)s_jingle_chunk[i];
            s_mix_chunk[i] = saturate_i64_to_i32(mixed);
        }

        size_t bytes_written = 0U;
        esp_err_t ret = i2s_channel_write(s_tx_chan,
                                          s_mix_chunk,
                                          sizeof(s_mix_chunk),
                                          &bytes_written,
                                          MIX_I2S_TIMEOUT_MS);
        if (ret != ESP_OK) {
            if (s_running) {
                ESP_LOGE(TAG, "I2S writer failed: %s", esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        } else if (bytes_written != sizeof(s_mix_chunk)) {
            ESP_LOGW(TAG, "Short I2S write: %u/%u bytes",
                     (unsigned)bytes_written, (unsigned)sizeof(s_mix_chunk));
        }
    }

    s_mixer_task = NULL;
    ESP_LOGI(TAG, "Single I2S writer stopped");
    vTaskDelete(NULL);
}

esp_err_t audio_mixer_start(i2s_chan_handle_t tx_chan)
{
    if (tx_chan == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return (s_tx_chan == tx_chan) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    s_tx_chan = tx_chan;
    s_jingle_active = false;
    s_hfp_drop_blocks = 0U;
    s_jingle_drop_blocks = 0U;
    fifo_reset(&s_hfp_fifo);
    fifo_reset(&s_jingle_fifo);

    s_running = true;
    BaseType_t task_ret = xTaskCreate(audio_mixer_task,
                                      "audio_mixer",
                                      MIX_TASK_STACK_SIZE,
                                      NULL,
                                      MIX_TASK_PRIORITY,
                                      &s_mixer_task);
    if (task_ret != pdPASS) {
        s_running = false;
        s_mixer_task = NULL;
        s_tx_chan = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_mixer_stop(void)
{
    if (!s_running) {
        fifo_reset(&s_hfp_fifo);
        fifo_reset(&s_jingle_fifo);
        s_jingle_active = false;
        return;
    }

    s_running = false;
    s_jingle_active = false;

    /* One mixer write is only 2.5 ms; use a bounded wait before I2S disable. */
    for (int i = 0; i < 30 && s_mixer_task != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (s_mixer_task != NULL) {
        ESP_LOGW(TAG, "Mixer task did not stop in time; forcing task deletion");
        vTaskDelete(s_mixer_task);
        s_mixer_task = NULL;
    }

    fifo_reset(&s_hfp_fifo);
    fifo_reset(&s_jingle_fifo);
    s_tx_chan = NULL;
}

esp_err_t audio_mixer_submit_hfp(const int32_t *stereo_pcm, size_t frames)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (stereo_pcm == NULL || frames == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!fifo_push_all(&s_hfp_fifo, stereo_pcm, frames)) {
        ++s_hfp_drop_blocks;
        if (s_hfp_drop_blocks == 1U || (s_hfp_drop_blocks % 100U) == 0U) {
            ESP_LOGW(TAG, "HFP mixer FIFO full; dropped complete block (drops=%lu)",
                     (unsigned long)s_hfp_drop_blocks);
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_mixer_submit_jingle(const int32_t *stereo_pcm,
                                    size_t frames,
                                    TickType_t timeout_ticks)
{
    if (stereo_pcm == NULL || frames == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const TickType_t start = xTaskGetTickCount();
    while (s_running) {
        if (fifo_push_all(&s_jingle_fifo, stereo_pcm, frames)) {
            return ESP_OK;
        }
        if (timeout_ticks == 0U || (xTaskGetTickCount() - start) >= timeout_ticks) {
            ++s_jingle_drop_blocks;
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_ERR_INVALID_STATE;
}

void audio_mixer_set_jingle_active(bool active)
{
    __atomic_store_n(&s_jingle_active, active, __ATOMIC_RELEASE);
    if (!active) {
        /* Drop any stale tail so HFP resumes immediately after an aborted jingle. */
        const uint32_t head = fifo_load_head(&s_jingle_fifo);
        fifo_store_tail(&s_jingle_fifo, head);
    }
}

esp_err_t audio_mixer_wait_jingle_drained(TickType_t timeout_ticks)
{
    const TickType_t start = xTaskGetTickCount();
    while (s_running) {
        if (fifo_available(&s_jingle_fifo) == 0U) {
            return ESP_OK;
        }
        if (timeout_ticks == 0U || (xTaskGetTickCount() - start) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_ERR_INVALID_STATE;
}

bool audio_mixer_is_running(void)
{
    return __atomic_load_n(&s_running, __ATOMIC_ACQUIRE);
}
