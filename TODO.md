# TODO

Open work only. How things currently work, and why, is in [NOTES.md](NOTES.md).

## Audio

- **DSP resampling coefficients.** Add an option to load a user-supplied `dsp_coef.bin` (the DSP
  ROM's polyphase table, 0x1000 bytes; Dolphin validates it with an Adler-32 checksum) in
  `AXUCode::LoadResamplingCoefficients` (`runtime/dsp/ax.cpp`), then compare the sound with and
  without it. Without it voices use linear interpolation.
- Honour the DAC rate selected in AICR (AIDFR); output is fixed at 32 kHz.
- Emulate the DSP init microcode handshake instead of skipping `__OSInitAudioSystem` /
  `__OSStopAudioSystem` with hooks.

## Graphics

- Seams between adjacent quads when upscaled (sub-pixel coverage differs from the hardware's
  1/12-pixel grid); only the viewport-edge case is handled.
- Texgens sourcing binormal/tangent on vertices without NBT read zero; find which models hit this
  and use the last loaded value, as hardware does.
- Check the GX bridge beyond the screens seen so far (title, menus, the first areas).
- Logic ops other than CLEAR/COPY/NOOP: add Dolphin's shader-side fixups for
  CLEAR/INVERT/COPY_INVERTED/SET on top of the blending approximation.
- Apply the BP write mask (BP 0xFE) to the registers the bridge shadows or rewrites (textures,
  TLUTs, cmode0, scissor offset).
- Textures rewritten in place without a texture-cache invalidate (BP 0x66) show stale data;
  consider hashing contents.
- Vertex arrays written in place between draws without a base change rely on aurora's per-frame
  re-upload; verify or track writes.
- Interlaced/field-based display copies and XFB tricks.
- Z (depth) EFB copies and EFB peeks/pokes.

## Input

- `WPADGetInfoAsync` never calls its callback (needs a way to call back into guest code).
- Motion controls: the accelerometer always reads "at rest"; map shakes/swings.
- Hook `KPADReadEx` and the connect/extension callbacks.
- Rumble is untested on hardware (no in-game rumble event was scripted); check intensity feel and
  that the Wii's on/off pattern is followed.
- Mouse pointer mapping assumes the game image fills the window; correct for letterboxing if aurora
  ever adds it.
- Confirm the provisional names `WPADInit`, `WPADRegisterAllocator` and `KPADInit` in
  `config/SX3P01/names.txt` (identified from call structure).

## Settings menu

- Graphics tab (internal resolution/scale, aspect ratio, vsync, MSAA) once aurora exposes them.
- Per-device controller profiles (by GUID) and more than one gamepad; currently the first one.
- Show controller glyphs for the connected pad type instead of position names.
- Keep the settings file in the user's profile rather than `build/aurora` for distributed builds.

## System

- Handle ES ioctlv 0x16 (GetConsumption).
- After reopening `/tmp/pac001.aar` the game issues a NAND write on fd 0 ("write on unknown fd 0");
  find out which open it expected to succeed.
- Check whether reporting a retail console (instead of NDEV 2.1 from the `0x800030E6` flag copied
  from Dolphin) changes game behaviour.
- VI timing assumes a 60 Hz field rate regardless of the selected video mode; handle 50 Hz.
- Emulate the locked-cache DMA queue length in HID2 if anything waits on it (DMA currently
  completes instantly).

## Performance

- Inline DolRecomp's FP/paired-single helpers (`binary_single`, `psq_load_value`, ...): they are
  out-of-line calls into cpu.c from ~31k call sites.
- Make sure the memory fast path (`pandora_mem_read32` etc.) is inlined everywhere.
- Bridge (~3.7 ms/frame): the vertex index scan in `prepare_draw`, and a FIFO publish (worker
  wake-up) per draw.
- Avoid storing `ctx->pc` at every instruction in the generated code.
- Re-measure boot speed (was ~0.3x real time, dominated by OS alarm/scheduler churn from
  `OSSleepTicks(1)` polling, before the pacing and clang changes).
