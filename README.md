# Pandora's Tower static recompilation

Work-in-progress static recompilation of *Pandora's Tower* (Wii, NTSC-U `SX3EXJ`) to a native PC
executable. Licensed under GPLv3.

No game code or data is included. You need your own copy of the game.

## Setup

1. Extract the game's disc partition so that `sys/main.dol` and `files/` exist, then copy the DOL:

   ```sh
   mkdir -p orig/SX3EXJ
   cp "<extracted disc>/sys/main.dol" orig/SX3EXJ/main.dol
   ```

   Expected SHA-1: `bdbd1e9153220a3a0b092f8319127f59093d3c16`.

2. Fetch [decomp-toolkit](https://github.com/encounter/decomp-toolkit) v1.8.4 into `build/tools/dtk.exe`.

3. Run the analysis:

   ```sh
   build/tools/dtk.exe dol split config/SX3EXJ/config.yml build/SX3EXJ
   ```

`orig/` and `build/` are git-ignored.

## Layout

- `config/SX3EXJ/` — decomp-toolkit project config: function boundaries (`symbols.txt`) and splits.
- `tools/` — analysis scripts (Python 3, standard library only; run with `python -I`).
  - `find_jumptables.py` — finds unbounded `bctr` dispatch tables that dtk would mis-size and
    emits sized symbol hints. dtk rewrites `symbols.txt` on every split, so merge its output in
    rather than overwriting the file.
  - `match_dsy.py` — matches functions against a Dolphin `.dsy` signature database.
  - `xref_analysis.py` — source-file attribution, GX FIFO writers, stack switchers.
