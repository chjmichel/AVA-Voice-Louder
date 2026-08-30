# AVA Voice - Louder ESP32 Rev H6 migration

This project variant targets **Sonocotta Louder ESP32 Rev H6** with the onboard
**TAS5805M** Class-D amplifier/DSP.

## H6 GPIO assignment used by this project

| Function | ESP32 GPIO | Notes |
|---|---:|---|
| I2S BCLK | 26 | TAS5805M SCLK |
| I2S WS/LRCLK | 25 | TAS5805M LRCLK |
| I2S DATA OUT | 22 | TAS5805M SDIN |
| I2C SDA | 21 | TAS5805M control |
| I2C SCL | 27 | TAS5805M control |
| AMP PWDN | 33 | TAS5805M PDN# |
| AMP FAULT | 34 | TAS5805M FAULT#, input-only GPIO |
| Volume potentiometer | **36** | ADC1_CH0, wiper input |
| RGB LED | 12 | Reserved by H6; no longer used as headset reset |
| IR input | 39 | Reserved by H6; not used for volume |

### Potentiometer wiring

Use a typical 10 kOhm linear potentiometer:

- outer terminal -> 3.3 V
- other outer terminal -> GND
- wiper -> **GPIO36**

Do not apply more than 3.3 V to GPIO36.

## TAS5805M / DSP startup

The H6 amplifier is not equivalent to the PCM5100A + TPA3110 path used by the
Amped board. The TAS5805M needs both I2S audio clocks and I2C control.

The revised sequence is:

1. Keep PWDN (GPIO33) low during boot.
2. Configure I2S on GPIO26/25/22.
3. Enable I2S and write silence so clocks are running.
4. Release PWDN and wait for the amplifier to settle.
5. Load a flat stereo TAS5805M DSP startup sequence over I2C (0x2D).
6. Apply the potentiometer volume through TAS5805M digital volume control.
7. Enter PLAY state.
8. On audio shutdown, enter Hi-Z before asserting PWDN.

This ordering is important because TAS5805M DSP/register configuration can be
ignored when it is sent before a stable I2S clock is present.

## Volume behavior

The original Amped version modified every 16-bit PCM sample in software. The H6
version no longer does that. It converts the logical volume to the TAS5805M
native 0.5 dB volume register.

The conversion preserves the previous linear-amplitude behavior:

- 100% logical volume = 0 dB DSP gain
- 60% logical volume = approximately -4.5 dB
- 50% logical volume = approximately -6 dB
- 0% = TAS5805M mute

The existing potentiometer response curve is retained. `VOLUME_MAX_PERCENT`
defaults to 60, so full potentiometer travel still has the same intended output
limit as the previous code.

## Speaker outputs

The firmware sends the HFP mono signal to **both left and right I2S channels**.
In the normal H6 stereo/BTL configuration, one speaker can therefore be connected
to either the LEFT or RIGHT speaker output and receives the same signal.

Do not connect one speaker between LEFT and RIGHT outputs unless the board and
DSP are deliberately configured for PBTL/mono mode.

## GPIO conflicts removed

The previous code used GPIO12 as a long-press reset input. On H6, GPIO12 is the
RGB LED signal, so the fixed GPIO12 reset function is disabled. The console
command `headset-reset` remains available. An external reset GPIO can be enabled
explicitly in menuconfig if required.

Legacy SDMMC recording is also disabled by default because the original ESP32
SDMMC default pins overlap Louder H6 peripherals. Re-enable it only after adding
an SD interface with a verified H6-safe pin mapping.

## Menuconfig

Open:

```text
idf.py menuconfig
```

Then use:

`AVA Voice / Louder ESP32 Rev H6`

The important defaults are already provided in `sdkconfig.defaults`.

## Expected boot/audio log

A healthy H6 audio connection should contain lines similar to:

```text
Louder H6 control prepared: I2C SDA=21 SCL=27, PWDN=33, FAULT=34, TAS5805M=0x2D
Volume control ready: GPIO36 -> ADC1_CH0 ...
TAS5805M I2S configured: BCLK=26 WS=25 DATA=22 ...
TAS5805M DSP ready, stereo BTL PLAY, FAULT#=1
DSP volume: ...
```

If `FAULT#=0` is reported, check PVDD, speaker wiring, shorts, and load impedance.
