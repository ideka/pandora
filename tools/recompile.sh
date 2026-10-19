#!/bin/sh
# Regenerate the recompiled C sources in build/recomp from orig/SX3P01/main.dol.
# Builds DolRecomp from extern/DolRecomp on first use.
set -e
cd "$(dirname "$0")/.."

DOLRECOMP=build/dolrecomp/Release/dolrecomp.exe
if [ ! -x "$DOLRECOMP" ]; then
    cmake -S extern/DolRecomp -B build/dolrecomp -G "Visual Studio 17 2022" -A x64
    cmake --build build/dolrecomp --config Release --target dolrecomp
fi

mkdir -p build/SX3P01
python -I tools/export_map.py config/SX3P01/symbols.txt build/SX3P01.map
python -I tools/patch_hooks.py orig/SX3P01/main.dol config/SX3P01/symbols.txt config/SX3P01/hooks.txt     build/SX3P01/main_hooked.dol
rm -rf build/recomp
"$DOLRECOMP" --cpu broadway --map build/SX3P01.map -j8 build/SX3P01/main_hooked.dol SX3P01 build/recomp
