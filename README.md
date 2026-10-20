# Pandora's Tower static recompilation

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

   **Esc** (keyboard) or **Guide** (gamepad) opens the settings menu. The game keeps running behind
   it (without input); **F5** pauses and resumes the game. Under Controls, the Gamepad and
   Keyboard & Mouse sections each give every Wii Remote/Nunchuk control and every menu action
   (open/close, accept, cancel, directions, previous/next tab) two mappings; select one and press
   the new input, or clear it with x (Open/Close Menu and Accept reset their first mapping to the
   default instead). Holding a direction in the menu repeats it. Volume and
   fullscreen are under Sound and General. Settings are saved to `build/aurora/settings.ini`.

   Default controls (Wii Remote + Nunchuk): gamepad south/east/west/north = A/B/1/2,
   start/back = +/-, LB = C, LT = Z, left stick = Nunchuk, right stick = pointer. Keyboard and
   mouse: the mouse aims the pointer, left click = B, Enter/Space = A, Backspace = B, arrows = D-pad,
   WASD = stick, IJKL = pointer, Z/C, 1/2, =/-. Rumble plays on the gamepad (intensity under
   General). The Wii Remote strap screen is skipped and the HOME Menu is disabled. In the menu: D-pad/left stick or arrows/WASD move, South or Enter/Space accept, East or
   Backspace cancel, LB/RB or PageUp/PageDown (Q/E) switch tabs.

   A real **Wii Remote** (with Nunchuk) works too: with "Scan for Wii Remotes" on (General, the
   default) just press 1 and 2 on the remote while the game runs, as with Dolphin's continuous
   scanning; pairing it in Windows' Bluetooth settings or a DolphinBar in mode 4 also work. Its
   buttons, Nunchuk, motion, pointer (needs a sensor bar; set whether it sits above or below the
   screen under General) and rumble go to the game directly. HOME opens the settings menu (D-pad, A, B, -/+ work there).

   F11 or Alt+Enter toggles fullscreen. Audio plays on the default output device; `--mute`
   disables it. `--background` runs are always muted and never write the settings file.

Each run logs to `build/aurora/pandora.log` (`pandora-background.log` for `--background` runs). On
a crash or fatal error the log ends with where the game was (guest function and backtrace) and a
minidump is written to `build/aurora/pandora.dmp` (open it in Visual Studio with the PDB next to
the executable). Please include both when reporting a crash.

`orig/` and `build/` are git-ignored. Saves and aurora's caches live in `build/nand` and
`build/aurora`.

## Layout

- `config/SX3P01/` — decomp-toolkit project config: function boundaries (`symbols.txt`) and splits.
- `tools/` — analysis scripts (Python 3, standard library only; run with `python -I`).
  - `find_jumptables.py` — finds unbounded `bctr` dispatch tables that dtk would mis-size and
    emits sized symbol hints. dtk rewrites `symbols.txt` on every split, so merge its output in
    rather than overwriting the file.
  - `match_dsy.py` — matches functions against a Dolphin `.dsy` signature database.
  - `xref_analysis.py` — source-file attribution, GX FIFO writers, stack switchers.
