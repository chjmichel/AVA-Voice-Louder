# PCM5100A DAC + TPA3110D2 Amplifier Wiring Guide

## Overview
This guide shows how to connect the PCM5100A 32-bit Stereo DAC with TPA3110D2 D-Class amplifier to your ESP32.

## PCM5100A DAC Module Connections

### Power Connections
| PCM5100A Pin | Connect To |
|-------------|------------|
| VIN         | 3.3V (ESP32) |
| GND         | GND (ESP32) |

### I2S Audio Interface (ESP32 to PCM5100A)
| PCM5100A Pin | ESP32 GPIO | Signal | Description |
|-------------|-----------|---------|-------------|
| BCK         | GPIO 26   | BCLK    | Bit Clock |
| LCK         | GPIO 25   | LRCLK   | Left/Right Clock (Word Select) |
| DIN         | GPIO 22   | DOUT    | Serial Data Input |
| SCK         | GND       | -       | System Clock (tie to GND for BCK-based operation) |
| FLT         | 3.3V      | -       | Filter Select (Normal latency) |
| DEMP        | GND       | -       | De-emphasis off |
| XSMT        | 3.3V      | -       | Soft mute control (3.3V = unmuted) |
| FMT         | GND       | -       | Format Select (GND = I2S format) |

### Audio Output (PCM5100A to TPA3110D2)
| PCM5100A Pin | TPA3110D2 Pin | Signal |
|-------------|---------------|---------|
| OUTL        | INL+          | Left Channel Audio Output |
| AGND        | INL-          | Left Channel Ground |
| OUTR        | INR+          | Right Channel Audio Output |
| AGND        | INR-          | Right Channel Ground |

## TPA3110D2 Amplifier Connections

### Power
| TPA3110D2 Pin | Connect To |
|--------------|------------|
| VCC          | 8-26V DC Power Supply |
| GND          | Power Supply GND + ESP32 GND (common ground) |

### Speaker Outputs
| TPA3110D2 Pin | Connect To |
|--------------|------------|
| OUTL+        | Left Speaker (+) |
| OUTL-        | Left Speaker (-) |
| OUTR+        | Right Speaker (+) |
| OUTR-        | Right Speaker (-) |

**Note:** Use 4Ω to 8Ω speakers rated for appropriate wattage.

## Configuration Settings

### Default GPIO Configuration (can be changed via menuconfig)
```
I2S_BCLK_GPIO  = 26  (Bit Clock)
I2S_LRCLK_GPIO = 25  (Word Select)
I2S_DOUT_GPIO  = 22  (Data Out)
```

### Audio Specifications
- **Sample Rate:** 44.1 kHz
- **Bit Depth:** 32-bit
- **Channels:** Stereo (Left + Right)
- **Format:** I2S Philips Standard

### To Change GPIO Pins:
```bash
idf.py menuconfig
# Navigate to: Project configuration
```

## Important Notes

1. **Common Ground:** Ensure ESP32, PCM5100A, and TPA3110D2 share a common ground connection.

2. **Power Supply:** The TPA3110D2 requires 8-26V DC. Do NOT connect ESP32 5V pin to TPA3110D2 VCC.

3. **PCM5100A Configuration:**
   - FMT tied to GND selects I2S format
   - SCK tied to GND uses BCK as master clock
   - XSMT tied to 3.3V keeps audio unmuted

4. **Audio Quality:**
   - Use short wires between PCM5100A and TPA3110D2
   - Keep I2S signal wires away from power lines
   - Use shielded cables for speaker connections if possible

5. **Volume Control:** 
   - Volume is controlled via Bluetooth connection
   - TPA3110D2 may have a gain adjustment potentiometer

## Schematic Summary

```
ESP32          PCM5100A           TPA3110D2         Speakers
                                                    
GPIO26 ------> BCK                                  
GPIO25 ------> LCK                                  
GPIO22 ------> DIN                                  
3.3V --------> VIN                                  
              FLT <---- 3.3V                       
              XSMT <--- 3.3V                       
              FMT <---- GND                        
              SCK <---- GND                        
              DEMP <--- GND                        
                                                    
              OUTL ---------> INL+ -----> OUTL+ --> Left Speaker +
              AGND ---------> INL- -----> OUTL- --> Left Speaker -
              OUTR ---------> INR+ -----> OUTR+ --> Right Speaker +
              AGND ---------> INR- -----> OUTR- --> Right Speaker -
                                                    
GND <-------> GND <----------> GND                  
              
              8-26V DC -----> VCC
              Supply GND ---> GND
```

## Testing

1. Flash the firmware to ESP32
2. Connect Bluetooth from your phone/device
3. Play audio - you should hear stereo output through both speakers
4. Audio will also be recorded to SD card (if configured)

## Troubleshooting

- **No Audio:** Check all I2S connections, verify common ground
- **One Channel Only:** Check L/R connections, verify stereo configuration
- **Distorted Audio:** Check power supply voltage to TPA3110D2, reduce volume
- **Noisy Audio:** Ensure short signal wires, check for proper grounding
- **Amp Getting Hot:** Verify speaker impedance (4-8Ω), check ventilation

## Additional Resources

- PCM5100A Datasheet: [Texas Instruments](https://www.ti.com/product/PCM5100A)
- TPA3110D2 Datasheet: [Texas Instruments](https://www.ti.com/product/TPA3110D2)
- ESP-IDF I2S Documentation: [Espressif Docs](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/i2s.html)
