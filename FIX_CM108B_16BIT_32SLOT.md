# CM108B 16-bit PCM / 32-bit slot fix

## Problem

The CM108B playback interface uses 64 BCLK ticks per stereo frame (32 per
channel), while the integrated USB DAC/PCM path is 16-bit. Treating the entire
slot as 32 valid audio bits can distort the received samples.

## Fix

I2S1 RX is now configured as:

- ESP32 role: slave
- Philips I2S
- valid data width: 16 bit
- slot width: 32 bit
- WS width: 32 BCLK
- stereo
- BCLK GPIO18
- LRCLK GPIO19
- DATA GPIO23

ESP-IDF returns signed `int16_t` samples for this configuration. The CM108B RX
task explicitly promotes them to the mixer's signed 32-bit full-scale domain
before queuing them.

## Sample-rate diagnostic

The CM108B is the clock master, so the software now measures the real input
sample rate from completed RX blocks instead of assuming Android selected
48 kHz. Expect one of these messages after about two seconds of active clocks:

    AUDIO_MIXER: CM108B measured input rate: 48000.x Hz (48 kHz, ... blocks)

or

    AUDIO_MIXER: CM108B measured input rate: 44100.x Hz (44.1 kHz, ... blocks)

If 44.1 kHz is reported while the TAS5805M mixer consumes 48 kHz, the next
required change is an asynchronous 44.1 -> 48 kHz resampler / clock-domain
rate matcher. Do not compensate that mismatch by increasing FIFO size because
that only turns it into periodic underflow/overflow and latency.
