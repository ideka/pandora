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
- `GX_CULL_ALL` (patches/aurora/0005): triangle draws are skipped while it is set (lines and points
  are never culled); aurora aborted on it ("unsupported cull mode 3").
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
- The Wii Remote strap screen is removed there: its state machine (`fn_8034BA78`, state at
  scene+208) fades in (0), waits for the fade (1), waits 60 frames (2), waits for a button or 20 s
  (3), fades out and frees its pages (4) and starts the title after 10 frames (5). State 0 branches
  straight to state 4's cleanup, so the strap is never drawn (no fade-in to flash).
- The HOME Menu is disabled by never reporting the HOME button (`KPADRead` masks it; there is no
  Home binding). `PANDORA_HOME_MENU=1` sends a real Wii Remote's HOME to the game instead of the
  settings menu: the HOME Menu's cursor is the quickest check of the pointer.

- 60 FPS mode: the community code `04774AB0 0000003C` (Dolphin forums' 60 FPS list, for PAL and
  NTSC-U) sets the game's frame-rate target, a word in its frame manager (`lbl_80774678` + 0x438;
  30 normally, next to 60/30 limits), to 60. It is re-applied every host frame like a cheat code;
  turning it off writes 30 back. `PANDORA_FPS60=0/1` overrides the setting for test runs.
- How the game keeps time: logic always ticks at 60 Hz. `gameFrame` (`fn_804C2844`) runs
  60/target ticks per rendered frame (2 at 30 FPS, 1 at 60), then renders once. Frame manager
  +0x448 is set on the first tick of a frame; `lbl_807F04F8` counts ticks, `lbl_807F04FC` frames.
  Particles (`fn_8048C7B4`) and the cloth (`dynMgrTask`, one 1/60 s step of two substeps per tick)
  are tick based. Code gated on +0x448 runs once per frame at 30 FPS but every tick at 60, and
  code gated on its being clear (the second tick) never runs at 60.
- One such gate: three characters' updates (the player's in `playerDynUpdate`) run an extra cloth
  step, `dynMgrExtraStep`, only on the second tick of a frame, so at 30 FPS that cloth gets 3 steps
  per 2 ticks. Those gates are patched out and a hook decides: unchanged with several ticks per
  frame, every other tick with one. It only fires in some character states (flags `&1` and `&0x40`
  of the character's state byte), not while standing.
- Aeron's cape was frozen at 60 FPS, stretched out in its bind pose while the cloth simulation
  moved underneath (the mesh is skinned to the cloth's bones). The cause is another "second tick
  only" gate: `sceneRefreshSkinned` (`fn_804A5780`), a scene pass that gives skinned nodes (type
  14) their slot and marks their subtrees' matrices dirty, so the skinned meshes never picked up
  the cloth's output. Same treatment as `dynMgrExtraStep`: gate patched out, the hook runs it
  every tick with one tick per frame, on the second tick otherwise (once per rendered frame
  either way). Found by opening each of the five second-tick gates in turn with
  `PANDORA_HOST_PATCH` (below) in tick-aligned runs (`fn_804F7F70`'s gate, the frame manager's
  own, hangs the game when opened; `fn_8000F9A4`, `fn_800384B0`, `fn_8007E364` change nothing
  visible). Tick-aligned: at 60 FPS every scripted press at twice the frame number of the 30 FPS
  run gives the same game state tick for tick, so screenshots compare at the same pose.
- The cloth's anchors (`dynClothSetAnchors`, `dynClothRefreshBones`) are written from world
  matrices that the engine recomputes once per rendered frame, so at 30 FPS they move only on a
  frame's first tick; at 60 FPS they move every tick and the cape streams out further behind a
  running Aeron. Hooks skip both writers on odd ticks in 60 FPS mode (`PANDORA_CAPE_OFF=1` turns
  that off), which brings the look close to 30 FPS; the cloth's step is 1/60 s per tick in both
  modes (`DynMgr+532`, a constant), so the simulation itself needs no change.

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

## Real Wii Remote

- `runtime/wiimote.cpp` drives a Wii Remote (057E:0306, and the -TR 057E:0330) through SDL's HID
  API, with SDL's own Wii joystick driver disabled. It scans when the system's HID device list
  changes, then: player LED 1, accelerometer calibration (EEPROM 0x16), IR camera in basic mode
  (Wii sensitivity level 3), the extension unencrypted (0x55 to A400F0, 0x00 to A400FB; a Nunchuk's
  id ends A4 20 00 00, retried while it reports busy) and continuous report 0x37 (buttons,
  accelerometer, 10 IR bytes, 6 extension bytes). A status report (extension plugged or unplugged)
  re-initialises the extension and the reporting mode.
- The pointer (`Wiimote::parse_ir`) reproduces the game's own KPAD library (its DPD code is in
  the DOL: `fn_805E7CA0` selects the dots, `fn_805E76C0` makes pos/horizon/dist; constants from
  `KPADInitEx`, `fn_805E61D0` and the sampling callback `fn_805EA6B0`): dots normalised as
  (x - 511.5) / 512 and (y - 383.5) / 512 with WPAD's y pointing down (767 - camera y: measured,
  the camera's y falls as the remote turns up), the outer 5% of the image ignored, the two ends
  ordered left to right (by x, reversed when the accelerometer says upside down), the midpoint
  rotated into the bar's frame, then pos = 1.25 * (origin - midpoint) with the origin 0.2 towards
  the bar (the Wii's Sensor Bar Position setting; `WPADGetSensorBarPosition` in the callback),
  horizon = the bar's direction, dist = (0.2 m / tan 21 deg) / separation; the scale is
  sqrt(1 + 0.75^2) / (0.75 - 0.2) = 2.27 (the sampling callback: the image's half height less
  the origin, the smaller of the two extents, reaches the corner; two earlier readings of that
  code, 1.25 and 1.32, left the far edge of the screen out of the camera's view); play radius 0
  and sensitivity 1 (KPAD's defaults, the game sets none). KPAD's minimum distance of 0.52 m is
  relaxed to 0.3 m (a desk monitor is closer than a Wii expects; a user's log showed 0.58 m). KPAD's tracker is ported too
  (`fn_805E7150` keeps the pair whose distance moved < 4 cm and direction < 25 degrees since the
  last sample, `fn_805E6F60` acquires the pair best aligned with the horizon within 0.52..3 m,
  `fn_805E7540` follows one dot within 0.1 of a previous end, `fn_805E7380` acquires from one dot
  with the bar's remembered length, learnt after 200 whole samples), with the accelerometer's
  up/down as the horizon reference. `PANDORA_IR_LOG=1` logs every IR report (raw dots, ends
  found, pos, dist, acc). The game's cursor reaches the screen's edges at about +-1.22 / +-0.91
  (measured). The Wii's IR sensitivity setting (1-5) selects the camera's gain block (WiiBrew's).
- Continuous scanning (`runtime/wiimote_scanner.cpp`, Windows), as in Dolphin: while enabled and
  no remote is connected, a thread runs Bluetooth inquiries (2 x 1.28 s). A "Nintendo
  RVL-CNT-01(-TR)" in discoverable mode (1+2, sync) gets its HID service enabled with
  `BluetoothSetServiceState`, so Windows creates the HID device without pairing; the driver then
  rescans HID devices every second for 10 s. Unauthenticated remotes Windows remembers but that are
  not connected are removed first (they block reconnecting); PIN-paired ones are left alone.
  Inquiries can disturb other Bluetooth devices (audio), hence the setting.
- Some Windows Bluetooth stacks reject WriteFile on the remote (notably the -TR); output reports
  then go through HidD_SetOutputReport, as in Dolphin.
- Core buttons have the WPAD bit layout. Accelerometer axes (remote: x left, y towards the camera,
  z up) become KPAD's (x right, y down, z towards the camera): (-x, -z, y); the same for the Nunchuk.
- Pointer: the midpoint of the first two IR dots, rotated by their angle (roll), mirrored, scaled so
  a ~1.2 m 16:9 screen at 2.5 m is covered, with pointing at the sensor bar at the screen edge next
  to it. No dots: the pointer is invalid (the game hides its cursor) unless the mouse or bound
  inputs moved it in the last two seconds.
- Rumble is on/off on the remote's motor, switched off explicitly when the game stops updating.
- Background (test) runs do not touch remotes unless PANDORA_POLL_WIIMOTE is set.

## System

- The locked L1 cache (16 KB at 0xE0000000) and its DMA (DMA_U/DMA_L) are emulated; the game skins
  characters there. DMA completes instantly.
- `/dev/net/*` (WiiConnect24) devices are absent; the game reports errors and continues.

## Performance

- Build with clang (`tools/build_clang.bat`): gameplay runs at ~37 fps unthrottled (1.24x real time)
  vs ~21 fps with MSVC.
- Gameplay cost is almost all per rendered frame: two logic ticks (30 FPS) cost little more than
  one (60 FPS), so 60 FPS needs nearly twice the work per second. The observatory at 60 FPS costs
  ~9.2 ms of guest time per frame (5.3M guest cycles, ~5.4 host cycles each; DolRecomp charges
  ~1.1 cycles per instruction) against a 16.7 ms budget. The display caps measured fps at 60 even
  with vsync off (the surface only offers FIFO presentation here), so guest ms/frame is the
  figure to compare, as `scratchpad/perf.sh`-style runs with `--profile` print it.
- How the time was found: `PANDORA_PROFILE_DUMP=<file>` with `--profile` writes every sampled host
  address; `tools/hotspots.py <dump> <exe>` attributes chunk samples to guest mnemonics and shows
  the hottest host instructions with their guest instruction. Loads/stores were 40% of all time
  before the memory work; the FP flag/NaN bookkeeping ~2 ms/frame after it.
- Guest memory is at a fixed host address (`fastmem/cpu/guest_base.h`, `runtime/guest_memory.cpp`):
  MEM1, MEM2 and the locked cache inside a reserved 4 GB window, so a RAM access is one range
  check plus a `movbe`. Accesses to nowhere in the window fault and are reported with the guest
  address (on the console they would be a DSI exception).
- Floating point is at the accuracy of Dolphin's JIT (fastfp.h): raw SSE NaNs, FPSCR[NI] mirrored
  to the host's MXCSR flush-to-zero/denormals-are-zero (`pandora_fpscr_updated`, hooked on FPSCR
  writes and FPU context loads), fused single multiply-adds without the double-rounding tie fix.
  Everything else is bit-exact with DolRecomp's cpu.c; `pandora --fp-selftest` checks 2M random
  operations, every lfs pattern and 20M stfs values and counts the allowed deviations.
- Chunk rewrites (tools/strip_pc.py, applied by recompile.sh): pc stores dropped, `__restrict`,
  the cycle downcount in a local, System V calling convention, bctrl/blrl calling the target chunk
  directly, cache/barrier instructions inline no-ops (each was a dispatcher round trip: half of all
  dispatches were the OS's cache-flush loops), loop budget 4096 cycles and call depth 96
  (CMakeLists). Dispatches went from 47k to 17k per frame; that alone changed little.
- `PANDORA_DISPATCH_HIST=1` prints where the chunks returned to the dispatcher most.
- Measuring: a window covered by other windows has its presents throttled by Windows, which
  backs up into aurora's two frame slots and stalls the emulation thread (~15% in gameplay);
  `--background` therefore puts the window off-screen. `PANDORA_PROFILE_FROM=<frame>` limits
  `--profile` to gameplay, `PANDORA_PROFILE_THREADS=1` adds the other threads, and samples outside
  the executable are attributed to their nearest caller inside it (waits show up that way).
- `runtime/fastmem/cpu/fastfp.h` replaces DolRecomp's out-of-line FP, paired-single and quantized
  load/store helpers in the chunks with inline versions that keep the results (single rounding,
  the 25-bit multiplier truncation, quantization) but no FPSCR exception bookkeeping (~15%).
- Memory accessors inline only the MEM1 path; forcing the whole range check inline at every site
  made the chunks slower (code size).
- aurora wakes its FIFO worker every 16 draws instead of every draw (`patches/aurora/0006`), and
  the bridge goes through `fifo::finish_draw` (it used to publish after every draw itself).
- The generated code's per-instruction `ctx->pc` stores are removed by `tools/strip_pc.py` (run by
  `recompile.sh`; `PANDORA_KEEP_PC=1` keeps them). Branches still set the pc, so the guest profile
  and crash logs only see the last branch target.
- Entry pruning (`tools/prune_entries.py`, run by `recompile.sh`; `PANDORA_KEEP_ENTRIES=1` keeps
  every instruction enterable): each chunk's entry switch keeps only the addresses that can be
  entered (function starts, code pointers in data, pc constants, return addresses, hook
  continuations; ~11% of instructions). FP-unavailable exceptions are handled in place
  (`runtime/fpu_switch.cpp` does the OS's lazy FPU switch) so FP instructions need no entry. An
  unlisted entry halts with "no chunk entry". Builds take ~3.5 minutes instead of ~13. Measured
  speed is unchanged: the generated code still keeps every guest register in ctx memory, ~15 host
  cycles per guest instruction on average; the next step is the code generator (registers in
  locals within a function).
- The guest is compiled with AVX2 + FMA (fused multiply-adds are one instruction). Math in
  `fastfp.h` uses compiler builtins: MSVC's C headers make `isnan` etc. calls into the CRT, which
  cost ~4 ms of guest time per frame.
- `PANDORA_PROFILE_LINES=<n>`: the hottest source lines in the `--profile` report, with the guest
  instruction for lines of the recompiled chunks.
- Write-gather pipe stores (`0xCC008000`) go straight from the chunks to `Hardware::gather`
  (`pandora_wgpipe_write`); 32-bit reads inline MEM2 too; lfs/stfs bit conversions inline their
  common case.
- `tools/recompile.sh` only rewrites generated files whose contents changed, so a patch or hook
  rebuilds a few chunks (seconds) instead of all of them (~13 minutes).
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
- `--press StickUp@<frame>+<frames>` (StickDown/Left/Right) holds the Nunchuk stick.
- `PANDORA_PROFILE_TOP=<n>` lists n guest functions in the `--profile` report (to diff runs). The
  guest profile samples `cpu.pc`, which is only updated at branches (see strip_pc), and host time
  (bridge, HLE) is charged to whatever guest code ran last; rebuild with `PANDORA_KEEP_PC=1` for a
  precise one.
- `PANDORA_NO_VSYNC=1` turns vsync off (to measure headroom).
- `PANDORA_HOST_PATCH=<rva>=<hex bytes>[;...]` patches this executable's code at startup (a
  recompiled branch opened or closed without a rebuild, e.g. a `bne` gate: `je` -> 6-byte nop);
  `tools/lines2rva.py <pdb> <chunk .c> <line>` gives the host RVAs of a chunk's source lines (the
  line after a `// <addr>: bc ...` comment holds the branch) via llvm-pdbutil, and llvm-objdump
  `--start-address=0x140000000+rva` shows the bytes. `PANDORA_WAIT_DEBUGGER=1` makes a test run
  wait for a debugger before starting (x64dbg attaches by pid, which the run prints).
- `tools/dol2elf.py <main.dol> <symbols.txt> <out.elf>` makes an ELF with every section and symbol,
  for Binary Ninja/Ghidra (`build/SX3P01/main.elf`).
