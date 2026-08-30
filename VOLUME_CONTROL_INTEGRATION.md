# Potentiometer Volume Control Integration

## Overview
Your ESP32 HFP Audio Gateway now has integrated potentiometer volume control on GPIO39. The volume adjustment is applied directly to the incoming Bluetooth audio stream before it's sent to the PCM5100A DAC via I2S.

## Files Added/Modified

### New Files Created:
1. **`main/volume_control.h`** - Header file with API declarations
2. **`main/volume_control.c`** - Implementation with ADC reading and volume scaling

### Modified Files:
1. **`main/CMakeLists.txt`** - Added `volume_control.c` to build
2. **`main/main.c`** - Added volume control initialization in `app_main()`
3. **`main/bt_app_hf.c`** - Integrated volume scaling into audio callback

## How It Works

### Hardware Setup
- **GPIO39**: Potentiometer connected to ADC1_CH3
- **ADC Resolution**: 12-bit (0-4095 raw values)
- **Voltage Range**: 0-3.3V (full potentiometer range)
- **Volume Range**: 0-100% (mapped from ADC values)

### Software Architecture

#### 1. **ADC Reading (volume_control.c)**
   - Initializes ADC1 with 12-bit resolution
   - Reads potentiometer every 100ms
   - Hysteresis threshold of 2% prevents jitter from noise
   - No blocking calls - uses FreeRTOS task

#### 2. **Volume Application (audio pipeline)**
   - **Location**: `bt_app_hf_incoming_cb()` function
   - **Timing**: Applied to incoming Bluetooth audio immediately after reception
   - **Format**: 16-bit mono PCM (mSBC codec from Bluetooth HFP)
   - **Process**:
     ```
     Bluetooth Audio (16-bit) 
     → Get volume from potentiometer 
     → Apply scaling: output = (input * volume) / 100 
     → Resample to 32-bit stereo 
     → Send to I2S/PCM5100A
     ```

#### 3. **Initialization Sequence**
   ```
   app_main()
   └── Bluetooth HFP init
   └── Volume control init (ADC setup)
   └── Start volume monitoring task
   └── Start Bluetooth discovery
   ```

## API Functions

### `volume_control_init()`
Initializes ADC, configures GPIO39 channel, and sets up ADC calibration.
- **Return**: `ESP_OK` on success

### `volume_control_start_monitoring()`
Creates a FreeRTOS task that continuously reads the potentiometer and updates `current_volume`.
- **Update Frequency**: 100ms (10 Hz)
- **Return**: `ESP_OK` on success

### `volume_control_get_volume()`
Returns the current volume level (0-100).
- **Return**: Volume percentage (uint8_t)

### `volume_apply_to_samples_16bit(int16_t *samples, int num_samples, uint8_t volume)`
Applies volume scaling to 16-bit audio samples (used for incoming Bluetooth audio).
- **In-place operation**: Modifies buffer directly
- **Clipping**: Prevents overflow with saturation (±32767)

### `volume_apply_to_samples_32bit(int32_t *samples, int num_samples, uint8_t volume)`
Applies volume scaling to 32-bit audio samples (could be used for other audio paths).
- **Clipping**: Prevents overflow using 32-bit range

## Features

✅ **Real-time Volume Control**: Immediate response to potentiometer changes
✅ **Noise Filtering**: 2% hysteresis threshold reduces ADC noise
✅ **Efficient**: Integer arithmetic, no floating-point operations
✅ **Safe**: Proper overflow/underflow protection for audio samples
✅ **Non-blocking**: Volume monitoring runs in separate task
✅ **Low Latency**: Applied immediately in audio callback

## Testing Recommendations

1. **Compile and Flash**:
   ```bash
   idf.py build
   idf.py flash -p COMx
   idf.py monitor
   ```

2. **Monitor Volume Changes**:
   - Watch serial output for "Volume changed to: X%" messages
   - Rotate potentiometer and verify log messages

3. **Audio Test**:
   - Connect Bluetooth headset/speaker
   - Initiate audio stream (call or audio app)
   - Rotate potentiometer and verify audio level changes in real-time

## Technical Details

### ADC Configuration
- **Unit**: ADC1
- **Channel**: CH3 (GPIO39)
- **Bitwidth**: 12-bit
- **Attenuation**: DB_12 (0-3.3V full range)

### Volume Scaling Math
For 16-bit samples:
```
output = clamp(input * volume / 100, -32768, 32767)
```

For 32-bit samples:
```
output = clamp(input * volume / 100, -2147483648, 2147483647)
```

### Task Properties
- **Name**: "volume_monitor"
- **Stack Size**: 2048 bytes
- **Priority**: 5 (above normal)
- **Update Rate**: 100ms (10 Hz)

## Troubleshooting

### Volume not changing
1. Check GPIO39 is not used by other peripherals
2. Verify ADC initialization logs in serial output
3. Use voltmeter to confirm potentiometer output range (0-3.3V)

### Audio distortion
- Potentiometer may not be properly scaled
- Check that raw ADC values match expected range (0-4095)
- Verify sampling at 100ms intervals is sufficient

### Compilation errors
- Ensure all include paths are correct
- Check that `volume_control.h` is in `main/` directory
- Verify `CMakeLists.txt` includes `volume_control.c`

## Future Enhancements

Possible improvements:
1. Add minimum/maximum volume limits via configuration
2. Implement logarithmic volume curve (more natural to ear)
3. Add volume persistence to NVS flash memory
4. Add software volume adjustments via Bluetooth commands
5. Create console command to set/get volume directly

## References

- ESP32 ADC Driver: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/adc.html
- PCM5100A Datasheet: Stereo Audio DAC with integrated amplifier
- HFP Audio Data Path: mSBC codec at 16 kHz, 16-bit mono
