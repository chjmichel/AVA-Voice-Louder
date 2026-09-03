> **Superseded:** Modes A/B/C were diagnostic experiments. The current implementation uses fixed Mode D; see `FIX_CM108B_MODE_D.md`.

# CM108B raw-I2S RX diagnostic fix

This revision narrows the remaining distortion to the CM108B -> ESP32 I2S1
link. The CM108B analog headphone output is already clean, so USB/UAC/Android
are outside the remaining fault domain.

## Core change

I2S1 now receives the complete stereo wire frame as 32-bit raw slots:

- 48 kHz
- 2 slots per frame
- 32 bits per slot
- 64 BCLK per stereo frame

The 16-bit PCM is extracted in software instead of using the classic ESP32
HW-v1 16-bit-in-32-bit-slot FIFO packing path.

## menuconfig

`AVA Voice / Louder ESP32 Rev H6 -> CM108B USB audio input / central mixer`

### I2S framing / clock-edge modes

- Mode A: Philips I2S, normal BCLK (default)
- Mode B: MSB/left-justified, normal BCLK
- Mode C: Philips I2S, inverted BCLK

### 16-bit extraction modes

- Upper 16 bits [31:16] (default)
- Lower 16 bits [15:0]
- One-bit window [30:15]
- Opposite one-bit test [31:17]

Change one dimension at a time. Recommended sequence:

1. A + Upper16
2. B + Upper16
3. C + Upper16
4. If all remain distorted, return to the best framing mode and test [30:15]
5. Lower16 is mainly a diagnostic to prove where the payload sits

## Raw diagnostics

With `CM108B_DIAG_RAW_STATS=y`, the monitor prints:

- first eight raw 32-bit slots
- high16 average/peak
- low16 average/peak
- measured input sample rate

Paste those lines after playing a steady voice or music segment.

## Internal 1 kHz test

Enable `CM108B_DIAG_1KHZ_TONE` to replace only the USB source in the mixer with
a locally generated 1 kHz sine. HFP and jingle paths are unchanged.

If the 1 kHz tone is clean while CM108B audio is distorted, the complete
ESP32 I2S0 -> TAS5805M -> loudspeaker path is proven good and the remaining
problem is strictly CM108B I2S1 framing/edge/data alignment or electrical
signal integrity.
