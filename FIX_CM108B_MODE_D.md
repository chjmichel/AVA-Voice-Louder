# CM108B I2S RX Mode D fix

This revision replaces the raw 32-bit diagnostic receive path with the intended
CM108B receive format for the original ESP32:

- Philips I2S
- 48 kHz stereo
- 16 valid PCM bits per channel
- 32 BCLK slot per channel (64 BCLK per stereo frame)
- normal BCLK polarity
- `msb_right = false`

The CM108B is the I2S master. ESP32 I2S1 remains an RX slave on:

- GPIO18 = DASCLK / BCLK
- GPIO19 = DALRCK / LRCLK
- GPIO23 = SDOUT / DATA

The RX DMA buffer is now `int16_t`. The ESP-IDF standard I2S driver is told
separately that the valid sample width is 16 bits and the wire slot width is
32 bits. Samples are then promoted to the common signed 32-bit mixer domain.

The old block-count based "measured input rate" line was removed because it
measured successful software block delivery, not LRCLK itself. It could report
~41 kHz simply because blocks were being lost.

When `CONFIG_CM108B_DIAG_RX_STATS=y`, the firmware now reports every 10 seconds:

- complete 5 ms RX blocks
- partial `i2s_channel_read()` calls
- RX timeouts
- I2S errors
- incomplete 5 ms blocks
- USB FIFO drops
- total mixer FIFO underflows
- current FIFO level

A healthy continuously playing CM108B stream should be close to 2000 full
5 ms blocks per 10 seconds, with zero or near-zero timeouts, incomplete blocks,
FIFO drops and mixer underflows.

The internal 1 kHz output-path test remains available. It should normally be
disabled for this test because the TAS5805M output path has already been proven
clean.
