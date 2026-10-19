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

4. Recompile the DOL and build the runtime (Visual Studio 2022, CMake 3.25+; aurora downloads
   prebuilt Dawn and SDL3 on first configure):

   ```sh
   tools/recompile.sh
   cmake -S . -B build/pandora -G "Visual Studio 17 2022" -A x64
   cmake --build build/pandora --config Release --target pandora
   ```

5. Run:

   ```sh
   build/pandora/Release/pandora.exe --disc "<extracted disc>"
   ```

   `--background` keeps the window from taking focus (for automated runs);
   `--screenshot <frame>:<file.png>` and `--exit-after <frames>` capture frames from inside the
   renderer; `--headless` runs without a window. See `--help`-style usage for debugging options.

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
