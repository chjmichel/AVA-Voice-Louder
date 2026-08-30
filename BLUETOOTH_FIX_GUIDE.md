# Bluetooth Connection Fix Guide

## Problem Summary

The original code had Bluetooth connection collision issues causing repeated connection failures with these errors:
```
W BT_HCI: btu_hcif_hdl_command_status,opcode:0x0405,status:0x0b
W BT_APPL: AG found collision (ACL) ...
W BT_RFCOMM: rfc_find_lcid_mcb LCID reused LCID:0x42
W BT_HCI: hcif disc complete: hdl 0x80, rsn 0x13
```

**Root Causes:**
1. **ACL Collision** - Auto-discovery conflicted with manual connection attempts
2. **No State Management** - No tracking of connection status leading to repeated attempts
3. **Missing Disconnect Handling** - Connection state not reset properly on disconnection

## Changes Made

### 1. Connection State Management (`app_hf_msg_set.c`)

Added global state tracking variables:
```c
static bool is_connected = false;
static bool connection_in_progress = false;
```

These prevent collision by ensuring only one connection attempt at a time.

### 2. Improved Connect Command (`con`)

**Before:**
```c
int hf_ag_connect_handler(int argn, char **argv) {
    printf("Connect.\n");
    esp_hf_ag_slc_connect(hf_peer_addr);
    return 0;
}
```

**After:**
```c
int hf_ag_connect_handler(int argn, char **argv) {
    if (is_connected) {
        printf("Already connected.\n");
        return 0;
    }
    if (connection_in_progress) {
        printf("Connection already in progress. Please wait...\n");
        return 0;
    }
    printf("Initiating connection...\n");
    connection_in_progress = true;
    esp_hf_ag_slc_connect(hf_peer_addr);
    return 0;
}
```

### 3. Enhanced Disconnect Command (`dis`)

Added proper state reset:
```c
int hf_disc_handler(int argn, char **argv) {
    if (!is_connected && !connection_in_progress) {
        printf("Not connected.\n");
        return 0;
    }
    printf("Disconnecting...\n");
    is_connected = false;
    connection_in_progress = false;
    esp_hf_ag_slc_disconnect(hf_peer_addr);
    return 0;
}
```

### 4. Connection State Callback (`bt_app_hf.c`)

Enhanced state tracking in `ESP_HF_CONNECTION_STATE_EVT` handler:

```c
case ESP_HF_CONNECTION_STATE_EVT:
{
    extern bool is_connected;
    extern bool connection_in_progress;
    
    switch (param->conn_stat.state) {
    case ESP_HF_CONNECTION_STATE_DISCONNECTED:
        is_connected = false;
        connection_in_progress = false;
        break;
        
    case ESP_HF_CONNECTION_STATE_CONNECTING:
        connection_in_progress = true;
        break;
        
    case ESP_HF_CONNECTION_STATE_SLC_CONNECTED:
        is_connected = true;
        connection_in_progress = false;
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait before audio connection
        esp_hf_ag_audio_connect(hf_peer_addr);
        break;
        
    // ... other states
    }
}
```

### 5. Disabled Auto-Discovery (`main.c`)

**Before:**
```c
esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
ESP_LOGI(BT_HF_TAG, "Starting device discovery...");
esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
```

**After:**
```c
esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
ESP_LOGI(BT_HF_TAG, "Ready for connection. Use 'con' command to connect.");
// Auto-discovery disabled to prevent collision
```

### 6. Removed Auto-Connect from Discovery

Device discovery no longer automatically initiates connection when target device is found. This prevents collision when the remote device also tries to connect.

**Before:**
```c
if (strcmp(peer_bdname, remote_device_name) == 0) {
    memcpy(hf_peer_addr, param->disc_res.bda, ESP_BD_ADDR_LEN);
    printf("Connect.\n");
    esp_hf_ag_slc_connect(hf_peer_addr);  // Auto-connect
    esp_bt_gap_cancel_discovery();
}
```

**After:**
```c
if (strcmp(peer_bdname, remote_device_name) == 0) {
    memcpy(hf_peer_addr, param->disc_res.bda, ESP_BD_ADDR_LEN);
    printf("Device found. Use 'con' command to connect.\n");
    esp_bt_gap_cancel_discovery();
    // No auto-connect
}
```

### 7. New Scan Command

Added manual scan command for device discovery:

```c
HF_CMD_HANDLER(scan) {
    printf("Starting Bluetooth device discovery...\n");
    esp_err_t ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    if (ret != ESP_OK) {
        printf("Failed to start discovery: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Scanning for 10 seconds...\n");
    return 0;
}
```

## Updated Usage Workflow

### Recommended Connection Sequence:

1. **Start the Device**
   ```
   Flash firmware and open serial monitor
   ```

2. **Scan for Devices (Optional)**
   ```
   hfp_ag> scan
   Starting Bluetooth device discovery...
   Scanning for 10 seconds...
   Found target device name: YourHeadset
   Device found. Use 'con' command to connect.
   ```

3. **Connect to Device**
   ```
   hfp_ag> con
   Initiating connection...
   HFP connecting...
   HFP Service Level Connection established
   Audio connection initiated
   ```

4. **Check Connection Status**
   - Look for "HFP Service Level Connection established" message
   - Audio should start automatically after SLC is established

5. **Disconnect (if needed)**
   ```
   hfp_ag> dis
   Disconnecting...
   HFP disconnected
   ```

## Available Commands

| Command | Description |
|---------|-------------|
| `scan`  | Scan for Bluetooth devices (10 seconds) |
| `con`   | Connect to configured device |
| `dis`   | Disconnect from device |
| `cona`  | Manually connect audio channel |
| `disa`  | Disconnect audio channel |
| `vu`    | Volume control |
| `help`  | Show all available commands |

## Configuration

Before using, configure your target device name in `menuconfig`:

```bash
idf.py menuconfig
# Navigate to: Project configuration -> Bluetooth headset Name
# Set to your device's Bluetooth name (e.g., "Rockerz 258 Pro+")
```

## Troubleshooting

### Still Getting Collision Errors?

1. **Ensure only one side initiates connection:**
   - Either use `con` command on ESP32 OR
   - Initiate connection from phone/headset
   - Do NOT try to connect from both sides simultaneously

2. **Clear Bluetooth pairing:**
   ```bash
   # Erase NVS flash to clear pairing info
   idf.py erase-flash
   idf.py flash
   ```

3. **Check connection state before retrying:**
   - Wait for "HFP disconnected" message before reconnecting
   - Don't spam `con` command multiple times

### Connection Timeout

If connection attempt doesn't complete:
1. Verify device is in pairing/discoverable mode
2. Check device name matches config (case-sensitive)
3. Ensure device is within range
4. Try power cycling both devices

### Audio Not Working After Connection

If SLC connects but no audio:
1. Check I2S pin connections to PCM5100A
2. Verify 3.3V power to DAC
3. Check for I2S configuration errors in logs
4. Manually try: `cona` command

### RFCOMM Errors Persist

If you still see RFCOMM LCID reuse errors:
1. Power cycle ESP32 completely
2. Forget/unpair device on phone
3. Use `scan` then `con` workflow
4. Wait 5 seconds between disconnect and reconnect

## Technical Details

### Connection State Machine

```
DISCONNECTED → (con command) → CONNECTING → CONNECTED → SLC_CONNECTED
     ↑                                                         ↓
     └────────────────── (dis command) ← DISCONNECTING ←──────┘
```

### State Variables Lifecycle

- `connection_in_progress` = true when connection initiated
- `connection_in_progress` = false when SLC established or disconnected
- `is_connected` = true only when SLC is fully established
- Both flags reset to false on disconnect or error

### Collision Prevention Strategy

1. **Single Entry Point**: Only `con` command initiates connection
2. **State Guards**: Prevent duplicate connection attempts
3. **No Auto-Connect**: Discovery and connection are separate operations
4. **Delay Before Audio**: 500ms delay before audio connection to ensure stability

## Log Messages to Monitor

**Successful Connection:**
```
I HF_AG_MAIN: Ready for connection. Use 'con' command to connect.
Initiating connection...
I BT_APP_HF: HFP connecting...
I BT_APP_HF: HFP Service Level Connection established
I BT_APP_HF: --Audio State connected
I I2S: PCM5100A configured: 32-bit Stereo @ 44100 Hz
```

**Successful Disconnection:**
```
Disconnecting...
I BT_APP_HF: HFP disconnected
I AUDIO: Audio Stopped.
```

## Comparison: Before vs After

| Issue | Before | After |
|-------|--------|-------|
| Auto-discovery | Enabled (causes collision) | Disabled (manual scan) |
| Connection state | Not tracked | Fully tracked |
| Multiple attempts | Allowed (causes issues) | Prevented with guards |
| Disconnect handling | Incomplete | Full state reset |
| User feedback | Minimal | Clear status messages |
| Audio connection | Immediate | 500ms delay for stability |

## Additional Notes

- The device remains **discoverable and connectable** at all times
- Remote devices (phones/headsets) can still initiate connections to ESP32
- The `scan` command is optional - use if you don't know the device address
- Connection state is preserved across audio connect/disconnect cycles
- State is reset on complete disconnection or ESP32 reset

---

**Summary:** These changes implement proper state management and eliminate auto-connection behaviors that caused ACL collisions. The result is reliable, user-controlled Bluetooth connections without RFCOMM errors.
