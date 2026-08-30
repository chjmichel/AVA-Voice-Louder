# AVA Voice - Louder ESP32 Rev H6 build

> **Current target:** Sonocotta **Louder ESP32 Rev H6 / TAS5805M**.  
> The older PCM5100A/TPA3110 and generic MAX98357A sections further down are
> retained as historical documentation. For the active H6 pinout, DSP startup,
> potentiometer on **GPIO36**, and migration notes, read `LOUDER_H6_MIGRATION.md`.

The H6 defaults are I2S **BCLK 26 / WS 25 / DATA 22**, TAS5805M I2C
**SDA 21 / SCL 27**, **PWDN 33**, **FAULT# 34**, and volume ADC **GPIO36**.
The TAS5805M is initialized only after I2S clocks are active and volume is
controlled in the hardware DSP rather than by scaling PCM samples in software.

---

### **ESP32 Hands-Free Audio Gateway (HF-AG)**
---

| Supported Targets | ESP32 |
| ---------------- | ----- |

This project implements a **Hands-Free Audio Gateway (HF-AG)** application using **ESP32 Bluetooth Classic**. The device automatically connects to a Bluetooth headset with the name configured in **menuconfig**, receives microphone audio data, and outputs the audio to an **I2S audio amplifier** like MAX98357A.

If an **SD card** is present, the device stores the audio continuously in the **`Root`** directory on the SD card, rotating between two WAV files every **30 minutes**.

Please use IDF Version 5.3.1 - all others are not working. 

---

## **Features**

- Automatic Bluetooth headset connection by name
- Audio playback via **I2S**
- Audio recording to **SD Card**
- Automatic **File Rotation** every 30 minutes (FileA → FileB → Overwrite)
- Resampling from **16 kHz (mSBC)** to **44100 Hz** for I2S Output
- Optional **SD Card Formatting**
- Configurable via **menuconfig**
- Simple **Console Command Interface**

---

## **Hardware Required**

| Component           | Example Part      |
|------------------|------------------|
| ESP32 Board       | ESP32 DevKitC |
| Audio Amplifier   | MAX98357A |
| Bluetooth Headset | Any Hands-Free Profile (HFP) supported headset |
| SD Card          | 4GB or higher (formatted in FAT32) |
| MicroSD Card Adapter | For SPI or SDMMC Interface |

---

## **Folder Structure**
```
/
├─ FileA.wav     (30 min Audio Recording)
└─ FileB.wav     (30 min Audio Recording - Overwritten every hour)
```

---

---

## **Configuration**

### Open Project Configuration
```bash
idf.py menuconfig
```
In **Project Configuration → Hands-Free Gateway**, you can configure:

| Setting             | Description                  | Default   |
|------------------|---------------------------|---------|
| I2S BCLK GPIO    | I2S Clock Pin           | 19      |
| I2S LRCLK GPIO   | I2S WS Pin             | 23      |
| I2S DOUT GPIO    | I2S Data Pin           | 18      |
| Headset Name     | Bluetooth headset name to search | Rockerz 258 Pro+ |
| SD Card Bus Width | 1-bit or 4-bit mode    | 1-bit   |
| Format SD Card   | Automatically format if mount fails | Disabled |

---

---

## **Build and Flash**

1. Build the project:
```bash
idf.py build
```
2. Flash the project to the ESP32:
```bash
idf.py -p PORT flash
```
(Replace **PORT** with your device port)

3. Open the monitor:
```bash
idf.py monitor
```
---

---

## **SD Card Storage**

If an **SD card** is inserted, audio is stored automatically:

| File Name | Duration | Action    |
|-----------|----------|----------|
| FileA.wav | 30 min   | Saved First |
| FileB.wav | 30 min   | Saved Second |
| FileA.wav | 30 min   | Overwritten |

---

---

## **Console Commands**

| Command | Description                      |
|--------|--------------------------------|
| `con`  | Connect to Bluetooth Headset    |
| `dis`  | Disconnect Headset             |
| `cona` | Start Audio Stream            |
| `disa` | Stop Audio Stream             |
| `vu 0 <vol>` | Set Speaker Volume (0-15)   |
| `vu 1 <vol>` | Set Microphone Volume (0-15) |

---

---

## **Audio Playback**
The incoming audio will automatically play on:
  **I2S Output (MAX98357A Amplifier)**

### Default Pinout
| Function | GPIO |
|---------|------|
| I2S BCLK | 19   |
| I2S LRCLK | 23   |
| I2S DOUT | 18   |


---

---

## **File Rotation System**

| File Name | Duration | Action       |
|-----------|----------|------------|
| FileA.wav | 30 min   | Start Recording |
| FileB.wav | 30 min   | Rotate File |
| FileA.wav | 30 min   | **Overwrite** |
| FileB.wav | 30 min   | **Overwrite** |

---

---

## **Known Limitations**
- Only two WAV files are stored on the SD card at a time.
- Audio is stored as **PCM 16-bit WAV** without compression.

---

---

## **Future Improvements**
- MP3 Compression (Helix MP3)
- Automatic File Deletion if the card is full
---

---

## **Tested Headsets**
| Headset Name       | Bluetooth Version | Works |
|------------------|------------------|-------|
| Rockerz 258 Pro+ | 5.0             | ✅ Yes |
---

---

## **How to Play Recorded Audio**
All files are stored in:
```
Root
```
Audio is recorded in **16-bit PCM WAV** format (16 kHz Mono). You can play files using:
- VLC Media Player
- Audacity
- Any WAV player

---

---

## **Troubleshooting**
| Issue                | Solution                   |
|--------------------|--------------------------|
| Headset Not Found   | Check headset name in `menuconfig` |
| Audio Lag          | Use **SDMMC 4-bit Mode** |
| Overflow Logs       | Increase Ringbuffer Size |
| SD Card Not Detected | Use **Class 10 or UHS-1 Card** |

---

---

## **Credits**

This project is based on the **ESP-IDF HFP-AG Example** and customized to support:
- Bluetooth Hands-Free Gateway
- SD Card Recording
- I2S Audio Output


---
