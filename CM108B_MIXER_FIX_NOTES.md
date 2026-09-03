# CM108B mixer compile/runtime fix

## Root cause of the compile error

`bt_app_hf.c` and `notification_sound.c` were written for the central-mixer API:

- `audio_mixer_submit_hfp()`
- `audio_mixer_submit_jingle()`
- `audio_mixer_set_jingle_active()`
- `audio_mixer_wait_jingle_drained()`

but the added `audio_mixer.h/.c` only exposed `audio_mixer_push_hfp()`.
This was an inconsistent merge of two mixer implementations.

## Fix implemented

- Restored one coherent central mixer API.
- Preserved the existing HFP resampler in `bt_app_hf.c`, including both
  CVSD 8 kHz and mSBC/WBS 16 kHz input.
- CM108B input is I2S1 RX slave:
  - GPIO18 BCLK
  - GPIO19 LRCLK
  - GPIO23 DATA
- TAS5805M remains I2S0 TX master at 48 kHz / stereo / 32-bit.
- HFP, CM108B and AVA jingle are mixed by a single task.
- CM108B RX no longer paces the mixer. This is important because CM108B clocks
  can be absent when Android is not actively playing USB audio.
- Removed unused codec-negotiation variables that produced compiler warnings.
- Added CM108B and mixer gain options to `Kconfig.projbuild`.

## Recommended rebuild

Use ESP-IDF 5.3.1 and rebuild the configuration after replacing the project:

    idf.py fullclean
    idf.py reconfigure
    idf.py build

