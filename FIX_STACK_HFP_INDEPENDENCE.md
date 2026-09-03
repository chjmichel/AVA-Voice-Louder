# AVA Louder H6 - stack overflow and HFP independence fix

## Fixed issues

1. `audio_mixer` stack overflow
   - Large 5 ms PCM buffers were moved from the task stack to static DRAM.
   - Mixer task stack increased from 4096 to 6144 bytes.

2. CM108B audio no longer depends on HFP
   - The central audio engine starts during normal application boot, before Bluetooth discovery.
   - I2S0 TX, TAS5805M, CM108B I2S1 RX and the central mixer remain active when HFP/SCO disconnects.
   - HFP disconnect now only flushes queued HFP PCM.

3. Stale HFP samples
   - Added `audio_mixer_clear_hfp()`.
   - Flush is executed by the mixer consumer task to avoid resetting a StreamBuffer concurrently from the Bluetooth callback.

## Expected boot order

- TAS5805M control prepared
- Bluetooth HFP initialized
- Volume control initialized
- `Starting central audio engine (CM108B + mixer + TAS5805M)...`
- I2S0 configured/enabled
- TAS5805M ready
- `Central mixer started: CM108B I2S1 RX slave BCLK=18 LRCLK=19 DATA=23...`
- Bluetooth discovery starts

The `Central mixer started` line must now appear even if no headset is connected.

## HFP disconnect behavior

An HFP or SCO disconnect must NOT produce `AUDIO: Shutting down audio...`.
The mixer, CM108B input and TAS5805M output continue running.

## Build

Use ESP-IDF 5.3.1:

    idf.py fullclean
    idf.py reconfigure
    idf.py build
    idf.py flash monitor
