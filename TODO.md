# TODO

## Audio

- **DSP resampling coefficients.** Dolphin's AX HLE resamples voices with a polyphase filter whose
  coefficients come from the DSP ROM (`dsp_coef.bin`, 0x1000 bytes). That table is Nintendo's data,
  so it is not shipped and `AXUCode::LoadResamplingCoefficients` (`runtime/dsp/ax.cpp`) always
  fails; voices fall back to linear interpolation, as in Dolphin without the file. Add an option to
  load a user-supplied `dsp_coef.bin` (Dolphin validates it with an Adler-32 checksum) and compare
  the sound with and without it.
- Audio plays through SDL (runtime/audio_out.cpp) with an 80 ms prebuffer; emulation is paced to
  the wall clock and nudged to keep 90-170 ms queued, so it follows the device clock. The DAC rate
  is fixed at 32 kHz (AICR AIDFR is ignored).
- Left/right are nearly uncorrelated on the title music: check the SYSCONF sound mode (Dolby Pro
  Logic II encoding would explain it) and compare with Dolphin.
- `__OSInitAudioSystem` / `__OSStopAudioSystem` are skipped by hooks; the DSP init microcode
  handshake is not emulated.

## Graphics

- Texgens sourcing binormal/tangent (incl. emboss bump) on vertices without NBT read zero
  (patches/aurora/0001); hardware uses the last loaded value. Check which models hit this.
- The GX bridge (`runtime/gpu/aurora_bridge.cpp`) is new; only the boot screens have been checked.
- Logic ops other than CLEAR/COPY/NOOP are approximated with blending (Dolphin's table) without
  Dolphin's shader-side fixups for CLEAR/INVERT/COPY_INVERTED/SET.
- The BP write mask (BP 0xFE) is not applied to the registers the bridge shadows or rewrites
  (textures, TLUTs, cmode0, scissor offset).
- Texture contents are versioned only by texture-cache invalidates (BP 0x66); textures rewritten in
  place without an invalidate would show stale data. Consider hashing.
- Vertex array sizes are inferred from the indices each draw uses; arrays written in place between
  draws without a base change rely on aurora's per-frame re-upload.
- Display copies (EFB -> XFB) are not forwarded; aurora presents its EFB at the end of each host
  frame. Interlaced/field-based copies, XFB tricks and the copy's clear flag are not emulated.
- Aurora decodes the XF viewport with a 340 offset (as its own `GXSetViewport` encodes); confirm
  against this game's SDK, which may use 342 (2 px shift).
- Z (depth) EFB copies and EFB peeks/pokes are not bridged.
- No frame pacing yet: the guest runs as fast as presentation allows.

## Input

- WPAD/KPAD are emulated at the API level (`KPADRead`, `WPADProbe`, `WPADGetInfoAsync`): one
  Wii Remote + Nunchuk on channel 0. Still missing:
  - `WPADGetInfoAsync` never calls its callback (needs a guest call-back mechanism).
  - Motion controls: the accelerometer always reads "at rest"; shakes/swings need a mapping.
  - Pointer is rate-controlled by the right stick/IJKL; consider mouse-absolute mode.
  - `KPADReadEx`, connect/extension callbacks and rumble (`WPADControlMotor`) are not hooked.
- `WPADInit`, `WPADRegisterAllocator` and `KPADInit` in `config/SX3P01/names.txt` are provisional
  (identified from call structure); confirm them.

## System

- Locked cache (16 KB at 0xE0000000) and its DMA are emulated; DMA completes instantly and the
  queue length in HID2 is never non-zero.
- ES ioctlv 0x16 (GetConsumption) is unhandled.
- After reopening `/tmp/pac001.aar` the game issues a NAND write on fd 0 ("write on unknown fd 0");
  find out which open it expected to succeed.
- `/dev/net/*` (WiiConnect24) devices are absent; the game reports errors and continues.
- The console reports itself as NDEV 2.1 because of the `0x800030E6` flag copied from Dolphin;
  check whether a retail value changes game behaviour.
- VI timing assumes a 60 Hz field rate regardless of the selected video mode.

## Performance

- Build with clang (tools/build_clang.bat): gameplay ~37 fps unthrottled (1.24x real time) vs
  ~21 fps with MSVC. Remaining host profile: DolRecomp's FP/paired-single helpers (binary_single,
  psq_load_value, ...) are out-of-line calls in cpu.c (31k call sites); the memory fast path
  (pandora_mem_read32) is not always inlined; the bridge costs ~3.7 ms/frame (vertex index scan in
  prepare_draw, a FIFO publish/worker wake per draw); every instruction stores ctx->pc.
- Cross-chunk direct calls (patches/dolrecomp/0001, DOLRECOMP_UNSAFE_DIRECT_CALLS) halve
  dispatches; they bypass dispatch-time checks, which is fine while hooks are in-code markers.
- About 0.3x real time during boot, dominated by OS alarm/scheduler churn from the game polling
  with `OSSleepTicks(1)`. Re-measure once frames complete.
