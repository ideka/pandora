# Pandora's Tower static recompilation

> [!NOTE]
> This project is vibe coded: nearly all of its code was written by an AI coding assistant (Claude),
> directed and play-tested by a human. Expect rough edges.

Work-in-progress static recompilation of *Pandora's Tower* (Wii, PAL `SX3P01`) to a native PC
executable. Licensed under GPLv3.

No game code or data is included. You need your own copy of the game.

## Setup

1. Extract the game's disc partition so that `sys/main.dol` and `files/` exist, then copy the DOL:

   ```sh
   mkdir -p orig/SX3P01
   cp "<extracted disc>/sys/main.dol" orig/SX3P01/main.dol
   ```

   Expected SHA-1: `bdbd1e9153220a3a0b092f8319127f59093d3c16`.

2. Fetch [decomp-toolkit](https://github.com/encounter/decomp-toolkit) v1.8.4 into `build/tools/dtk.exe`.

3. Run the analysis:

   ```sh
   build/tools/dtk.exe dol split config/SX3P01/config.yml build/SX3P01
   ```

4. Recompile the DOL and build the runtime (Visual Studio 2022 with its C++ Clang tools, CMake
   3.25+; aurora downloads prebuilt Dawn and SDL3 on first configure):

   ```sh
   tools/recompile.sh
   cmd //c tools\build_clang.bat        # clang-cl + Ninja, into build/clang
   ```

   clang compiles the recompiled code into something about 1.8x faster than MSVC does (gameplay
   runs at ~1.25x real time instead of ~0.7x), so it is the build to play. The MSVC build still
   works for debugging in Visual Studio:
   `cmake -S . -B build/pandora -G "Visual Studio 17 2022" -A x64` then
   `cmake --build build/pandora --config Release --target pandora`.

5. Run:

   ```sh
   build/clang/pandora.exe --disc "<extracted disc>"
   ```

   The game is paced to real time (30 fps) following the audio device's clock; `--unthrottled`
   runs as fast as possible.

   `--background` keeps the window from taking focus (for automated runs);
   `--screenshot <frame>:<file.png>` and `--exit-after <frames>` capture frames from inside the
   renderer; `--headless` runs without a window. `--press A@300+10` holds a button from a given
   frame (scripted input). See `--help`-style usage for debugging options.

   **Esc** (keyboard) or **Guide** (gamepad) opens the settings menu; the game keeps running
   behind it (without input), and **F5** pauses and resumes the game. The menu has four tabs:

   - **Controls**: every control of the emulated controller and every menu action (open/close,
     accept, cancel, previous/next tab) has two mappings per device (Gamepad, Keyboard & Mouse);
     select one and press the new input, or clear it with x (Open/Close Menu and Accept reset
     their first mapping to the default instead). The right stick scrolls the list.
   - **Sound**: volume and mute.
   - **Graphics**: 60 FPS mode, fullscreen, anisotropic filtering (off to 16x), 4x MSAA
     anti-aliasing (applies after a restart) and removing the ghosting (the game's depth of field,
     which at high resolutions shows as shifted doubles of the background). Changing texture filtering or the ghosting
     hides the rest of the menu until you move to another row, so the game shows behind it.
   - **General**: rumble, Classic Controller, free stick direction, keeping quick saves after
     restarting from them (the game deletes them), the mouse pointer, and the real
     Wii Remote's settings and pointer calibration (below).

   In the menu: D-pad/left stick or arrows/WASD move (fixed, not bindable; they repeat while held),
   East or Enter/Space accept, South or Backspace cancel, LB/RB or PageUp/PageDown (Q/E) switch
   tabs. Settings are saved to `build/aurora/settings.ini`.

   Default controls (Wii Remote + Nunchuk): gamepad east/south/north/west = A/B/1/2 (by position,
   as on Nintendo's pads), RT = B too, RB = shake, start/back = +/-, LB = C, LT = Z, left stick =
   Nunchuk, right stick = pointer. Keyboard and mouse: the mouse aims the pointer, left click = B,
   Enter/Space = A, Backspace = B, arrows = D-pad, WASD = stick, IJKL = pointer, R = shake, Z/C,
   1/2, =/-. Rumble plays on the gamepad (intensity under General). The Wii Remote strap screen is
   skipped and the HOME Menu is disabled.

   "Classic Controller" (General) switches the emulated controller from Wii Remote + Nunchuk to a
   Classic Controller, which the game supports; its bindings then show under Controls. Defaults:
   buttons by position as on the real one (A/B/X/Y on the pad's east/south/north/west), the analog
   L/R on the triggers, the digital ZL/ZR on the bumpers, both sticks; keyboard WASD/IJKL for the
   sticks, Q/E for L/R, Z/C for ZL/ZR, left click ZR (fire), right click R (aim). The cursor
   follows the right stick, or the mouse while it moves.

   A real **Wii Remote** (with Nunchuk) works too: with "Scan for Wii Remotes" on (General, the
   default) just press 1 and 2 on the remote while the game runs, as with Dolphin's continuous
   scanning; pairing it in Windows' Bluetooth settings or a DolphinBar in mode 4 also work. Its
   buttons, Nunchuk, motion, pointer and rumble go to the game directly, and HOME opens the
   settings menu (D-pad, A, B, -/+ work there). The pointer needs a sensor bar and behaves as on a
   Wii (the pointer maths is the game's own KPAD library's); the Wii's two settings are under
   General: "Sensor bar above screen" (Sensor Bar Position) and "IR sensitivity" (the camera's
   gain, 1-5). "Calibrate..." (General) shows what the remote's camera sees, with those settings and
   "Merge nearby dots": if a sensor bar end shows as several dots (and the cursor jumps), raise it
   until each end shows as one. The game's Wii Remote speaker sounds play through the PC's speakers
   (the remote's own speaker would need Bluetooth passthrough, as on Dolphin).

   F11 or Alt+Enter toggles fullscreen. Audio plays on the default output device; `--mute`
   disables it. `--background` runs are always muted and never write the settings file.

Each run logs to `build/aurora/pandora.log` (`pandora-background.log` for `--background` runs). On
a crash or fatal error the log ends with where the game was (guest function and backtrace) and a
minidump is written to `build/aurora/pandora.dmp` (open it in Visual Studio with the PDB next to
the executable). Please include both when reporting a crash.

`orig/` and `build/` are git-ignored. Saves live in the emulated NAND, in
`build/nand/title/00010000/53583350/data` (the three slots, their temporary saves and the game's
system file; standard Wii save files, as Dolphin's NAND keeps them). Aurora's caches (compiled
shaders and pipelines) live in `build/aurora`.

Shaders the GPU hasn't compiled yet are drawn with a generic, slower "ubershader" meanwhile, so a
first visit to an area appears at once; its pipelines are built at startup (a few seconds on the
first run, a fraction of a second after). `--temp-cache` runs with empty shader caches in a
temporary folder, deleted on exit, to see a first run.

## Layout

- `config/SX3P01/` — decomp-toolkit project config: function boundaries (`symbols.txt`) and splits.
- `patches/aurora/`, `patches/dolrecomp/` — local changes to the submodules, applied at configure
  time (the applied ones are recorded in `extern/aurora/.pandora-patches`).
- `data/initial_pipeline_cache.db` — the ubershader pipelines built at startup (copied next to the
  executable; aurora merges it into its pipeline cache), made by `tools/make_uber_seed.py`.
- `tools/` — scripts (Python 3; the analysis ones use the standard library only, run with
  `python -I`).
  - `make_uber_seed.py` — builds `data/initial_pipeline_cache.db` from aurora pipeline caches
    (needs the `xxhash` package); rerun it with a cache from new areas to cover them.
  - `find_jumptables.py` — finds unbounded `bctr` dispatch tables that dtk would mis-size and
    emits sized symbol hints. dtk rewrites `symbols.txt` on every split, so merge its output in
    rather than overwriting the file.
  - `match_dsy.py` — matches functions against a Dolphin `.dsy` signature database.
  - `xref_analysis.py` — source-file attribution, GX FIFO writers, stack switchers.

## Credits

- [Dolphin](https://github.com/dolphin-emu/dolphin) — the DSP audio code in `runtime/dsp/` is
  adapted from its DSP HLE (GPL-2.0-or-later), and its sources were the reference for much of the
  rest of the hardware emulation.
- [aurora](https://github.com/encounter/aurora) — the GX-to-WebGPU renderer, windowing and input.
- [DolRecomp](https://github.com/ExpansionPak/DolRecomp) — the PowerPC-to-C static recompiler.
- [decomp-toolkit](https://github.com/encounter/decomp-toolkit) — DOL analysis and splitting.
- [Dawn](https://dawn.googlesource.com/dawn), [SDL](https://www.libsdl.org/),
  [RmlUi](https://github.com/mikke89/RmlUi) and [Dear ImGui](https://github.com/ocornut/imgui),
  used through aurora.
- [Lato](https://www.latofonts.com/) by Łukasz Dziedzic — the menu font (SIL Open Font License,
  `assets/fonts/LICENSE-Lato.txt`).
- *Pandora's Tower* is by Ganbarion and Nintendo. This project is not affiliated with either.
