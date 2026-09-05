# HFP microphone gain control (VGM)

Implemented behavior:

- Default HFP microphone gain after every SLC connection: **10 / 15**.
- The default is configurable with `CONFIG_HFP_MIC_DEFAULT_GAIN` in `menuconfig`.
- Incoming headset `AT+VGM` changes update only the HFP microphone source in the central mixer.
- VGM 10 = existing nominal HFP level (1.0x).
- VGM 15 = 2.0x (about +6 dB).
- VGM 0 = mute.
- USB/CM108B, jingle, TAS5805M master volume and the low-latency HFP FIFO are unchanged.
- Headset `AT+VGS` (speaker gain) is logged but intentionally not mapped to the microphone.

Startup handling:

Many HFP headsets report a previously stored VGM immediately after the SLC is established.
AVA keeps the configured default authoritative during that startup window and re-sends the default
just before the SCO audio connection is requested. After that point, headset `AT+VGM` events are
applied immediately.
