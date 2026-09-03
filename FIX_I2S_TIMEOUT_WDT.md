# I2S TX timeout / watchdog fix

Observed symptoms:

- loud crackling / knocking instead of clean audio
- repeated `AUDIO_MIXER: TAS5805M I2S write failed: ESP_ERR_TIMEOUT`
- eventually `task_wdt: IDLE1` while `audio_mixer` is running

## Root cause

ESP-IDF 5.3 `i2s_channel_write()` receives its final argument in **milliseconds**.
The mixer incorrectly passed `pdMS_TO_TICKS(20)`.

This project uses `CONFIG_FREERTOS_HZ=100`, so:

    pdMS_TO_TICKS(20) == 2

The I2S API interpreted that as a 2 ms timeout. Internally this can become zero
scheduler ticks at 100 Hz and lead to immediate repeated `ESP_ERR_TIMEOUT`
returns. The mixer then flooded the serial log and starved `IDLE1`, triggering
the task watchdog.

## Changes

- I2S TX timeout is now passed as `50U` milliseconds directly.
- TX timeout logging is rate-limited (first event and every 100th event).
- The mixer yields defensively after a real TX failure.
- I2S0 TX DMA is explicitly configured for 240 frames per descriptor (5 ms at
  48 kHz) and 6 DMA descriptors.
- `AUDIO_MIXER_BLOCK_FRAMES` is shared between mixer and TX initialization.

These changes do not add 50 ms of normal latency. 50 ms is only the maximum
blocking time if no TX DMA buffer becomes available; normal writes are released
by the regular ~5 ms DMA cadence.
