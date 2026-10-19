# Notes

How the runtime currently works where it differs from hardware or is not obvious, and the debugging
aids. Open work is in [TODO.md](TODO.md).

## Audio

- The DSP is emulated with Dolphin's AX HLE (`runtime/dsp`). The polyphase resampling table lives in
  the DSP ROM, which is Nintendo's and not shipped, so voices are resampled with linear
  interpolation, as in Dolphin without `dsp_coef.bin`.
- Output goes through SDL (`runtime/audio_out.cpp`) at 32 kHz with an 80 ms prebuffer, and
  rebuffers after an underrun. Emulation is paced to the wall clock and nudged to keep 90-170 ms of
  audio queued, so it follows the audio device's clock.
- Idle skipping (jumping to the next timed event when no thread is runnable) stops at the next
  audio DMA block and pending DSP interrupts. Skipping past them made the audio DMA replay stale
  buffers before the game's AI interrupt handler could switch them.
- The music is streamed by CRI: the game decodes ADX on the CPU into two PCM16 ring buffers
  (45 ms each), played by two AX voices panned hard left and right.
- `__OSInitAudioSystem` / `__OSStopAudioSystem` are skipped by hooks.

## Graphics

- Rendering goes through aurora (`extern/aurora`, FIFO level) via `runtime/gpu/aurora_bridge.cpp`,
  which forwards the guest's GX stream and adds aurora's 0x50 extension commands (host pointers for
  arrays, textures, TLUTs and copies). Vertex array sizes are inferred from the indices each draw
  uses; texture contents are versioned by texture-cache invalidates (BP 0x66).
- Local fixes to aurora live in `patches/aurora`, applied at configure time:
  - 0001: texgens sourcing binormal/tangent on vertices without NBT read zero instead of aborting.
  - 0002: the XF viewport origin bias is 342 (as the hardware and this game's SDK use), not 340.
  - 0003: vertices within half a logical pixel of the viewport edge are snapped onto it. The game's
    full-screen quads stop 0.19 px short of each side (ortho -92.244..732.244, quads -92..732),
    which left the outermost physical columns unpainted when upscaled.
- Aurora's scissor-offset (BP 0x59) convention is the negative of the hardware's; the bridge
  converts it.
- Logic ops other than CLEAR/COPY/NOOP are approximated with blending (Dolphin's table).
- Display copies (EFB -> XFB) are not forwarded: aurora presents its EFB at the end of each host
  frame. The copy's clear is applied at the start of the next host frame.
- The game uses the EFB's top-left quadrant as scratch space for small passes and restores it by
  redrawing a full-frame copy, so any offset in viewport mapping shows up there first.

## Input

- WPAD/KPAD are emulated at the API level (`KPADRead`, `WPADProbe`, `WPADGetInfoAsync`): one Wii
  Remote + Nunchuk on channel 0, fed from SDL gamepads, the keyboard and scripted presses
  (`--press`).

## System

- The locked L1 cache (16 KB at 0xE0000000) and its DMA (DMA_U/DMA_L) are emulated; the game skins
  characters there. DMA completes instantly.
- `/dev/net/*` (WiiConnect24) devices are absent; the game reports errors and continues.

## Performance

- Build with clang (`tools/build_clang.bat`): gameplay runs at ~37 fps unthrottled (1.24x real time)
  vs ~21 fps with MSVC.
- Cross-chunk direct calls (`patches/dolrecomp/0001`, enabled by `tools/recompile.sh` with
  `DOLRECOMP_UNSAFE_DIRECT_CALLS=1`) halve dispatches. They bypass dispatch-time checks, which is
  fine while hooks are in-code markers.

## Debugging aids

- `--profile`: per-frame host timing (guest, bridge, draw-done wait, present) every 60 frames, a
  host sampling profile of the emulation thread and a guest-function profile at exit.
- `PANDORA_DEBUG_FRAME=<n>` with `-vv`: logs every draw (vertex descriptor, first vertex), texture,
  EFB copy, viewport/projection load and small draws' vertices of frame n.
- `--shot-draw <frame>:<draw>:<png>` captures the EFB before a draw; `--shot-copy` reads back an EFB
  texture copy; `PANDORA_SHOT_SCALE=<n>` reads screenshots back at n times the guest resolution.
- `PANDORA_DSP_TRACE=1`: logs audio DMA reloads, AX output buffers and the running voices once a
  second, and writes the first left-only PCM16 voice's raw reads to `voice_left_raw.bin`.
- `--audio-dump <wav>` writes the mixer output; `SDL_AUDIO_DRIVER=disk` (with
  `SDL_AUDIO_DISK_OUTPUT_FILE`) tests live output without a sound device.
- `--window <w>x<h>` sets the window size; `--unthrottled` disables pacing.
