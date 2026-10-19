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

## Settings menu

- RmlUi through aurora's integration (`AURORA_ENABLE_RMLUI`): `runtime/ui/settings_menu.cpp`
  (data model, input layer, navigation) with the layout in `assets/ui/menu.rml` and the style in
  `assets/ui/menu.rcss`. Assets are copied next to the executable on every build. The font is Lato
  (SIL OFL, `assets/fonts/LICENSE-Lato.txt`), taken from RmlUi's samples.
- The menu's actions are bindings (`Bindings::menu`), handled from aurora input events: every event
  becomes press/release edges of physical inputs (stick directions with hysteresis; the left mouse
  button stays RmlUi's), which hold menu actions; directions repeat after 500 ms every 50 ms. The
  input layer sees events even while the menu is closed (for the toggle) and captures every source
  while open. Game input is not polled while the menu is open; the game keeps running.
- RmlUi's spatial navigation does not cross scroll containers, so the bindings tables are navigated
  by hand (rows of mapping, clear, mapping, clear).
- Bindings are per device class (gamepad; keyboard and mouse), two mappings each. "Open/Close Menu"
  and "Accept" cannot lose their first mapping: its button resets it to the default instead.
- Held inputs are tracked as a set of physical inputs that are down. The router sends a layer
  `Cancelled` (and drops its pending releases) when capturing starts or stops with a press held,
  e.g. the key that opened the menu, so a cancelled source's inputs are forgotten.
- F5 pauses the game: the guest is not run and `aurora_set_preserve_frame` (patches/aurora/0004)
  keeps the last EFB on screen; the pacer is re-anchored on resume.
- `--menu-test <frame>:<png>` (with `--menu-keys a,b,Key*N`) opens the menu, plays keys through
  SDL's event queue (`*N` holds one for N host frames) and captures our own window with
  `PrintWindow` (the UI is composited after the EFB, so the EFB readback does not see it).

## Game patches

- Instruction patches for the recompiled code live in `config/SX3P01/patches.txt` (address,
  original word, new word; the original is checked). Guest memory keeps the original DOL.
- The Wii Remote strap screen is skipped there: its state machine (`fn_8034BA78`, state at
  scene+208) goes from state 0 straight to the button wait, which sees a press at once, so the strap
  fades out from zero alpha and is cleaned up as usual.
- The HOME Menu is disabled by never reporting the HOME button (`KPADRead` masks it; there is no
  Home binding).

## Input

- WPAD/KPAD are emulated at the API level (`KPADRead`, `WPADProbe`, `WPADGetInfoAsync`): one Wii
  Remote + Nunchuk on channel 0, fed from SDL gamepads, the keyboard and mouse, and scripted
  presses (`--press`).
- The pointer is tracked in screen units (-1..1 across the window): rate-controlled by its bound
  inputs, or put under the mouse when it moves over the window. KPAD values are those scaled by the
  game's range at the screen edges, measured in 16:9 as about +-1.22 horizontally and +-0.91
  vertically (a real Wii Remote reports values past +-1 at the screen's sides; at +-1 the cursor
  stopped short of the side edges). Measured from cursor positions at fixed pointer values. The OS cursor is hidden in the game while
  the mouse aims, and shown in the menu.
- Rumble: the game starts the motor through `fn_804F1F84` only if `WPADIsMotorEnabled` (the SYSCONF
  setting, read by the stubbed `WPADInit`), which is hooked to return true; `WPADControlMotor` is
  hooked and drives `SDL_RumbleGamepad`, refreshed every guest frame with a 200 ms duration so it
  stops by itself when the game pauses.

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
