# Quick Reference: Bluetooth Connection Commands

## Connection Workflow

```
┌─────────────────────────────────────────────────┐
│  1. Power on ESP32                              │
│  2. Wait for "Ready for connection" message     │
│  3. Run: scan  (optional - to find device)      │
│  4. Run: con   (connect to device)              │
│  5. Wait for "SLC established" message          │
│  6. Audio starts automatically                  │
└─────────────────────────────────────────────────┘
```

## Essential Commands

| Command | What It Does | When to Use |
|---------|--------------|-------------|
| `scan` | Find Bluetooth devices nearby | First time, or finding new device |
| `con` | Connect to configured device | After scan or when device known |
| `dis` | Disconnect from device | End call or switch device |
| `help` | List all commands | Learn more commands |

## Quick Tips

✅ **DO:**
- Wait for "Ready for connection" before typing `con`
- Wait for "HFP disconnected" before reconnecting
- Use `scan` to verify device is discoverable

❌ **DON'T:**
- Type `con` multiple times rapidly
- Connect from both ESP32 AND phone simultaneously
- Disconnect during "connecting..." phase

## Expected Output

### Successful Connection
```
hfp_ag> con
Initiating connection...
I BT_APP_HF: HFP connecting...
I BT_APP_HF: HFP Service Level Connection established
I BT_APP_HF: --Audio State connected
I I2S: PCM5100A configured: 32-bit Stereo @ 44100 Hz
```

### Successful Scan
```
hfp_ag> scan
Starting Bluetooth device discovery...
Scanning for 10 seconds...
I HF_AG_MAIN: Found target device name: Rockerz 258 Pro+
Device found. Use 'con' command to connect.
```

## Troubleshooting Quick Fixes

| Problem | Solution |
|---------|----------|
| "Already connected" | Check if device really connected, or run `dis` first |
| "Connection in progress" | Wait 10 seconds, then try again |
| No device found after scan | Ensure device is in pairing mode |
| Connection timeout | Run `dis`, wait 5 sec, then `con` again |
| Collision errors | Only connect from ONE side (ESP32 OR phone) |

## Device Configuration

Set your Bluetooth device name:
```bash
idf.py menuconfig
# Project configuration -> Bluetooth headset Name
# Default: "Rockerz 258 Pro+"
```

## Audio Configuration

I2S pins for PCM5100A (configurable in menuconfig):
- BCLK: GPIO 26
- LRCLK: GPIO 25  
- DOUT: GPIO 22

Format: 32-bit Stereo @ 44.1kHz

---
For detailed information, see: BLUETOOTH_FIX_GUIDE.md
