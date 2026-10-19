#!/bin/sh
# Regenerate the recompiled C sources in build/recomp from orig/SX3EXJ/main.dol.
# Builds DolRecomp from extern/DolRecomp on first use.
set -e
cd "$(dirname "$0")/.."

DOLRECOMP=build/dolrecomp/Release/dolrecomp.exe
if [ ! -x "$DOLRECOMP" ]; then
    cmake -S extern/DolRecomp -B build/dolrecomp -G "Visual Studio 17 2022" -A x64
    cmake --build build/dolrecomp --config Release --target dolrecomp
fi

python -I tools/export_map.py config/SX3EXJ/symbols.txt build/SX3EXJ.map
python -I tools/patch_hooks.py orig/SX3EXJ/main.dol config/SX3EXJ/symbols.txt config/SX3EXJ/hooks.txt     build/SX3EXJ/main_hooked.dol
rm -rf build/recomp
"$DOLRECOMP" --cpu broadway --map build/SX3EXJ.map -j8 build/SX3EXJ/main_hooked.dol SX3EXJ build/recomp
