# CM108B Mode E - inverted BCLK edge

This revision changes only the CM108B -> ESP32 I2S1 RX framing edge relative to Mode D.

Configuration:

- Philips I2S (`bit_shift = true`)
- 48 kHz stereo
- 16 valid PCM bits
- 32-bit wire slots / 64 BCLK per stereo frame
- `msb_right = false`
- **`bclk_inv = true`**
- WS/LRCLK is not inverted

The TAS5805M/I2S0 output, mixer, FIFO sizes, HFP path and sample rate are unchanged.

Expected boot log:

```text
AUDIO_MIXER: CM108B I2S RX config: Mode E; Philips; data=16 bit; slot=32 bit; WS=32 BCLK; msb_right=false; INVERTED BCLK
```

Use this build with the internal 1 kHz diagnostic tone disabled so the loudspeaker is fed from the real CM108B input.
