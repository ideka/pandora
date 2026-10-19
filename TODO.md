# TODO

## Audio

- **DSP resampling coefficients.** Dolphin's AX HLE resamples voices with a polyphase filter whose
  coefficients come from the DSP ROM (`dsp_coef.bin`, 0x1000 bytes). That table is Nintendo's data,
  so it is not shipped and `AXUCode::LoadResamplingCoefficients` (`runtime/dsp/ax.cpp`) always
  fails; voices fall back to linear interpolation, as in Dolphin without the file. Add an option to
  load a user-supplied `dsp_coef.bin` (Dolphin validates it with an Adler-32 checksum) and compare
  the sound with and without it.
- Verify AX mixing with real voices once the game plays sound (use `--audio-dump`).
- Real audio output (SDL) instead of the WAV dump; the DAC rate is fixed at 32 kHz for now.
- `__OSInitAudioSystem` / `__OSStopAudioSystem` are skipped by hooks; the DSP init microcode
  handshake is not emulated.

## Input

- `WPADInit` is stubbed (no Bluetooth). Implement WPAD/KPAD at the API level on top of SDL.
- `WPADInit`, `WPADRegisterAllocator` and `KPADInit` in `config/SX3P01/names.txt` are provisional
  (identified from call structure); confirm them.

## System

- ES ioctlv 0x16 (GetConsumption) is unhandled.
- After reopening `/tmp/pac001.aar` the game issues a NAND write on fd 0 ("write on unknown fd 0");
  find out which open it expected to succeed.
- `/dev/net/*` (WiiConnect24) devices are absent; the game reports errors and continues.
- The console reports itself as NDEV 2.1 because of the `0x800030E6` flag copied from Dolphin;
  check whether a retail value changes game behaviour.
- VI timing assumes a 60 Hz field rate regardless of the selected video mode.

## Performance

- About 0.3x real time during boot, dominated by OS alarm/scheduler churn from the game polling
  with `OSSleepTicks(1)`. Re-measure once frames complete.
