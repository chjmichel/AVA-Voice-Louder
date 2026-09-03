# AVA Coach: CM108B + HFP central mixer (correct-wiring baseline)

Target: ESP-IDF 5.3.1, classic ESP32, Sonocotta Louder H6 / TAS5805M.

## Wiring

CM108B -> ESP32 Louder H6:

- DASCLK / BCLK -> GPIO18
- DALRCK / LRCLK -> GPIO19
- SDOUT / DATA -> GPIO23
- GND -> GND
- DAMCLK / MCLK -> not connected

Existing Louder TAS5805M output:

- GPIO26 -> BCLK
- GPIO25 -> LRCLK
- GPIO22 -> DATA OUT

## CM108B I2S input format

The receive path is now fixed to the **correct-wiring baseline**:

- Philips I2S (`bit_shift=true`)
- CM108B is master; ESP32 I2S1 is slave
- 48 kHz stereo
- 16 valid PCM bits per channel
- 32-bit wire slot per channel / 64 BCLK per stereo frame
- WS width = 32 BCLK
- `msb_right=true` for the original ESP32 I2S HW-v1
- normal BCLK polarity

The DMA receive buffer is `int16_t`; received PCM is promoted into the 32-bit
central mixer domain before mixing with HFP and the AVA jingle.

## Architecture

Samsung A16 -> powered USB hub -> CM108B -> I2S1 RX (ESP32 slave)
                                            |
Bluetooth HFP -> HFP FIFO -----------------+-> central mixer -> I2S0 TX
                                            |                  -> TAS5805M
AVA jingle ---------------------------------+

The central audio engine is independent of HFP connection state.

## RX diagnostics

Enable `CONFIG_CM108B_DIAG_RX_STATS` (default: enabled). Every ~10 s the monitor
prints a line similar to:

    AUDIO_MIXER: CM108B RX stats/10s: full_blocks=2000 partial_reads=0 timeouts=0 errors=0 incomplete=0 fifo_drops=0 mixer_underflows_total=0 fifo_level=2/4 blocks

For a healthy continuous 48 kHz stream, `full_blocks` should be approximately
2000 per 10 seconds because each block contains 5 ms of audio.

The former `CM108B measured input rate` diagnostic has intentionally been
removed. It measured successful software blocks, not the physical LRCLK.

## Optional 1 kHz isolation test

`CONFIG_CM108B_DIAG_1KHZ_TONE=y` replaces only the CM108B source with an
internal 1 kHz sine. Keep it disabled for normal CM108B testing. A clean test
tone proves mixer -> I2S0 -> TAS5805M -> speaker.

## Build

Use ESP-IDF 5.3.1:

    idf.py fullclean
    idf.py reconfigure
    idf.py build
    idf.py flash monitor

## BCLK sampling edge

With the corrected hardware wiring (`DASCLK -> GPIO18`), the CM108B RX path uses `bclk_inv = false` together with Philips I2S, 16 valid PCM bits in 32-bit slots and `msb_right = true`.


## Current diagnostic baseline: clock probe + RAW32 RX

The corrected wiring baseline now receives complete 32-bit I2S slots and extracts PCM in software.
The ESP32 PCNT peripheral also measures DASCLK and DALRCK directly.
Expected at 48 kHz: DASCLK ~3.072 MHz, DALRCK ~48 kHz, ratio ~64.
RX statistics are emitted every 2 seconds even when no full DMA block is received.
